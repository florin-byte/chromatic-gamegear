/* cf_fota_test.c: cf_fota.c (the FPGA boot flash's update layout "rf": headers, recovery design, two full designs,
 * the game area) against a model of the 4 MiB NOR (a program only clears bits, an erase sets a 64 KiB block to FF)
 * and of the GW5A's boot scan as measured on units 1 and 2 (fpgacomp1/NOTES.md): it scans up from 0 for a sync
 * (A5 C3; A5 CB is taken too) in the first 2 KiB, reads the header after it and boots its target if that holds a
 * whole image; there is no fallback after a complete sync. An update is cut at every header byte (and at bit
 * states inside it), at every erase, and at sampled bytes of the design; after each cut the chip must boot the old
 * design or the recovery design, and a second run (as the firmware does at the next power-on) must finish it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../cf_fota.h"

#define FL (4u << 20)
static uint8_t fl[FL];
static int cut_after = -1, n_ops;     /* the power goes at this op (-1: never); every byte program / erase is one */
static uint8_t cut_mask;              /* the bits of a cut byte that did clear */
static int cut_hit;
static int step_cuts;
static int ok_n, bad_n;
static void check(int c, const char *what) { if (c) ok_n++; else { bad_n++; printf("  FAIL %s\n", what); } }

static uint8_t img_old[0x60000], img_new[0x5A000], img_rec[0x50000], img_big[0x70000];
static uint32_t crc_big;
static uint32_t crc_old, crc_new, crc_rec;

static int invariant_failures;
static int referenced(uint32_t sector)
{
    for (uint32_t at = 0; at <= CF_FOTA_FULL_TOP; at += 64) {
        const uint8_t *p = fl + at;
        if (p[34] != 0xA5 || (p[35] != 0xC3 && p[35] != 0xCB)) continue;
        uint32_t t = (uint32_t)p[48] << 24 | (uint32_t)p[49] << 16 | (uint32_t)p[50] << 8 | p[51];
        if (t == sector) return 1;
    }
    return 0;
}
static void chain_invariant(void)
{
    for (uint32_t sector = CF_FOTA_SELECTOR_A; sector <= CF_FOTA_SELECTOR_B; sector += 4096) {
        if (!referenced(sector)) continue;
        int complete = 0;
        for (uint32_t at = sector; at <= sector + CF_FOTA_FULL_TOP; at += 64) {
            uint8_t want[64]; const uint8_t *p = fl + at;
            uint32_t t = (uint32_t)p[48] << 24 | (uint32_t)p[49] << 16 | (uint32_t)p[50] << 8 | p[51];
            cf_fota_header(want, t);
            if (!memcmp(p, want, 64) && cf_fota_slot_of(t) >= 0) complete = 1;
        }
        if (!complete) invariant_failures++;
    }
}
static int cut_now(void) { if (cut_hit) return 1; if (cut_after >= 0 && n_ops == cut_after) { cut_hit = 1; return 2; } n_ops++; return 0; }
static int m_fcrc(void *u, uint32_t a, uint32_t n, uint32_t *crc, int *ff)
{
    (void)u;
    if (cut_hit || a + n > FL) return -1;
    *crc = cf_fota_crc32(0, fl + a, n);
    *ff = 1;
    for (uint32_t i = 0; i < n; i++) if (fl[a + i] != 0xFF) { *ff = 0; break; }
    return 0;
}
static int prog_bytes(uint32_t a, const uint8_t *b, uint32_t n)
{
    if (step_cuts && cut_now()) return -1;
    for (uint32_t i = 0; i < n; i++) {
        if (b[i] == 0xFF) continue;
        const int c = step_cuts ? 0 : cut_now();
        if (c == 1) return -1;
        if (c == 2) { const uint8_t clear = (uint8_t)(fl[a + i] & ~b[i]); fl[a + i] &= (uint8_t)~(clear & cut_mask); return -1; }
        fl[a + i] &= b[i];
        if (a < CF_FOTA_REC_HDR || (a >= CF_FOTA_SELECTOR_A && a < CF_FOTA_GG_BASE)) chain_invariant();
    }
    return 0;
}
static int m_hdr(void *u, uint32_t a, const uint8_t *b, uint32_t n) { (void)u; if (a + n > CF_FOTA_REC_HDR) return -9; return prog_bytes(a, b, n); }
static int n_erase_ops;
static int m_erase(void *u, uint32_t a, uint32_t len)
{
    (void)u;
    if (a < CF_FOTA_GAME_BASE || (len != 0x1000 && len != 0x10000) || (a & (len - 1))) return -9;
    if ((a == CF_FOTA_SELECTOR_A || a == CF_FOTA_SELECTOR_B) && referenced(a)) invariant_failures++;
    n_erase_ops++;
    const int c = cut_now();
    if (c == 1) return -1;
    if (c == 2) { memset(fl + a, 0xFF, len / 2); fl[a + len / 2 + 7] = 0x5A; return -1; }   /* half done, garbage */
    memset(fl + a, 0xFF, len);
    return 0;
}
static int m_read(void *u, uint32_t a, uint8_t *b, uint32_t n) { (void)u; if (cut_hit || a + n > FL) return -1; memcpy(b, fl + a, n); return 0; }
static int m_prog(void *u, uint32_t a, const uint8_t *b, uint32_t n) { (void)u; if (a < CF_FOTA_GAME_BASE || a + n > FL) return -9; return prog_bytes(a, b, n); }
static const cf_fota_host_t H = { NULL, m_fcrc, m_hdr, m_erase, m_prog, m_read };
static const uint8_t *src_img; static uint32_t src_bad_at = 0xFFFFFFFFu;
static int m_src(void *u, uint32_t o, uint8_t *b, uint32_t n)
{
    (void)u;
    memcpy(b, src_img + o, n);
    if (src_bad_at >= o && src_bad_at < o + n) b[src_bad_at - o] ^= 1;
    return 0;
}

/* the chip at power-on: 'O' old full, 'N' new full, 'R' recovery, 0 nothing starts */
static int boot_at(uint32_t sector, int depth)
{
    if (depth > 1) return 0;
    for (uint32_t i = sector; i + 20 <= sector + CF_FOTA_HDR_AREA + 2; i++) {
        if (fl[i] != 0xA5 || (fl[i + 1] != 0xC3 && fl[i + 1] != 0xCB)) continue;
        const uint8_t *b = fl + i + 2;
        const uint16_t c = (uint16_t)(b[16] | b[17] << 8);
        if (c != cf_fota_crc16(b, 16)) return 0;
        const uint32_t t = (uint32_t)b[12] << 24 | (uint32_t)b[13] << 16 | (uint32_t)b[14] << 8 | b[15];
        if (t == CF_FOTA_SELECTOR_A || t == CF_FOTA_SELECTOR_B) return boot_at(t, depth + 1);
        if (t == CF_FOTA_REC_BASE) return !memcmp(fl + t, img_rec, sizeof img_rec) ? 'R' : 0;
        if (t + sizeof img_old <= FL && !memcmp(fl + t, img_old, sizeof img_old)) return 'O';
        if (t + sizeof img_new <= FL && !memcmp(fl + t, img_new, sizeof img_new)) return 'N';
        if (t + sizeof img_big <= FL && !memcmp(fl + t, img_big, sizeof img_big)) return 'B';
        return 0;
    }
    return 0;
}

static int chip_boot(void) { return boot_at(0, 0); }

/* the USB install's layout: recovery header + design, the old full design + its header at 0x780 */
static void usb_layout(void)
{
    memset(fl, 0xFF, FL);
    uint8_t h[64];
    cf_fota_header(h, CF_FOTA_REC_BASE); memcpy(fl + CF_FOTA_REC_HDR, h, 64);
    memcpy(fl + CF_FOTA_REC_BASE, img_rec, sizeof img_rec);
    cf_fota_header(h, cf_fota_full_base(sizeof img_old)); memcpy(fl + CF_FOTA_FULL_TOP, h, 64);
    memcpy(fl + cf_fota_full_base(sizeof img_old), img_old, sizeof img_old);
}
static int resume_flag, pinned_slot = -1;
static uint8_t saved[FL], b4[4096];
static int install(const uint8_t *img, uint32_t size, uint32_t crc) { src_img = img; return cf_fota_install(&H, m_src, NULL, size, crc, pinned_slot >= 0 ? pinned_slot : resume_flag ? CF_FOTA_SLOT_GB : -1, NULL, NULL, b4); }

static void image_prefix(uint8_t *img, uint32_t size)
{
    static const uint8_t sid[10] = { 0xA5, 0xC3, 0x06, 0, 0, 0, 0, 0x01, 0x28, 0x1B };
    static const uint8_t uc[8] = { 0x0A, 0, 0, 0, 0x13, 0x37, 0xD0, 0x0D };
    memset(img, 0xFF, 24); memset(img + 24, 0xDE, 4);
    memcpy(img + 34, sid, sizeof sid); memcpy(img + size - 16, uc, sizeof uc);
}
static uint32_t gg_base(void) { return cf_fota_slot_end(CF_FOTA_SLOT_GG) - sizeof img_big; }
static void two_layout(void)
{
    usb_layout();
    memcpy(fl + gg_base(), img_big, sizeof img_big);
    uint8_t h[64];
    cf_fota_header(h, gg_base()); memcpy(fl + CF_FOTA_FULL_TOP, h, 64);
}

int main(void)
{
    srand(7);
    for (size_t i = 0; i < sizeof img_old; i++) img_old[i] = (uint8_t)rand();
    for (size_t i = 0; i < sizeof img_new; i++) img_new[i] = (uint8_t)rand();
    for (size_t i = 0; i < sizeof img_rec; i++) img_rec[i] = (uint8_t)rand();
    image_prefix(img_old, sizeof img_old); image_prefix(img_new, sizeof img_new);
    crc_old = cf_fota_crc32(0, img_old, sizeof img_old); crc_new = cf_fota_crc32(0, img_new, sizeof img_new);
    crc_rec = cf_fota_crc32(0, img_rec, sizeof img_rec);
    for (size_t i = 0; i < sizeof img_big; i++) img_big[i] = (uint8_t)rand();
    image_prefix(img_big, sizeof img_big);
    crc_big = cf_fota_crc32(0, img_big, sizeof img_big);

    printf("header bytes\n");
    {
        uint8_t h[64];
        cf_fota_header(h, cf_fota_full_base(768531));
        static const uint8_t want[20] = { 0xA5, 0xC3, 0x06, 0, 0, 0, 0, 0x01, 0x28, 0x1B, 0x6E, 0x80, 0, 0, 0x00, 0x34, 0x40, 0x00 };
        check(cf_fota_full_base(768531) == 0x344000 && cf_fota_full_base(0xC0000) == 0x340000, "a full design's place: the top, 4 KiB aligned");
        check(!memcmp(h + 34, want, 18), "header body -> 0x344000");
        check(h[52] | h[53] << 8 ? cf_fota_crc16(h + 36, 16) == (uint16_t)(h[52] | h[53] << 8) : 0, "header crc16");
    }
    printf("layouts\n");
    {
        cf_fota_layout_t L; uint32_t gb = 0, gs = 0;
        memset(fl, 0xFF, FL);
        check(!cf_fota_scan(&H, &L) && L.kind == CF_FL_LEGACY, "blank flash: legacy");
        check(cf_fota_game_region(&L, &gb, &gs) != 0, "legacy: no game area");
        usb_layout();
        check(!cf_fota_scan(&H, &L) && L.kind == CF_FL_RF && L.live_at == 0x780 && L.next_at == 0x740 && L.used == 1 && L.free == 30, "usb layout: rf, live 0x780, 30 free");
        check(chip_boot() == 'O', "usb layout boots the full design");
        check(L.full_base == cf_fota_full_base(sizeof img_old), "the full design's place from its header");
        check(!cf_fota_game_region(&L, &gb, &gs) && gb == 0x05C000 && gb + gs == CF_FOTA_GAME_END, "fixed game area 0x05C000 up to 0x260000");
        check(CF_FOTA_REC_BASE + CF_FOTA_REC_MAX == CF_FOTA_GAME_BASE, "the recovery design's place ends where the game area starts");
        memcpy(fl, fl + 0x780, 64); memset(fl + 0x780, 0xFF, 64);     /* a stock-like flash: something at 0, no recovery */
        memset(fl + CF_FOTA_REC_HDR, 0xFF, 64);
        check(!cf_fota_scan(&H, &L) && L.kind == CF_FL_LEGACY, "no recovery header: legacy");
    }
    printf("image check\n");
    {
        uint8_t f[64], l[64];
        memset(f, 0xFF, 64); memset(f + 24, 0xDE, 4);
        static const uint8_t sid[10] = { 0xA5, 0xC3, 0x06, 0, 0, 0, 0, 0x01, 0x28, 0x1B };
        memcpy(f + 34, sid, 10);
        memset(l, 0xFF, 64); static const uint8_t uc[8] = { 0x0A, 0, 0, 0, 0x13, 0x37, 0xD0, 0x0D }; memcpy(l + 10, uc, 8);
        check(cf_fota_image_ok(f, l, 768531) == 0, "a compressed 18.31 image passes");
        check(cf_fota_image_ok(f, l, 0x0D0001) != 0, "bigger than the slot: refused");
        check(cf_fota_image_ok(f, l, 0x1000) != 0, "too small: refused");
        f[41] = 0x00; check(cf_fota_image_ok(f, l, 768531) != 0, "another chip's ID: refused"); f[41] = 0x01;
        l[14] = 0; check(cf_fota_image_ok(f, l, 768531) != 0, "not this project's user code: refused");
    }
    printf("a whole update\n");
    {
        cf_fota_layout_t L;
        usb_layout();
        check(install(img_new, sizeof img_new, crc_new) == 0, "install ok");
        check(chip_boot() == 'N', "the new design boots");
        check(!cf_fota_scan(&H, &L) && L.kind == CF_FL_RF && L.live_at == 0x740 && L.used == 2 && L.free == 29, "header 0x740 live, 29 free");
        check(install(img_new, sizeof img_new, crc_new) == 0 && !cf_fota_scan(&H, &L) && L.used == 2, "again: nothing written (it is live)");
        check(install(img_old, sizeof img_old, crc_old + 1) == CF_FOTA_E_SRC && chip_boot() == 'N', "a staged copy that does not match: nothing touched");
        src_bad_at = 0x1234;
        check(install(img_old, sizeof img_old, crc_old) == CF_FOTA_E_SRC && chip_boot() == 'N', "a bit flipped in the staged copy: nothing touched");
        src_bad_at = 0xFFFFFFFFu;
        check(install(img_old, 0x0D0000 + 1, crc_old) == CF_FOTA_E_SIZE, "bigger than CF_FOTA_FULL_MAX");
        /* The entire slot is reserved, even when a bigger replacement needs its lower bytes. */
        {
            const uint32_t nb = cf_fota_full_base(sizeof img_big);
            fl[nb + 5] = 0x42;
            check(install(img_big, sizeof img_big, crc_big) == 0 && chip_boot() == 'B', "bigger design uses its reserved slot");
            check(!cf_fota_scan(&H, &L) && L.full_base == nb, "bigger design remains at its slot top");
            check(install(img_new, sizeof img_new, crc_new) == 0 && chip_boot() == 'N', "and a smaller one again");
        }
        int n = 2, e = 0;
        while (!e) { e = install((n & 1) ? img_new : img_old, (n & 1) ? sizeof img_new : sizeof img_old, (n & 1) ? crc_new : crc_old); if (!e) n++; }
        { cf_fota_layout_t Lf; (void)cf_fota_scan(&H, &Lf); check(e == CF_FOTA_E_FULL && Lf.used == 31, "31 headers used: the next update is refused (USB)"); }
        check(chip_boot() == ((n & 1) ? 'O' : 'N') || chip_boot() == 'O' || chip_boot() == 'N', "and the last one still boots");
        check(!cf_fota_scan(&H, &L) && L.kind == CF_FL_RF_FULL && L.free == 0 && L.live_at == 0, "area full, live at 0x000");
    }
    printf("two slots and switches\n");
    {
        cf_fota_layout_t L;
        uint32_t base, gb, gs, gb2, gs2;
        two_layout();
        check(!cf_fota_scan(&H, &L) && cf_fota_slot_of(L.full_base) == CF_FOTA_SLOT_GG && chip_boot() == 'B', "initial GG design boots");
        check(!cf_fota_game_region(&L, &gb, &gs), "game region before switch");
        check(!cf_fota_slot_base(&H, 0, &base) && base == cf_fota_full_base(sizeof img_old), "GB base found by nonblank sector");
        check(!cf_fota_slot_base(&H, 1, &base) && base == gg_base(), "GG base found by nonblank sector");
        check(!cf_fota_switch(&H, &L, 0, b4) && chip_boot() == 'O' && L.free == 29, "GG to GB consumes one place");
        check(!cf_fota_switch(&H, &L, 1, b4) && chip_boot() == 'B' && L.free == 29, "GB to GG consumes one place");
        check(!cf_fota_game_region(&L, &gb2, &gs2) && gb == gb2 && gs == gs2 && gb + gs == CF_FOTA_GAME_END, "switch leaves game area fixed");
        memcpy(saved, fl, FL); n_ops = 0;
        check(!cf_fota_switch(&H, &L, 1, b4) && n_ops == 0 && !memcmp(saved, fl, FL), "switch to live slot changes no byte");
        check(!cf_fota_invalidate(&H, &L) && L.kind == CF_FL_RF_REC && chip_boot() == 'R', "invalidate skips every complete full header");
        check(!cf_fota_game_region(&L, &gb2, &gs2) && gb == gb2 && gs == gs2, "recovery keeps both slots reserved");

        two_layout(); memset(fl + CF_FOTA_GB_BASE, 0xFF, CF_FOTA_FULL_MAX); memcpy(saved, fl, FL);
        check(cf_fota_switch(&H, &L, 0, b4) == CF_FOTA_E_EMPTY && !memcmp(saved, fl, FL), "empty slot refused without writes");
        two_layout(); fl[cf_fota_full_base(sizeof img_old) + 41] ^= 1; memcpy(saved, fl, FL);
        check(cf_fota_switch(&H, &L, 0, b4) == CF_FOTA_E_CHECK && !memcmp(saved, fl, FL), "wrong chip prefix refused without writes");
        two_layout(); fl[cf_fota_full_base(sizeof img_old)] = 0; memcpy(saved, fl, FL);
        check(cf_fota_switch(&H, &L, 0, b4) == CF_FOTA_E_CHECK && !memcmp(saved, fl, FL), "wrong initial FF bytes refused without writes");
        two_layout(); fl[CF_FOTA_GB_BASE] = 0; memcpy(saved, fl, FL);
        check(cf_fota_switch(&H, &L, 0, b4) == CF_FOTA_E_CHECK && !memcmp(saved, fl, FL), "first nonblank sector must be the design prefix");
        two_layout();
        uint8_t h[64]; cf_fota_header(h, gg_base()); memcpy(fl, h, 64); memcpy(saved, fl, FL);
        check(cf_fota_switch(&H, &L, 0, b4) == CF_FOTA_E_FULL && !memcmp(saved, fl, FL), "full header area refused without writes");
        check(!cf_fota_switch(&H, &L, 1, b4) && !memcmp(saved, fl, FL), "same slot succeeds even with no free header");
        for (int slot = 0; slot < 2; slot++) {
            two_layout(); if (!slot) (void)cf_fota_switch(&H, &L, 0, b4);
            memcpy(saved, fl, FL);
            const uint32_t other = cf_fota_slot_end(1 - slot) - CF_FOTA_FULL_MAX;
            check(!install(img_new, sizeof img_new, crc_new) && !memcmp(fl + other, saved + other, CF_FOTA_FULL_MAX) &&
                  !cf_fota_scan(&H, &L) && cf_fota_slot_of(L.full_base) == slot, "automatic install replaces only the live slot");
        }
        two_layout(); memcpy(saved, fl, FL);
        check(cf_fota_switch(&H, &L, -1, b4) == CF_FOTA_E_LAYOUT && !memcmp(saved, fl, FL), "invalid slot refused without writes");
        check(install(img_old, CF_FOTA_FULL_MAX + 1, crc_old) == CF_FOTA_E_SIZE && !memcmp(saved, fl, FL), "oversized install leaves both slots unchanged");
        check(cf_fota_slot_of(CF_FOTA_GAME_END - CF_FOTA_SECTOR) == -1 && cf_fota_slot_of(CF_FOTA_GG_BASE) == 1 &&
              cf_fota_slot_of(CF_FOTA_GB_BASE) == 0 && cf_fota_slot_of(CF_FOTA_FLASH_END) == -1 && !cf_fota_slot_end(2), "slot boundaries");
    }
    printf("switch power cuts at every programmed byte\n");
    {
        int runs = 0, bad = 0, old = 0, next = 0;
        for (int slot = 0; slot < 2; slot++) {
            cf_fota_layout_t L;
            two_layout(); if (slot == 1) (void)cf_fota_switch(&H, &L, 0, b4);
            n_ops = 0; (void)cf_fota_switch(&H, &L, slot, b4); const int total = n_ops;
            for (int k = 0; k < total; k++) for (int mask = 0; mask < 5; mask++) {
                two_layout(); if (slot == 1) (void)cf_fota_switch(&H, &L, 0, b4);
                memcpy(saved, fl, FL);
                cut_after = k; cut_mask = (uint8_t)(mask == 0 ? 0xFF : mask == 1 ? 0x0F : mask == 2 ? 0xF0 : mask == 3 ? 0x55 : 0);
                n_ops = 0; cut_hit = 0; (void)cf_fota_switch(&H, &L, slot, b4);
                cut_after = -1; cut_hit = 0;
                const int boot = chip_boot(), want = slot == 0 ? 'O' : 'B', prev = slot == 0 ? 'B' : 'O';
                if (boot == want) next++; else if (boot == prev) old++; else bad++;
                if (memcmp(fl + CF_FOTA_REC_HDR, saved + CF_FOTA_REC_HDR, CF_FOTA_GAME_END - CF_FOTA_REC_HDR)) bad++;
                if (cf_fota_switch(&H, &L, slot, b4) || chip_boot() != want) bad++;
                runs++;
            }
        }
        printf("  %d cuts: %d old, %d new, %d failures\n", runs, old, next, bad);
        check(!bad && old, "every switch cut boots old or new, preserves images, retry completes");
    }
    printf("install and resume preserve the other slot\n");
    {
        int bad = 0, runs = 0;
        for (int slot = 0; slot < 2; slot++) {
            cf_fota_layout_t L;
            two_layout(); if (slot == 0) (void)cf_fota_switch(&H, &L, 0, b4);
            pinned_slot = slot;
            n_ops = 0; (void)install(img_new, sizeof img_new, crc_new); const int total = n_ops;
            for (int k = 0; k <= total; k += k < 80 || k > total - 80 ? 1 : 1999) {
                two_layout(); if (slot == 0) (void)cf_fota_switch(&H, &L, 0, b4);
                /* Keep several complete headers, including one targeting the other slot. */
                if (slot == 1) { (void)cf_fota_switch(&H, &L, 0, b4); (void)cf_fota_switch(&H, &L, 1, b4); }
                memcpy(saved, fl, FL);
                cut_after = k; cut_mask = 0x33; n_ops = 0; cut_hit = 0;
                (void)install(img_new, sizeof img_new, crc_new);
                cut_after = -1; cut_hit = 0;
                const uint32_t other = cf_fota_slot_end(1 - slot) - CF_FOTA_FULL_MAX;
                if (memcmp(fl + other, saved + other, CF_FOTA_FULL_MAX) || chip_boot() == 0) bad++;
                if (memcmp(fl + CF_FOTA_REC_HDR, saved + CF_FOTA_REC_HDR, CF_FOTA_GAME_END - CF_FOTA_REC_HDR)) bad++;
                resume_flag = 1;
                if (install(img_new, sizeof img_new, crc_new) || chip_boot() != 'N') bad++;
                if (memcmp(fl + other, saved + other, CF_FOTA_FULL_MAX)) bad++;
                resume_flag = 0; runs++;
            }
        }
        pinned_slot = -1;
        printf("  %d install cuts/resumes\n", runs);
        check(!bad, "both slots: install and pinned resume preserve every byte of the other slot and game/recovery areas");
    }
    printf("power cuts\n");
    {
        /* how many ops a whole run takes */
        usb_layout(); cut_after = -1; n_ops = 0; cut_hit = 0;
        (void)install(img_new, sizeof img_new, crc_new);
        const int total = n_ops;
        printf("  %d ops in one update\n", total);
        int runs = 0, bad_boot = 0, not_done = 0, states_r = 0, states_o = 0;
        for (int k = 0; k < total; k++) {
            /* every op of the headers and the erases; every 997th byte of the design + its first and last 64 */
            const int design_from = 40, design_to = total - 140;
            if (k > design_from + 64 && k < design_to - 64 && (k % 997)) continue;
            const int masks = (k < design_from || k > design_to) ? 5 : 1;
            for (int m = 0; m < masks; m++) {
                usb_layout();
                cut_after = k; cut_mask = (uint8_t)(m == 0 ? 0xFF : m == 1 ? 0x0F : m == 2 ? 0xF0 : m == 3 ? 0x55 : 0x00);
                n_ops = 0; cut_hit = 0;
                (void)install(img_new, sizeof img_new, crc_new);
                cut_after = -1; cut_hit = 0;
                const int b = chip_boot();
                if (b == 0) { bad_boot++; if (bad_boot < 5) printf("  cut at op %d mask %02x: NOTHING boots\n", k, cut_mask); }
                if (b == 'R') states_r++;
                if (b == 'O') states_o++;
                /* the next power-on: the firmware finishes the job */
                if (install(img_new, sizeof img_new, crc_new) != 0 || chip_boot() != 'N') not_done++;
                runs++;
            }
        }
        printf("  %d cuts: %d boot the old design, %d the recovery design\n", runs, states_o, states_r);
        check(bad_boot == 0, "every cut boots the old or the recovery design");
        check(not_done == 0, "every cut is finished by the next run");
        check(states_r > 0 && states_o > 0, "both outcomes seen");
    }
    printf("power cuts, a bigger design (the next start resumes in the reserved slot)\n");
    {
        usb_layout(); cut_after = -1; n_ops = 0; cut_hit = 0; resume_flag = 0;
        (void)install(img_big, sizeof img_big, crc_big);
        const int total = n_ops;
        int runs = 0, bad = 0;
        for (int k = 0; k < total; k += (k < 60 || k > total - 160) ? 1 : 1499) {
            usb_layout(); cut_after = k; n_ops = 0; cut_hit = 0; cut_mask = 0x33;
            (void)install(img_big, sizeof img_big, crc_big);
            cut_after = -1; cut_hit = 0;
            const int b = chip_boot();
            if (b != 'O' && b != 'R' && b != 'B') bad++;
            resume_flag = 1;
            if (install(img_big, sizeof img_big, crc_big) != 0 || chip_boot() != 'B') bad++;
            resume_flag = 0;
            runs++;
        }
        printf("  %d cuts\n", runs);
        check(bad == 0, "bigger design: every cut bootable, every one finished by a resume");
    }
    printf("a cut while finishing (two cuts in a row)\n");
    {
        int bad = 0, runs = 0;
        for (int k1 = 0; k1 < 120; k1 += 7) for (int k2 = 0; k2 < 300; k2 += 13) {
            usb_layout();
            cut_after = k1; n_ops = 0; cut_hit = 0; cut_mask = 0x3C; (void)install(img_new, sizeof img_new, crc_new);
            cut_after = k2; n_ops = 0; cut_hit = 0; (void)install(img_new, sizeof img_new, crc_new);
            cut_after = -1; cut_hit = 0;
            if (chip_boot() == 0) bad++;
            if (install(img_new, sizeof img_new, crc_new) != 0 || chip_boot() != 'N') bad++;
            runs++;
        }
        printf("  %d double cuts\n", runs);
        check(bad == 0, "double cuts: always bootable, always finished");
    }

    printf("two-level rotation and every host-step cut\n");
    {
        cf_fota_layout_t L;
        static uint8_t snapshot[FL];
        int runs = 0, bad = 0;
        step_cuts = 1;
        /* Direct migration, ordinary append, A->B rotation, B->A with stale A references. */
        for (int path = 0; path < 4; path++) {
            two_layout();
            int count = path == 0 ? 0 : path == 1 ? 1 : path == 2 ? 31 : 62;
            for (int j = 0; j < count; j++) if (cf_fota_switch(&H, &L, j & 1, b4)) bad++;
            (void)cf_fota_scan(&H, &L);
            memcpy(snapshot, fl, FL);
            const int prev = chip_boot(), slot = count & 1;
            n_ops = 0;
            if (cf_fota_switch(&H, &L, slot, b4)) bad++;
            int total = n_ops;
            for (int k = 0; k <= total; k++) {
                memcpy(fl, snapshot, FL);
                cut_after = k; cut_hit = 0; n_ops = 0;
                (void)cf_fota_switch(&H, &L, slot, b4);
                cut_after = -1; cut_hit = 0;
                int boot = chip_boot();
                if (boot != prev && boot != (slot ? 'B' : 'O')) bad++;
                if (cf_fota_switch(&H, &L, slot, b4) || chip_boot() != (slot ? 'B' : 'O')) bad++;
                runs++;
            }
        }
        two_layout();
        for (int j = 0; j < 700; j++) {
            int free_before;
            (void)cf_fota_scan(&H, &L); free_before = L.free;
            if (cf_fota_switch(&H, &L, j & 1, b4) || chip_boot() != ((j & 1) ? 'B' : 'O')) bad++;
            if (j == 0 && (L.selector != CF_FOTA_SELECTOR_A || L.l2_free != 30)) bad++;
            if (j > 0 && j < 31 && L.free != free_before) bad++;
            if (j == 31 && (L.selector != CF_FOTA_SELECTOR_B || L.free != free_before - 1)) bad++;
        }
        check(!bad && !invariant_failures, "700 alternating switches, rotation and all step cuts preserve both invariants");
        printf("  %d switch step cuts/resumes, invariant failures %d\n", runs, invariant_failures);
        /* A full level-1 area still permits level-2 appends, but refuses rotation unchanged. */
        two_layout(); (void)cf_fota_switch(&H, &L, 0, b4);
        uint8_t h[64]; cf_fota_header(h, L.selector); memcpy(fl, h, 64);
        check(!cf_fota_switch(&H, &L, 1, b4) && L.free == 0, "level-2 switch with no level-1 places");
        memset(fl + L.selector, 0, L.l2_next - L.selector + CF_FOTA_HDR_STEP);
        memcpy(snapshot, fl, FL);
        check(cf_fota_switch(&H, &L, 0, b4) == CF_FOTA_E_FULL && !memcmp(snapshot, fl, FL), "rotation without level-1 place writes nothing");
        /* Chained install with a free selector, and with a full selector. */
        for (int full = 0; full < 2; full++) {
            two_layout();
            int count = full ? 31 : 1;
            for (int j = 0; j < count; j++) (void)cf_fota_switch(&H, &L, j & 1, b4);
            pinned_slot = 0;
            memcpy(snapshot, fl, FL); n_ops = 0;
            if (install(img_new, sizeof img_new, crc_new)) bad++;
            int total = n_ops;
            runs = 0;
            for (int k = 0; k <= total; k++) {
                memcpy(fl, snapshot, FL); cut_after = k; cut_hit = 0; n_ops = 0;
                (void)install(img_new, sizeof img_new, crc_new);
                cut_after = -1; cut_hit = 0;
                if (!chip_boot()) bad++;
                if (install(img_new, sizeof img_new, crc_new) || chip_boot() != 'N') bad++;
                if (cf_fota_scan(&H, &L) || !L.selector) bad++;
                runs++;
            }
            printf("  %d chained install step cuts/resumes (%s selector)\n", runs, full ? "full" : "free");
        }
        pinned_slot = -1; step_cuts = 0;
        check(!bad && !invariant_failures, "every chained install step cut resumes and retains the chain");
    }
    printf("guards\n");
    {
        uint8_t b[64]; memset(b, 0, 64);
        check(m_hdr(NULL, CF_FOTA_REC_HDR, b, 64) != 0, "(model) the recovery header place is out of the header op's reach");
    }
    printf("\ncf_fota_test: %d ok, %d failed\n", ok_n, bad_n);
    return bad_n ? 1 : 0;
}
