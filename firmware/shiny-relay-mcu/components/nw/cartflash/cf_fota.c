/* cf_fota.c: see cf_fota.h (the FPGA boot flash's update layout "rf": headers, recovery design, full design, games). */
#include <string.h>

#include "cf_fota.h"

/* Gowin's MSPI JUMP header (evt1_x2_jump.bin, V1.9.12.04, 24-bit, single): FF x24, DE x4, FF x6, the sync A5 C3,
   the ID check 06 00 00 00 00 01 28 1B, the jump 6E 80 00 00 + address (u32 BE), CRC-16/ARC of the 16 bytes after
   the sync (little-endian) */
static const uint8_t IDCHK[8] = { 0x06, 0x00, 0x00, 0x00, 0x00, 0x01, 0x28, 0x1B };

uint16_t cf_fota_crc16(const uint8_t *b, uint32_t n)
{
    uint16_t c = 0;
    for (uint32_t i = 0; i < n; i++) {
        c ^= b[i];
        for (int k = 0; k < 8; k++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0xA001u) : (uint16_t)(c >> 1);
    }
    return c;
}

uint32_t cf_fota_crc32(uint32_t c, const uint8_t *b, uint32_t n)
{
    c = ~c;
    for (uint32_t i = 0; i < n; i++) {
        c ^= b[i];
        for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return ~c;
}

void cf_fota_header(uint8_t out[64], uint32_t target)
{
    memset(out, 0xFF, 64);
    memset(out + 24, 0xDE, 4);
    out[CF_FOTA_SYNC_AT] = 0xA5;
    out[CF_FOTA_SYNC_AT + 1] = 0xC3;
    uint8_t *b = out + CF_FOTA_SYNC_AT + 2;
    memcpy(b, IDCHK, 8);
    b[8] = 0x6E; b[9] = 0x80; b[10] = 0; b[11] = 0;
    b[12] = (uint8_t)(target >> 24); b[13] = (uint8_t)(target >> 16); b[14] = (uint8_t)(target >> 8); b[15] = (uint8_t)target;
    const uint16_t c = cf_fota_crc16(b, 16);
    b[16] = (uint8_t)c; b[17] = (uint8_t)(c >> 8);
}

/* a 64 B place's header body (everything but the sync) is whole: 1, its target in *t */
static int body_ok(const uint8_t *p, uint32_t *t)
{
    uint8_t want[64];
    const uint32_t tg = (uint32_t)p[CF_FOTA_TARGET_AT] << 24 | (uint32_t)p[CF_FOTA_TARGET_AT + 1] << 16 |
                        (uint32_t)p[CF_FOTA_TARGET_AT + 2] << 8 | p[CF_FOTA_TARGET_AT + 3];
    cf_fota_header(want, tg);
    for (int i = 0; i < 64; i++) if (i != CF_FOTA_SYNC_AT && i != CF_FOTA_SYNC_AT + 1 && p[i] != want[i]) return 0;
    *t = tg;
    return 1;
}

static int selector_of(uint32_t t)
{
    return t == CF_FOTA_SELECTOR_A || t == CF_FOTA_SELECTOR_B;
}

/* Resolve both live and invalidated level-1 bodies, so a cut install retains its scheme. */
static int resolve(const cf_fota_host_t *h, cf_fota_layout_t *L, uint32_t target, int live)
{
    L->l1_target = target;
    L->full_base = target;
    if (!selector_of(target)) return !target || cf_fota_slot_of(target) >= 0 ? 0 : CF_FOTA_E_LAYOUT;
    L->selector = target;
    uint8_t p[64];
    uint32_t lowest = CF_FOTA_FULL_TOP + CF_FOTA_HDR_STEP, newest = 0, chosen = 0;
    for (uint32_t at = 0; at <= CF_FOTA_FULL_TOP; at += CF_FOTA_HDR_STEP) {
        if (h->read(h->user, target + at, p, 64)) return CF_FOTA_E_FLASH;
        int ff = 1;
        for (int i = 0; i < 64; i++) if (p[i] != 0xFF) ff = 0;
        if (ff) continue;
        if (at < lowest) lowest = at;
        uint32_t t;
        const int ok = body_ok(p, &t) && cf_fota_slot_of(t) >= 0;
        if (ok && !newest) newest = t;
        if (!chosen && p[34] == 0xA5 && (p[35] == 0xC3 || p[35] == 0xCB)) {
            if (!ok) return CF_FOTA_E_LAYOUT;
            chosen = t;
        }
    }
    L->l2_free = (int)(lowest / CF_FOTA_HDR_STEP);
    L->l2_next = L->l2_free ? target + lowest - CF_FOTA_HDR_STEP : 0;
    L->full_base = chosen ? chosen : newest;
    return live && !chosen ? CF_FOTA_E_LAYOUT : 0;
}

int cf_fota_scan(const cf_fota_host_t *h, cf_fota_layout_t *L)
{
    /* 256 B at a time (4 places): static RAM is short on the unit */
    static uint8_t a[256];
    memset(L, 0, sizeof *L);
    uint8_t rh[64];
    cf_fota_header(rh, CF_FOTA_REC_BASE);
    if (h->read(h->user, CF_FOTA_REC_HDR, a, 64)) return -1;
    if (memcmp(a, rh, 64)) { L->kind = CF_FL_LEGACY; return 0; }
    uint32_t lowest = CF_FOTA_FULL_TOP + CF_FOTA_HDR_STEP, newest_t = 0;
    int have_live = 0, dead_sync = 0;
    for (uint32_t at = 0; at <= CF_FOTA_FULL_TOP; at += CF_FOTA_HDR_STEP) {
        if ((at & 0xFFu) == 0 && h->read(h->user, at, a, 256)) return -1;
        const uint8_t *p = a + (at & 0xFFu);
        int ff = 1;
        for (int i = 0; i < 64; i++) if (p[i] != 0xFF) { ff = 0; break; }
        if (ff) continue;
        L->used++;
        if (at < lowest) lowest = at;
        uint32_t t = 0;
        const int ok = body_ok(p, &t) && t >= CF_FOTA_GAME_BASE && t < CF_FOTA_FLASH_END;
        if (ok && !newest_t) newest_t = t;                       /* the lowest written body is the newest */
        const int sync = p[CF_FOTA_SYNC_AT] == 0xA5 && (p[CF_FOTA_SYNC_AT + 1] == 0xC3 || p[CF_FOTA_SYNC_AT + 1] == 0xCB);
        if (sync && !have_live && !dead_sync) {                  /* the chip boots the first complete sync */
            if (ok) { have_live = 1; L->live_at = at; L->full_base = t; }
            else dead_sync = 1;                                  /* a sync with a bad body: nothing would start */
        }
    }
    if (dead_sync) { L->kind = CF_FL_LEGACY; return 0; }
    L->free = (int)(lowest / CF_FOTA_HDR_STEP);
    L->next_at = L->free ? lowest - CF_FOTA_HDR_STEP : 0;
    if (!have_live) {
        L->kind = CF_FL_RF_REC; L->live_at = CF_FOTA_REC_HDR;
        const int e = resolve(h, L, newest_t, 0);
        if (e == CF_FOTA_E_FLASH) return e;
        if (e) L->kind = CF_FL_LEGACY;
        return 0;
    }
    const int e = resolve(h, L, L->full_base, 1);
    if (e == CF_FOTA_E_FLASH) return e;
    if (e) { L->kind = CF_FL_LEGACY; return 0; }
    L->kind = L->free ? CF_FL_RF : CF_FL_RF_FULL;
    return 0;
}

/* one program step into the header place, then the place read back against want (the whole 64 B) */
static int step(const cf_fota_host_t *h, uint32_t at, const uint8_t *b, const uint8_t *want)
{
    if (at < CF_FOTA_REC_HDR ? h->hdr(h->user, at, b, 64) : h->prog(h->user, at, b, 64)) return -1;
    uint32_t crc = 0;
    int ff = 0;
    if (h->fcrc(h->user, at, 64, &crc, &ff)) return -1;
    return crc == cf_fota_crc32(0, want, 64) ? 0 : -2;
}

int cf_fota_invalidate(const cf_fota_host_t *h, cf_fota_layout_t *L)
{
    for (int k = 0; k <= CF_FOTA_HDRS; k++) {
        if (cf_fota_scan(h, L)) return -1;
        if (L->kind == CF_FL_RF_REC) return 0;
        if (L->kind == CF_FL_LEGACY) return CF_FOTA_E_LAYOUT;
        uint8_t b[64], want[64];
        memset(b, 0xFF, 64);
        b[CF_FOTA_SYNC_AT] = 0; b[CF_FOTA_SYNC_AT + 1] = 0;
        cf_fota_header(want, L->l1_target);
        want[CF_FOTA_SYNC_AT] = 0; want[CF_FOTA_SYNC_AT + 1] = 0;
        const int e = step(h, L->live_at, b, want);
        if (e) return e;
    }
    return -2;
}

static int commit_at(const cf_fota_host_t *h, uint32_t at, uint32_t target)
{
    uint32_t crc = 0;
    int ff = 0;
    if (h->fcrc(h->user, at, 64, &crc, &ff)) return -1;
    if (!ff) return -4;                                               /* the place must still be blank */
    uint8_t full[64], want[64], b[64];
    cf_fota_header(full, target);
    /* 1: everything but the sync */
    memcpy(want, full, 64);
    want[CF_FOTA_SYNC_AT] = 0xFF; want[CF_FOTA_SYNC_AT + 1] = 0xFF;
    int e = step(h, at, want, want);
    /* 2: the sync's second byte, 3: its first (the order the chip skips at every state on the way) */
    if (!e) {
        memset(b, 0xFF, 64); b[CF_FOTA_SYNC_AT + 1] = 0xC3;
        want[CF_FOTA_SYNC_AT + 1] = 0xC3;
        e = step(h, at, b, want);
    }
    if (!e) {
        memset(b, 0xFF, 64); b[CF_FOTA_SYNC_AT] = 0xA5;
        e = step(h, at, b, full);
    }
    return e;
}

int cf_fota_commit(const cf_fota_host_t *h, cf_fota_layout_t *L, uint32_t target)
{
    if (L->kind == CF_FL_LEGACY || L->free <= 0) return CF_FOTA_E_FULL;
    /* E3: never publish a chain until the complete level-2 header was read back. */
    if (selector_of(target)) {
        cf_fota_layout_t check = {0};
        int e = resolve(h, &check, target, 1);
        if (e) return e;
    }
    int e = commit_at(h, L->next_at, target);
    const int e2 = cf_fota_scan(h, L);
    if (e) return e;
    if (e2) return CF_FOTA_E_FLASH;
    return (L->kind == CF_FL_RF || L->kind == CF_FL_RF_FULL) && L->l1_target == target ? 0 : CF_FOTA_E_CHECK;
}

/* Remove every complete reference before erasing an inactive selector. The live header must
   precede all such references; a cut during invalidation therefore leaves the live chain intact. */
static int prepare_selector(const cf_fota_host_t *h, const cf_fota_layout_t *L, uint32_t sector, uint32_t base)
{
    uint8_t p[64], mask[64];
    for (uint32_t at = 0; at <= CF_FOTA_FULL_TOP; at += CF_FOTA_HDR_STEP) {
        if (h->read(h->user, at, p, 64)) return CF_FOTA_E_FLASH;
        uint32_t t;
        if (!body_ok(p, &t) || t != sector || p[34] != 0xA5 || (p[35] != 0xC3 && p[35] != 0xCB)) continue;
        if (L->kind != CF_FL_RF_REC && at <= L->live_at) return CF_FOTA_E_LAYOUT;
        memset(mask, 0xFF, 64); mask[34] = mask[35] = 0;
        p[34] = p[35] = 0;
        int e = step(h, at, mask, p);
        if (e) return e;
    }
    if (h->erase(h->user, sector, CF_FOTA_SECTOR)) return CF_FOTA_E_FLASH;
    return commit_at(h, sector + CF_FOTA_FULL_TOP, base);
}

static int select_design(const cf_fota_host_t *h, cf_fota_layout_t *L, uint32_t base, int install)
{
    if (L->selector && L->l2_free) {
        int e = commit_at(h, L->l2_next, base);
        if (e) return e;
        if (!install) return cf_fota_scan(h, L);
        return cf_fota_commit(h, L, L->selector);
    }
    if (!L->free) return CF_FOTA_E_FULL;
    const uint32_t sector = L->selector == CF_FOTA_SELECTOR_A ? CF_FOTA_SELECTOR_B : CF_FOTA_SELECTOR_A;
    int e = prepare_selector(h, L, sector, base);
    if (e) return e;
    return cf_fota_commit(h, L, sector);
}

static int image_start_ok(const uint8_t first[64])
{
    static const uint8_t SYNC_ID[10] = { 0xA5, 0xC3, 0x06, 0x00, 0x00, 0x00, 0x00, 0x01, 0x28, 0x1B };
    for (int i = 0; i < 24; i++) if (first[i] != 0xFF) return -2;
    return memcmp(first + 34, SYNC_ID, sizeof SYNC_ID) ? -2 : 0;
}

int cf_fota_slot_base(const cf_fota_host_t *h, int slot, uint32_t *base)
{
    const uint32_t end = cf_fota_slot_end(slot);
    if (!end) return CF_FOTA_E_LAYOUT;
    for (uint32_t a = end - CF_FOTA_FULL_MAX; a < end; a += CF_FOTA_SECTOR) {
        uint32_t crc; int ff;
        if (h->fcrc(h->user, a, CF_FOTA_SECTOR, &crc, &ff)) return CF_FOTA_E_FLASH;
        if (!ff) { *base = a; return 0; }
    }
    return CF_FOTA_E_EMPTY;
}

int cf_fota_switch(const cf_fota_host_t *h, cf_fota_layout_t *L, int slot, uint8_t buf[4096])
{
    if (!cf_fota_slot_end(slot)) return CF_FOTA_E_LAYOUT;
    if (cf_fota_scan(h, L)) return CF_FOTA_E_FLASH;
    if (L->kind == CF_FL_LEGACY) return CF_FOTA_E_LAYOUT;
    if (L->kind != CF_FL_RF_REC && cf_fota_slot_of(L->full_base) == slot) return 0;
    if (L->free <= 0 && !(L->selector && L->l2_free)) return CF_FOTA_E_FULL;
    uint32_t base;
    int e = cf_fota_slot_base(h, slot, &base);
    if (e) return e;
    if (h->read(h->user, base, buf, 64)) return CF_FOTA_E_FLASH;
    if (image_start_ok(buf)) return CF_FOTA_E_CHECK;
    /* No size is stored in the slot: the trailing user code cannot be checked reliably. */
    return select_design(h, L, base, 0);
}

int cf_fota_image_ok(const uint8_t first[64], const uint8_t last[64], uint32_t size)
{
    static const uint8_t UCODE[8] = { 0x0A, 0x00, 0x00, 0x00, 0x13, 0x37, 0xD0, 0x0D };   /* user code 1337D00D */
    if (size < CF_FOTA_IMG_MIN || size > CF_FOTA_FULL_MAX) return -1;
    if (image_start_ok(first)) return -2;
    for (int i = 0; i + 8 <= 64; i++) if (!memcmp(last + i, UCODE, 8)) return 0;
    return -3;
}

int cf_fota_install(const cf_fota_host_t *h, cf_fota_src_t src, void *src_u, uint32_t size, uint32_t crc, int slot,
                    void (*progress)(void *u, uint32_t done, uint32_t total), void *pu, uint8_t buf[4096])
{
    if (size < CF_FOTA_IMG_MIN || size > CF_FOTA_FULL_MAX) return CF_FOTA_E_SIZE;
    uint8_t *const b = buf;
    enum { B = 4096 };
    /* the staged copy first: whole and right, else nothing is touched */
    uint32_t c = 0;
    for (uint32_t o = 0; o < size; o += (uint32_t)B) {
        const uint32_t n = size - o < (uint32_t)B ? size - o : (uint32_t)B;
        if (src(src_u, o, b, n)) return CF_FOTA_E_SRC;
        c = cf_fota_crc32(c, b, n);
    }
    if (c != crc) return CF_FOTA_E_SRC;
    cf_fota_layout_t L;
    if (cf_fota_scan(h, &L)) return CF_FOTA_E_FLASH;
    if (L.kind == CF_FL_LEGACY) return CF_FOTA_E_LAYOUT;
    if (slot == -1) slot = L.full_base ? cf_fota_slot_of(L.full_base) : CF_FOTA_SLOT_GB;
    const uint32_t end = cf_fota_slot_end(slot);
    if (!end) return CF_FOTA_E_LAYOUT;
    const uint32_t base = end - ((size + CF_FOTA_SECTOR - 1u) & ~(CF_FOTA_SECTOR - 1u));
    uint32_t got = 0;
    int ff = 0;
    if (L.kind != CF_FL_RF_REC && L.full_base == base) {
        if (h->fcrc(h->user, base, size, &got, &ff)) return CF_FOTA_E_FLASH;
        if (got == crc) return 0;                                    /* this design is live already */
    }
    if (L.free <= 0) return CF_FOTA_E_FULL;                          /* nothing changed: a USB install */
    /* 1: the live full header out of the way: from here the recovery design boots */
    int e = cf_fota_invalidate(h, &L);
    if (e) return e == CF_FOTA_E_LAYOUT ? e : CF_FOTA_E_FLASH;
    /* 2: the full design (unless it is there already, from a cut run) */
    if (h->fcrc(h->user, base, size, &got, &ff)) return CF_FOTA_E_FLASH;
    if (got != crc) {
        /* Erase the reserved slot, including stale prefixes left by a smaller replacement. */
        for (uint32_t a = end - CF_FOTA_FULL_MAX; a < end; ) {
            const uint32_t len = (a & 0xFFFFu) == 0 && a + 0x10000u <= end ? 0x10000u : CF_FOTA_SECTOR;
            if (h->erase(h->user, a, len)) return CF_FOTA_E_FLASH;
            a += len;
        }
        for (uint32_t o = 0; o < size; o += (uint32_t)B) {
            const uint32_t n = size - o < (uint32_t)B ? size - o : (uint32_t)B;
            if (src(src_u, o, b, n)) return CF_FOTA_E_SRC;
            if (h->prog(h->user, base + o, b, n)) return CF_FOTA_E_FLASH;
            if (progress) progress(pu, o + n, size);
        }
        if (h->fcrc(h->user, base, size, &got, &ff)) return CF_FOTA_E_FLASH;
        if (got != crc) return CF_FOTA_E_CHECK;
    }
    /* 3: its header: it boots at the next power-on */
    if (cf_fota_scan(h, &L)) return CF_FOTA_E_FLASH;
    e = L.selector ? select_design(h, &L, base, 1) : cf_fota_commit(h, &L, base);
    return e ? CF_FOTA_E_FLASH : 0;
}
