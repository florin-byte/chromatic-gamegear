/* cf_esp.c: see cf_esp.h. DRAFT (lane cartflash1). */
#include "cf_esp.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cf_sha256.h"
#include "cf_writer_bin.h"
#include "cf_probe_bin.h"
#include "../nw_esp.h"

uint32_t cf_stage_rom_off = 0x210000u;   /* cf_usb.h CF_STAGE_ROM_A until the boot says otherwise */
int cf_stage_hold;

#define BANK 0x4000u

static const uint8_t k_logo[48] = {
    0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
    0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E, 0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
    0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E,
};

/* ---------------------------------------------------------------- helpers */
uint16_t cf_crc16(uint16_t crc, const uint8_t *b, uint32_t n)
{
    while (n--) {
        crc ^= (uint16_t)(*b++ << 8);
        for (int i = 0; i < 8; i++) crc = (uint16_t)((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}

/* cartspeed2: by table (the bit loop cost ~0.4 s per 2 MiB pass at 160 MHz, and an install makes several
   passes: the download, each block's scan, every post). const: in flash, not RAM (the TLS heap gate) */
static const uint32_t k_crc32_tab[256] = {
0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu, 0x076DC419u, 0x706AF48Fu,
0xE963A535u, 0x9E6495A3u, 0x0EDB8832u, 0x79DCB8A4u, 0xE0D5E91Eu, 0x97D2D988u,
0x09B64C2Bu, 0x7EB17CBDu, 0xE7B82D07u, 0x90BF1D91u, 0x1DB71064u, 0x6AB020F2u,
0xF3B97148u, 0x84BE41DEu, 0x1ADAD47Du, 0x6DDDE4EBu, 0xF4D4B551u, 0x83D385C7u,
0x136C9856u, 0x646BA8C0u, 0xFD62F97Au, 0x8A65C9ECu, 0x14015C4Fu, 0x63066CD9u,
0xFA0F3D63u, 0x8D080DF5u, 0x3B6E20C8u, 0x4C69105Eu, 0xD56041E4u, 0xA2677172u,
0x3C03E4D1u, 0x4B04D447u, 0xD20D85FDu, 0xA50AB56Bu, 0x35B5A8FAu, 0x42B2986Cu,
0xDBBBC9D6u, 0xACBCF940u, 0x32D86CE3u, 0x45DF5C75u, 0xDCD60DCFu, 0xABD13D59u,
0x26D930ACu, 0x51DE003Au, 0xC8D75180u, 0xBFD06116u, 0x21B4F4B5u, 0x56B3C423u,
0xCFBA9599u, 0xB8BDA50Fu, 0x2802B89Eu, 0x5F058808u, 0xC60CD9B2u, 0xB10BE924u,
0x2F6F7C87u, 0x58684C11u, 0xC1611DABu, 0xB6662D3Du, 0x76DC4190u, 0x01DB7106u,
0x98D220BCu, 0xEFD5102Au, 0x71B18589u, 0x06B6B51Fu, 0x9FBFE4A5u, 0xE8B8D433u,
0x7807C9A2u, 0x0F00F934u, 0x9609A88Eu, 0xE10E9818u, 0x7F6A0DBBu, 0x086D3D2Du,
0x91646C97u, 0xE6635C01u, 0x6B6B51F4u, 0x1C6C6162u, 0x856530D8u, 0xF262004Eu,
0x6C0695EDu, 0x1B01A57Bu, 0x8208F4C1u, 0xF50FC457u, 0x65B0D9C6u, 0x12B7E950u,
0x8BBEB8EAu, 0xFCB9887Cu, 0x62DD1DDFu, 0x15DA2D49u, 0x8CD37CF3u, 0xFBD44C65u,
0x4DB26158u, 0x3AB551CEu, 0xA3BC0074u, 0xD4BB30E2u, 0x4ADFA541u, 0x3DD895D7u,
0xA4D1C46Du, 0xD3D6F4FBu, 0x4369E96Au, 0x346ED9FCu, 0xAD678846u, 0xDA60B8D0u,
0x44042D73u, 0x33031DE5u, 0xAA0A4C5Fu, 0xDD0D7CC9u, 0x5005713Cu, 0x270241AAu,
0xBE0B1010u, 0xC90C2086u, 0x5768B525u, 0x206F85B3u, 0xB966D409u, 0xCE61E49Fu,
0x5EDEF90Eu, 0x29D9C998u, 0xB0D09822u, 0xC7D7A8B4u, 0x59B33D17u, 0x2EB40D81u,
0xB7BD5C3Bu, 0xC0BA6CADu, 0xEDB88320u, 0x9ABFB3B6u, 0x03B6E20Cu, 0x74B1D29Au,
0xEAD54739u, 0x9DD277AFu, 0x04DB2615u, 0x73DC1683u, 0xE3630B12u, 0x94643B84u,
0x0D6D6A3Eu, 0x7A6A5AA8u, 0xE40ECF0Bu, 0x9309FF9Du, 0x0A00AE27u, 0x7D079EB1u,
0xF00F9344u, 0x8708A3D2u, 0x1E01F268u, 0x6906C2FEu, 0xF762575Du, 0x806567CBu,
0x196C3671u, 0x6E6B06E7u, 0xFED41B76u, 0x89D32BE0u, 0x10DA7A5Au, 0x67DD4ACCu,
0xF9B9DF6Fu, 0x8EBEEFF9u, 0x17B7BE43u, 0x60B08ED5u, 0xD6D6A3E8u, 0xA1D1937Eu,
0x38D8C2C4u, 0x4FDFF252u, 0xD1BB67F1u, 0xA6BC5767u, 0x3FB506DDu, 0x48B2364Bu,
0xD80D2BDAu, 0xAF0A1B4Cu, 0x36034AF6u, 0x41047A60u, 0xDF60EFC3u, 0xA867DF55u,
0x316E8EEFu, 0x4669BE79u, 0xCB61B38Cu, 0xBC66831Au, 0x256FD2A0u, 0x5268E236u,
0xCC0C7795u, 0xBB0B4703u, 0x220216B9u, 0x5505262Fu, 0xC5BA3BBEu, 0xB2BD0B28u,
0x2BB45A92u, 0x5CB36A04u, 0xC2D7FFA7u, 0xB5D0CF31u, 0x2CD99E8Bu, 0x5BDEAE1Du,
0x9B64C2B0u, 0xEC63F226u, 0x756AA39Cu, 0x026D930Au, 0x9C0906A9u, 0xEB0E363Fu,
0x72076785u, 0x05005713u, 0x95BF4A82u, 0xE2B87A14u, 0x7BB12BAEu, 0x0CB61B38u,
0x92D28E9Bu, 0xE5D5BE0Du, 0x7CDCEFB7u, 0x0BDBDF21u, 0x86D3D2D4u, 0xF1D4E242u,
0x68DDB3F8u, 0x1FDA836Eu, 0x81BE16CDu, 0xF6B9265Bu, 0x6FB077E1u, 0x18B74777u,
0x88085AE6u, 0xFF0F6A70u, 0x66063BCAu, 0x11010B5Cu, 0x8F659EFFu, 0xF862AE69u,
0x616BFFD3u, 0x166CCF45u, 0xA00AE278u, 0xD70DD2EEu, 0x4E048354u, 0x3903B3C2u,
0xA7672661u, 0xD06016F7u, 0x4969474Du, 0x3E6E77DBu, 0xAED16A4Au, 0xD9D65ADCu,
0x40DF0B66u, 0x37D83BF0u, 0xA9BCAE53u, 0xDEBB9EC5u, 0x47B2CF7Fu, 0x30B5FFE9u,
0xBDBDF21Cu, 0xCABAC28Au, 0x53B39330u, 0x24B4A3A6u, 0xBAD03605u, 0xCDD70693u,
0x54DE5729u, 0x23D967BFu, 0xB3667A2Eu, 0xC4614AB8u, 0x5D681B02u, 0x2A6F2B94u,
0xB40BBE37u, 0xC30C8EA1u, 0x5A05DF1Bu, 0x2D02EF8Du
};
uint32_t cf_crc32(uint32_t crc, const uint8_t *b, uint32_t n)
{
    crc = ~crc;
    while (n--) crc = k_crc32_tab[(crc ^ *b++) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

int cf_header_ok(const uint8_t h[80])
{
    if (memcmp(h + 4, k_logo, sizeof k_logo)) return 0;
    uint8_t x = 0;
    for (int i = 0x34; i <= 0x4C; i++) x = (uint8_t)(x - h[i] - 1);
    return x == h[0x4D];
}

int cf_image_policy(const uint8_t *h, uint32_t size, uint32_t cap)
{
    (void)h;                                   /* any ROM: the header is not judged */
    if (size < 0x8000u) return CF_D_BAD_IMAGE;
    if (cap && size > cap) return CF_D_BAD_IMAGE;
    return 0;
}

unsigned cf_image_compat(const uint8_t *h)
{
    unsigned w = 0;
    const uint8_t t = h[0x47];
    if (!cf_header_ok(h)) w |= CF_W_HEADER;
    if (!(t == 0x00 || t == 0x08 || t == 0x09 || (t >= 0x19 && t <= 0x1E))) w |= CF_W_MAPPER;
    if (t == 0x0F || t == 0x10) w |= CF_W_RTC;
    if (h[0x49] == 0x04 || h[0x49] == 0x05) w |= CF_W_RAM;       /* 128 KiB / 64 KiB */
    return w;
}

const char *cf_compat_text(unsigned w)
{
    if (w & CF_W_HEADER) return "WILL  NOT  START";
    if (w & CF_W_MAPPER) return "MAY  NOT  RUN";
    if (w & CF_W_RAM) return "SAVE  TOO  BIG";
    if (w & CF_W_RTC) return "NO  CLOCK";
    return "";
}

void cf_title_from_header(const uint8_t *h, char *out)
{
    int n = 0;
    for (int i = 0x34; i < 0x43 && n < 15; i++) {            /* 15 bytes; 0x143 is the CGB flag */
        uint8_t c = h[i];
        if (c == 0) break;
        if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 32);
        if (c == '_') c = ' ';
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '.' || c == '!' || c == '\'')) {
            if (c >= 0x80 && i >= 0x3F) break;               /* a manufacturer code / CGB flag in a newer header */
            c = ' ';
        }
        out[n++] = (char)c;
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    int s = 0;
    while (s < n && out[s] == ' ') s++;
    memmove(out, out + s, (size_t)(n - s));
    n -= s;
    out[n] = 0;
    if (n == 0) memcpy(out, "UNKNOWN GAME", 13);
}

int cf_cfi_regions(const uint8_t cfi[128], cf_region_t *r, int max, uint32_t *size)
{
    int s;
    if (cfi[0x10] == 'Q' && cfi[0x11] == 'R' && cfi[0x12] == 'Y') s = 1;            /* x8 */
    else if (cfi[0x20] == 'Q' && cfi[0x22] == 'R' && cfi[0x24] == 'Y') s = 2;       /* x16, byte mode */
    else return -1;
#define B(w) cfi[(w) * s]
    if (0x30 + 4 * (max - 1) >= 128 / s) max = (128 / s - 0x31) / 4 + 1;
    *size = 1u << B(0x27);
    int n = B(0x2C);
    if (n < 1 || n > max) return -1;
    for (int i = 0; i < n; i++) {
        r[i].count = (uint16_t)((B(0x2D + 4 * i) | (B(0x2E + 4 * i) << 8)) + 1);
        uint32_t z = (uint32_t)(B(0x2F + 4 * i) | (B(0x30 + 4 * i) << 8));
        r[i].size = z ? z * 256u : 128u;
    }
#undef B
    return n;
}

const cf_chip_t *cf_chip_match(const uint8_t id[16])
{
    for (int i = 0; i < cf_n_chips; i++) {
        const cf_chip_t *k = &cf_chips[i];
        if (k->id_len && !memcmp(id, k->id, k->id_len)) return k;
    }
    return NULL;
}

static void logf_(cf_esp_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void logf_(cf_esp_t *c, const char *fmt, ...)
{
    if (!c->h.log) return;
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    c->h.log(c->h.user, b);
}

static void jr_save(cf_esp_t *c)
{
    c->j.magic = CF_JOURNAL_MAGIC;
    c->j.crc = cf_crc32(0, (const uint8_t *)&c->j, (uint32_t)offsetof(cf_journal_t, crc));
    if (c->h.jr_save) c->h.jr_save(c->h.user, &c->j);
}

static void jr_load(cf_esp_t *c)
{
    memset(&c->j, 0, sizeof c->j);
    if (!c->h.jr_load || c->h.jr_load(c->h.user, &c->j) != 0 || c->j.magic != CF_JOURNAL_MAGIC ||
        c->j.crc != cf_crc32(0, (const uint8_t *)&c->j, (uint32_t)offsetof(cf_journal_t, crc))) {
        memset(&c->j, 0, sizeof c->j);
        c->j.magic = CF_JOURNAL_MAGIC;
    }
}

static void status(cf_esp_t *c, const char *what, uint32_t done, uint32_t total)
{
    c->st.phase = (uint8_t)c->ph;
    c->st.job = c->j.job;
    c->st.done_bytes = done;
    c->st.total_bytes = total;
    c->st.nsec = (uint16_t)c->plan_n;
    snprintf(c->st.text, sizeof c->st.text, "%s %u%%", what, total ? (unsigned)((uint64_t)done * 100 / total) : 0u);
    if (c->h.progress) c->h.progress(c->h.user, &c->st);
}

/* sector i of the chip: start, size */
static void sec_geom(const cf_esp_t *c, int i, uint32_t *start, uint32_t *size)
{
    uint32_t a = 0;
    for (int r = 0; r < c->nreg; r++) {
        if (i < c->reg[r].count) { *start = a + (uint32_t)i * c->reg[r].size; *size = c->reg[r].size; return; }
        i -= c->reg[r].count;
        a += (uint32_t)c->reg[r].count * c->reg[r].size;
    }
    *start = a; *size = 0;
}

static int plan_sector(const cf_esp_t *c, int oi) { return oi + 1 < c->plan_n ? oi + 1 : 0; }   /* header sector last */
static int done_get(const cf_esp_t *c, int s) { return (c->j.done[s >> 3] >> (s & 7)) & 1; }
static void done_set(cf_esp_t *c, int s, int v)
{
    if (v) c->j.done[s >> 3] |= (uint8_t)(1u << (s & 7));
    else c->j.done[s >> 3] &= (uint8_t)~(1u << (s & 7));
}

/* the bytes being written: the staged image (INSTALL) or the backup (RESTORE); FF past the end */
static int tgt_read(cf_esp_t *c, uint32_t off, uint8_t *b, uint32_t n)
{
    memset(b, 0xFF, n);
    if (off >= c->j.tgt_size) return 0;
    if (off + n > c->j.tgt_size) n = c->j.tgt_size - off;
    if (c->j.job == CF_JOB_RESTORE || c->j.job == CF_JOB_ROUNDTRIP) return c->h.bak_read(c->h.user, off, b, n);
    return c->h.img_read(c->h.user, off, b, n);
}

static int tgt_all_ff(cf_esp_t *c, uint32_t off, uint32_t n, uint16_t *crc)
{
    uint8_t b[512];
    int ff = 1;
    uint16_t x = 0xFFFF;
    while (n) {
        uint32_t k = n < sizeof b ? n : sizeof b;
        tgt_read(c, off, b, k);
        for (uint32_t i = 0; i < k; i++) if (b[i] != 0xFF) ff = 0;
        x = cf_crc16(x, b, k);
        off += k; n -= k;
    }
    if (crc) *crc = x;
    return ff;
}

/* ---------------------------------------------------------------- commands */
static void cmd_flash(cf_esp_t *c, uint8_t cmd, uint32_t f, uint16_t len)
{
    memset(&c->last, 0, sizeof c->last);
    c->last.cmd = cmd;
    c->last.mbc = CP_MBC5;
    c->last.bank = (uint16_t)(f / BANK);
    c->last.addr = (uint16_t)(0x4000u + (f % BANK));
    c->last.len = len;
    if (c->chip) {
        c->last.cmdset = c->chip->cmdset;
        c->last.op = c->chip->erase_op;
        c->last.tmo = c->chip->erase_tmo;
        c->last.cfi_mode = c->chip->cfi_mode;
        c->last.cfi_addr = c->chip->cfi_addr;
    }
}

#define CF_RAMDUMP_CHUNK 128u
#define CF_RAMTEST_OFF 0x2000u          /* RAM bank 1, first KiB */
static uint8_t ramtest_pattern(uint32_t i) { return (uint8_t)((i * 29u + 0xA5u) ^ (i >> 4)); }
static void cmd_ram(cf_esp_t *c, uint8_t cmd, uint32_t off, uint16_t len)
{
    memset(&c->last, 0, sizeof c->last);
    c->last.cmd = cmd;
    c->last.mbc = CP_MBC5;
    c->last.flags = CF_F_RAM;
    c->last.bank = (uint16_t)(off / 0x2000u);
    c->last.addr = (uint16_t)(0xA000u + off % 0x2000u);
    c->last.len = len;
}

static void cmd_done(cf_esp_t *c, uint8_t code)
{
    memset(&c->last, 0, sizeof c->last);
    c->last.cmd = CF_C_DONE;
    c->last.mbc = CP_MBC5;
    c->last.op = code;
    c->ph = CF_PH_DONE;
    c->st.result = code;
    c->done_pending = 1;
    c->done_at = c->now;
}

static uint16_t chunk_in_bank(uint32_t f, uint32_t end, uint32_t max)
{
    uint32_t n = end - f;
    if (n > max) n = max;
    if (n > BANK - f % BANK) n = BANK - f % BANK;
    return (uint16_t)n;
}

/* a job ends; code CF_D_*. Before the first erase the cart is untouched: phase IDLE. */
static void finish(cf_esp_t *c, uint8_t code)
{
    c->j.result = code;
    if (code == CF_D_OK) {
        c->j.phase = CF_PH_IDLE;
        c->j.job = CF_JOB_NONE;
    } else if (c->j.phase == CF_PH_WRITE || c->j.phase == CF_PH_FAILED) {
        c->j.phase = CF_PH_FAILED;       /* cart in flux: the OSD offers retry / restore */
    } else {
        c->j.job = CF_JOB_NONE;
    }
    jr_save(c);
    free(c->tbuf); c->tbuf = NULL;
    free(c->sbuf); c->sbuf = NULL;
    logf_(c, "cf: job ends, code %u (cart %s)", code,
          c->j.phase == CF_PH_FAILED ? (c->j.job == CF_JOB_STREAM ? "NOT FINISHED: it has no first block; resume or cancel" : "IN FLUX: retry or restore") : "consistent");
    cmd_done(c, code);
}

/* ---------------------------------------------------------------- WP TEST (1.0.2)
 * The SST39VF1681's WP# pin, held low, protects its BOTTOM 64 KiB (DS25040A): there every erase and
 * program is ignored. That block holds the start of every game and the header, so it cannot be
 * planned around (the 1682's protected TOP block is). Nobody knows whether a 1681 Tetris board
 * holds WP# low (likely not: Cart Clinic rewrites Tetris on these carts). So before the FIRST
 * erase of any job that writes the flash, one bit of one byte of that block is programmed and read
 * back. The block is erased next anyway, so the changed bit never outlives the job:
 *   scan     the block is read top down, 1 KiB at a time; the byte chosen is the highest one that
 *            ends a run of 16 FF bytes (padding: a power cut before the erase leaves the game as it
 *            ran), else the highest FF, else the highest byte with any 1 bit outside the header
 *            (0x100-0x14F), else one in the header; a block of only 00 bytes cannot be tested:
 *            refused (CF_D_LOCKED), nothing changed
 *   read     that byte again (each byte read twice by the writer): v
 *   program  v with its lowest 1 bit cleared (v & (v - 1): never FF, so the writer programs it)
 *   read     v & (v - 1): the block takes writes, the job goes on to its first erase;
 *            v: the chip ignored the program (WP#): refused with CF_D_LOCKED, the cart byte for
 *            byte as it was; anything else (or a block that does not read back): CF_D_WPTEST,
 *            nothing erased (never CF_D_FAILED, which tells the page the cart is in flux)
 * It runs after the verified-backup gate (INSTALL / RESTORE / ROUNDTRIP: the backup is complete),
 * after the power check, and before the journal says "in flux": a refusal leaves the journal idle.
 * A STREAM runs it after its data channel check (the image's first bytes arrived), right before
 * it erases block 0. A resumed write skips it (its earlier session passed it, or it would never
 * have begun erasing). Save-only, backup and read jobs never run it: they program no flash. */
static int wp_needed(const cf_esp_t *c) { return c->chip && c->chip->protect_bottom >= 1024u && !c->wp_done && !c->resume; }
static void wp_begin(cf_esp_t *c)
{
    c->wp_step = 0; c->wp_tries = 0; c->wp_prog_res = 0;
    c->wp_scan = c->chip->protect_bottom;
    c->wp_ff = c->wp_nz = c->wp_hdr = 0;
    logf_(c, "cf: write test: does the first %u KiB block of the %s take a write? (one bit, before anything is erased)",
          (unsigned)(c->chip->protect_bottom / 1024u), c->chip->part);
}
static int wp_hdr_byte(uint32_t a) { return a >= 0x100u && a < 0x150u; }
/* the next command of the test: 0 issued (return), 1 passed, -1 the job was finished */
static int wp_next(cf_esp_t *c)
{
    switch (c->wp_step) {
    case 0:
        cmd_flash(c, CF_C_READ, c->wp_scan - 1024u, 1024);
        return 0;
    case 1:
    case 3:
        cmd_flash(c, CF_C_READ, c->wp_at, 1);
        return 0;
    case 2:
        cmd_flash(c, CF_C_PROGRAM, c->wp_at, 1);
        c->last.src = 9;
        return 0;
    default:
        c->wp_done = 1;
        return 1;
    }
}
/* a result of the test: -1 the job was finished, else 0 (the caller asks wp_next) */
static int wp_result(cf_esp_t *c, uint8_t res, const uint8_t *r, uint16_t n)
{
    if (c->last.cmd == CF_C_PROGRAM) { c->wp_prog_res = res; c->wp_step = 3; c->wp_tries = 0; return 0; }
    if (res != CF_R_OK || n != c->last.len) {
        if (++c->wp_tries > 3) {
            logf_(c, "cf: write test: the first block does not read back at %06x (result %u): stopped, nothing was erased",
                  (unsigned)(c->wp_step ? c->wp_at : c->wp_scan - 1024u), res);
            finish(c, CF_D_WPTEST);
            return -1;
        }
        return 0;                                   /* the same read again */
    }
    c->wp_tries = 0;
    if (c->wp_step == 0) {
        const uint32_t base = c->wp_scan - 1024u;
        for (int i = 1023; i >= 0; i--) {
            const uint32_t a = base + (uint32_t)i;
            if (wp_hdr_byte(a)) { if (r[i] && !c->wp_hdr) c->wp_hdr = a + 1; continue; }
            if (r[i] == 0xFF) {
                if (!c->wp_ff) c->wp_ff = a + 1;
                int run = i >= 15;
                for (int k = 1; run && k < 16; k++) if (r[i - k] != 0xFF || wp_hdr_byte(a - (uint32_t)k)) run = 0;
                if (run) { c->wp_at = a; c->wp_step = 1; return 0; }
            } else if (r[i] && !c->wp_nz) c->wp_nz = a + 1;
        }
        c->wp_scan = base;
        if (c->wp_scan) return 0;                   /* the next KiB down */
        const uint32_t a1 = c->wp_ff ? c->wp_ff : c->wp_nz ? c->wp_nz : c->wp_hdr;
        if (!a1) {
            logf_(c, "cf: write test: every byte of the first block is 00, so no bit can be cleared to test it: refused, nothing was changed");
            finish(c, CF_D_LOCKED);
            return -1;
        }
        c->wp_at = a1 - 1; c->wp_step = 1;
        return 0;
    }
    if (c->wp_step == 1) {
        c->wp_v = r[0];
        if (!c->wp_v) {                             /* the scan saw a 1 bit here: the cart reads differently now */
            logf_(c, "cf: write test: the byte at %06x now reads 00: stopped, nothing was erased", (unsigned)c->wp_at);
            finish(c, CF_D_WPTEST);
            return -1;
        }
        c->wp_nv = (uint8_t)(c->wp_v & (c->wp_v - 1u));
        c->wp_step = 2;
        return 0;
    }
    const uint8_t got = r[0];                       /* step 3: the read back */
    if (got == c->wp_nv) {
        logf_(c, "cf: write test passed: the first block took a write (%06x: %02x -> %02x); it is erased next", (unsigned)c->wp_at, c->wp_v, got);
        c->wp_step = 4;
        return 0;
    }
    if (got == c->wp_v) {
        logf_(c, "cf: write test: the first %u KiB block ignored a write (%06x stays %02x, result %u): write-protected (WP#). Refused, nothing erased or written",
              (unsigned)(c->chip->protect_bottom / 1024u), (unsigned)c->wp_at, got, c->wp_prog_res);
        finish(c, CF_D_LOCKED);
        return -1;
    }
    logf_(c, "cf: write test: %06x read %02x after %02x -> %02x: stopped, nothing was erased", (unsigned)c->wp_at, got, c->wp_v, c->wp_nv);
    finish(c, CF_D_WPTEST);
    return -1;
}

/* ---------------------------------------------------------------- planning */
static void plan_next(cf_esp_t *c);
static void write_plan_go(cf_esp_t *c, int same);

static void begin_write_plan(cf_esp_t *c)
{
    c->ph = CF_PH_PLAN;
    uint8_t hdr[80];
    uint32_t size;
    if (c->j.job == CF_JOB_RESTORE || c->j.job == CF_JOB_ROUNDTRIP) {
        if (!c->j.bak_ok) { finish(c, CF_D_NO_BACKUP); return; }
        size = c->j.bak_rom_size;
        c->h.bak_read(c->h.user, 0x100, hdr, 80);
    } else {
        size = c->h.img_size;
        if (!size || c->h.img_read(c->h.user, 0x100, hdr, 80) != 0) { finish(c, CF_D_BAD_IMAGE); return; }
        /* any ROM, as long as it fits what this chip lets us write (the 1682's top block is protected) */
        if (cf_image_policy(hdr, size, c->chip ? c->chip->size - c->chip->protect_top : c->j.chip_size)) {
            logf_(c, "cf: the image (%u bytes) does not fit this cart's writable flash (%u bytes)", (unsigned)size,
                  (unsigned)(c->chip ? c->chip->size - c->chip->protect_top : c->j.chip_size));
            finish(c, CF_D_BAD_IMAGE);
            return;
        }
        {
            const unsigned w = cf_image_compat(hdr);
            char t[17];
            cf_title_from_header(hdr, t);
            logf_(c, "cf: image \"%s\": cart type %02x, rom code %02x, ram code %02x%s%s%s%s", t, hdr[0x47], hdr[0x48], hdr[0x49],
                  w & CF_W_HEADER ? "; HEADER INVALID (the console will not start it)" : "",
                  w & CF_W_MAPPER ? "; not an MBC5 / ROM-only game (this cart's mapper is MBC5-like: it may not run)" : "",
                  w & CF_W_RAM ? "; wants more than this cart's 32 KiB of save RAM" : "",
                  w & CF_W_RTC ? "; a clock cartridge (no clock on this cart)" : "");
        }
        if (!c->j.bak_ok) { finish(c, CF_D_NO_BACKUP); return; }
    }
    if (size > c->j.chip_size) { finish(c, CF_D_BAD_IMAGE); return; }
    /* crc32 of the target, to recognise a resumed job */
    uint32_t crc = 0;
    uint8_t b[512];
    const uint32_t save_size = c->j.tgt_size;
    const uint8_t save_job = c->j.job;
    c->j.tgt_size = size;
    for (uint32_t o = 0; o < size; o += sizeof b) {
        uint32_t k = size - o < sizeof b ? size - o : sizeof b;
        tgt_read(c, o, b, k);
        crc = cf_crc32(crc, b, k);
    }
    (void)save_job;
    const int same = c->resume && save_size == size && c->j.tgt_crc32 == crc;
    if (!same) memset(c->j.done, 0, sizeof c->j.done);
    c->j.tgt_crc32 = crc;
    memcpy(c->j.tgt_hdr, hdr, 80);
    c->plan_n = 0;
    for (int i = 0; i < c->nsec; i++) {
        uint32_t s0, sz;
        sec_geom(c, i, &s0, &sz);
        /* the WP#-protected top block (SST39VF1682) is never planned; a protected BOTTOM block
           (SST39VF1681 with WP# low) is found by the write test below, before anything is erased */
        if (s0 < size && s0 + sz <= c->chip->size - c->chip->protect_top) c->plan_n = i + 1;
    }
    if (!c->h.power_ok || !c->h.power_ok(c->h.user)) { finish(c, CF_D_ABORTED); return; }
    if (wp_needed(c)) {                  /* 1.0.2: after the backup gate + the power check, before "in flux" */
        wp_begin(c);
        c->ph = CF_PH_WPTEST;
        plan_next(c);
        return;
    }
    write_plan_go(c, same);
}

static void write_plan_go(cf_esp_t *c, int same)
{
    c->j.phase = CF_PH_WRITE;            /* from here the cart may be in flux */
    c->j.verify_pass = 0;
    c->j.nsec = (uint16_t)c->plan_n;
    jr_save(c);
    logf_(c, "cf: plan %d sectors for %u bytes (%s), %s", c->plan_n, (unsigned)c->j.tgt_size,
          c->j.job == CF_JOB_RESTORE ? "restore" : c->j.job == CF_JOB_ROUNDTRIP ? "round trip" : "install", same ? "resuming" : "fresh");
    c->ph = CF_PH_WRITE;
    c->oi = 0; c->step = 0; c->pos = 0; c->retry = 0; c->piece_mismatch = 0;
    plan_next(c);
}

static void begin_selftest(cf_esp_t *c);
static void after_backup(cf_esp_t *c)
{
    if (c->j.job == CF_JOB_BACKUP) { finish(c, CF_D_OK); return; }
    if (c->j.job == CF_JOB_SELFTEST) { begin_selftest(c); return; }
    begin_write_plan(c);
}

/* the next command for the current phase (c->last) */
static void selftest_next(cf_esp_t *c);

/* ---------------------------------------------------------------- CF_JOB_STREAM (2026-10-11)
 * An image goes STRAIGHT from its source (the gateway over Wi-Fi, or the owner's computer over
 * USB) to the cart: nothing is staged or backed up on the ESP32 (owner ruling). Order:
 *   TOP      an image longer than the writable flash: the cart's protected top block must read FF
 *   ERASE0   the journal says "in flux", then block 0 (the header) is ERASED FIRST: from here to
 *            the very end the cart has no logo, so the console never starts a half-written cart
 *   BLOCKS   blocks 1..n-1: erase, blank check, program chunk by chunk from the source (the writer
 *            compares every chunk); a finished block is journalled, so a resume asks the source
 *            only for what is missing (Range)
 *   TAIL     blocks past the image, up to the writable end: erased if not blank
 *   HASH     the whole image: block 0 from the source (hashed, not stored), the rest READ BACK
 *            from the cart; crc32 (+ sha256 when given) must be the record's
 *   RAMCLR   the save RAM to 00 (the game and its save are replaced)
 *   BLK0     block 0 from the source again, its first chunk (the header) LAST; then block 0 is
 *            read back and must have the crc32 of the bytes that were hashed
 * A cut or a failed check leaves the journal pending: the same image resumes, another is refused
 * until this one is cancelled. */
enum { SP_TOP, SP_ERASE0, SP_BLOCKS, SP_TAIL, SP_HASH0, SP_HASHCART, SP_RAMCLR, SP_BLK0, SP_BLK0CHK,
       SP_WPTEST /* 1.0.2: the bottom block's write test, between the channel check and the erase of block 0 */ };
#define CF_STREAM_WAIT_MS 300000u      /* the source silent this long: pause (resume later) */

static uint32_t strm_writable(const cf_esp_t *c) { return c->chip->size - c->chip->protect_top; }
static void strm_wait(cf_esp_t *c) { memset(&c->last, 0, sizeof c->last); c->last.cmd = CF_C_WAIT; }
static int src_gate(cf_esp_t *c);
static void stream_next(cf_esp_t *c);

/* The second look at the FPGA's overlay state, before a job starts changing the cart from a
   computer's data (USB restore of the ROM or the save). 1 = go; 0 = not yet (the writer was
   told to wait, the caller returns) or refused (the job is finished). */
static int ovl_write_ok(cf_esp_t *c)
{
    if (!c->h.overlay_state || c->ovl_ok != 3) return 1;
    const int st = c->h.overlay_state(c->h.user, 0);
    if (st == 0) { c->ovl_ok = 2; return 1; }
    if (st < 0 && c->now - c->ovl_t0 <= 3000u) { strm_wait(c); return 0; }
    logf_(c, "cf: USB write refused: the FPGA %s", st == 1 ? "reports its patch overlay ON" : "gave no second report of its patch overlay");
    finish(c, CF_D_OVERLAY);
    return 0;
}

static void begin_stream(cf_esp_t *c)
{
    const uint32_t size = c->j.strm_size;
    if (size < 0x8000u || size > c->chip->size || !c->h.chunk) {
        logf_(c, "cf: stream: %u bytes do not fit this cart (%u writable of %u)", (unsigned)size, (unsigned)strm_writable(c), (unsigned)c->chip->size);
        finish(c, CF_D_BAD_IMAGE);
        return;
    }
    free(c->sbuf);
    c->sbuf = malloc(CF_PROG_CHUNK);
    if (!c->sbuf) { finish(c, CF_D_ABORTED); return; }
    if (!c->opt.strm_chunk || c->opt.strm_chunk > CF_PROG_CHUNK) c->opt.strm_chunk = CF_PROG_CHUNK;
    c->s_wr_end = size < strm_writable(c) ? size : strm_writable(c);
    c->s_nblk = 0;
    for (int i = 0; i < c->nsec; i++) { uint32_t s0, sz; sec_geom(c, i, &s0, &sz); if (s0 < c->s_wr_end) c->s_nblk = i + 1; }
    if (!c->resume) memset(c->j.done, 0, sizeof c->j.done);
    int left = 0;
    for (int i = 1; i < c->s_nblk; i++) if (!done_get(c, i)) left++;
    logf_(c, "cf: stream \"%s\": %u bytes from %s, %d blocks (%d to write%s), crc32 %08x", c->j.strm_title, (unsigned)size,
          c->j.strm_src == 2 ? "the staged image" : c->j.strm_src == CF_SRC_WIFI_STAGED ? "the downloaded image (staged)" : c->j.strm_src ? "USB" : "the gateway", c->s_nblk, left + 1, c->resume ? ", resuming" : "", (unsigned)c->j.strm_crc32);
    c->ph = CF_PH_STREAM;
    c->s_have = 0; c->s_wait_t0 = 0; c->retry = 0; c->step = 0; c->pos = 0;
    c->s_probed = c->resume ? 1 : 0;      /* a resumed write has no first block already: nothing left to protect */
    c->sp = size > strm_writable(c) ? SP_TOP : SP_ERASE0;
    free(c->xs); c->xs = NULL;
    /* cartspeed1: a staged image on an FPGA with the express stream goes the express way */
    if ((c->j.strm_src == 2 || c->j.strm_src == CF_SRC_WIFI_STAGED) && !c->opt.xs_off && c->h.xs_avail && c->h.xs_send && c->h.xs_ring &&
        c->h.xs_avail(c->h.user) && c->chip->cmdset == CF_CS_AMD_AAA) {
        c->xs = (struct cf_xs *)calloc(1, sizeof *c->xs);
        if (c->xs) c->xs->on = 1;
        else logf_(c, "cf: stream: no memory for the express stream: the classic route");
    }
    else if (c->j.strm_src == 2 || c->j.strm_src == CF_SRC_WIFI_STAGED) logf_(c, "cf: stream: the classic route (%s)", c->opt.xs_off ? "asked for" : "this FPGA has no express stream");
    stream_next(c);
}

/* the chunk at c->pos (n bytes) into sbuf: 1 have it, 0 a WAIT / finish was issued (return) */
static int strm_chunk(cf_esp_t *c, uint16_t n)
{
    if (c->s_have) return 1;
    const int r = c->h.chunk(c->h.user, c->pos, n, c->sbuf);
    if (r > 0) { c->s_have = 1; c->s_wait_t0 = 0; return 1; }
    if (r < 0) { logf_(c, "cf: stream: the source no longer has this image"); finish(c, CF_D_GONE); return 0; }
    if (!c->s_wait_t0) c->s_wait_t0 = c->now ? c->now : 1;
    if (c->now - c->s_wait_t0 > CF_STREAM_WAIT_MS) {
        logf_(c, "cf: stream: no data for %u s at %06x: paused (it resumes where it stopped)", (unsigned)(CF_STREAM_WAIT_MS / 1000u), (unsigned)c->pos);
        finish(c, CF_D_PAUSED);
        return 0;
    }
    strm_wait(c);
    return 0;
}
static int all_ff(const uint8_t *b, uint32_t n) { for (uint32_t i = 0; i < n; i++) if (b[i] != 0xFF) return 0; return 1; }
/* The piece of block 0 that is written LAST: it holds the entry point and the whole header
   (0x100-0x14F), so a cart without it never starts. Fixed, whatever chunk size the source uses
   (USB chunks are 96 bytes: the first one alone would leave the header to the earlier step). */
#define CF_STRM_HEAD 512u
/* Before the cart is touched, the image's first CF_STRM_PROBE bytes must ARRIVE (each chunk
   checked by its source: crc per USB frame / line, the length per gateway answer). Unit 1,
   2026-10-11: a USB restore erased block 0 and then could not get a single data line through;
   the cart was left without a game. A channel that does not work now ends the job with the
   cart as it was. (They are asked for again when they are written.) */
#define CF_STRM_PROBE 2048u
static uint16_t strm_n(const cf_esp_t *c, uint32_t end) { return chunk_in_bank(c->pos, end, c->opt.strm_chunk); }
static int strm_fast(const cf_esp_t *c) { return c->opt.fast_verify && (c->j.strm_src == 2 || c->j.strm_src == CF_SRC_WIFI_STAGED); }
/* fast verify: CRC16 (as the writer's CRC command: CCITT, init FFFF) of n staged bytes at off.
   The staged source answers at once. 0 = it did not. */
static int strm_src_crc16(cf_esp_t *c, uint32_t off, uint32_t n, uint16_t *out)
{
    uint16_t x = 0xFFFF;
    while (n) {
        const uint16_t k = (uint16_t)(n > CF_PROG_CHUNK ? CF_PROG_CHUNK : n);
        if (c->h.chunk(c->h.user, off, k, c->sbuf) <= 0) return 0;
        x = cf_crc16(x, c->sbuf, k);
        off += k; n -= k;
    }
    c->s_have = 0;
    *out = x;
    return 1;
}
/* one bank of the cart against the staged data, by CRC16 on the Game Boy side */
static int strm_crc_bank(cf_esp_t *c, uint32_t end)
{
    const uint16_t n = chunk_in_bank(c->pos, end, BANK);
    if (!strm_src_crc16(c, c->pos, n, &c->s_want16)) { logf_(c, "cf: stream: the staged image cannot be read"); finish(c, CF_D_GONE); return 0; }
    cmd_flash(c, CF_C_CRC, c->pos, n);
    return 1;
}

static void xs_next(cf_esp_t *c);
static void xs_begin(cf_esp_t *c);
static void stream_next(cf_esp_t *c)
{
    uint32_t s0, sz;
    if (c->j.strm_src != 0 && !ovl_write_ok(c)) return;
    for (;;) {
        /* cartspeed1: the express stream owns these phases (the writer waits) */
        if (c->xs && c->xs->on && c->xs->st != 0 /* XS_IDLE */) { xs_next(c); return; }
        switch (c->sp) {
        case SP_TOP:                       /* nothing written yet: a refusal here leaves the cart as it was */
            if (c->pos == 0) c->pos = strm_writable(c);
            if (c->pos >= c->chip->size) { c->sp = SP_ERASE0; c->step = 0; c->pos = 0; continue; }
            cmd_flash(c, CF_C_BLANK, c->pos, chunk_in_bank(c->pos, c->chip->size, BANK));
            return;
        case SP_ERASE0:
            sec_geom(c, 0, &s0, &sz);
            if (c->step == 0 && !c->s_probed) {
                const uint32_t pe = c->j.strm_size < CF_STRM_PROBE ? c->j.strm_size : CF_STRM_PROBE;
                if (c->pos < pe) {
                    const uint16_t n = strm_n(c, pe);
                    if (!strm_chunk(c, n)) return;           /* waits; 5 minutes without it: the job ends, cart untouched */
                    c->pos += n; c->s_have = 0;
                    continue;
                }
                c->s_probed = 1; c->pos = 0;
                logf_(c, "cf: stream: the first %u bytes arrived: the channel works, the write starts", (unsigned)pe);
            }
            if (c->step == 0 && wp_needed(c)) { wp_begin(c); c->sp = SP_WPTEST; continue; }
            if (c->step == 0 && c->xs && c->xs->on) { c->sp = SP_BLOCKS; c->pos = 0; xs_begin(c); xs_next(c); return; }
            if (c->step == 0) {
                c->j.phase = CF_PH_WRITE;                /* from here the cart is in flux */
                jr_save(c);
                cmd_flash(c, CF_C_ERASE, 0, 0); c->n_erase++;
                return;
            }
            if (c->pos >= sz) { c->sp = SP_BLOCKS; c->s_blk = 1; c->step = 0; c->pos = 0; c->retry = 0; continue; }
            cmd_flash(c, CF_C_BLANK, c->pos, chunk_in_bank(c->pos, sz, BANK));
            return;
        case SP_BLOCKS: {
            if (c->s_blk >= c->s_nblk) { c->sp = SP_TAIL; c->step = 0; c->pos = 0; continue; }
            if (done_get(c, c->s_blk)) { c->s_blk++; c->step = 0; continue; }
            sec_geom(c, c->s_blk, &s0, &sz);
            const uint32_t end = s0 + sz < c->s_wr_end ? s0 + sz : c->s_wr_end;
            if (c->step == 0) { c->s_have = 0; cmd_flash(c, CF_C_ERASE, s0, 0); c->n_erase++; return; }
            if (c->step == 1) {
                if (c->pos < s0) c->pos = s0;
                if (c->pos >= s0 + sz) { c->step = 2; c->pos = s0; continue; }
                cmd_flash(c, CF_C_BLANK, c->pos, chunk_in_bank(c->pos, s0 + sz, BANK));
                return;
            }
            if (c->pos >= end) {
                done_set(c, c->s_blk, 1);
                jr_save(c);
                c->s_blk++; c->step = 0; c->pos = 0; c->retry = 0;
                status(c, "WRITE", (uint32_t)c->s_blk * sz, c->s_wr_end);
                continue;
            }
            const uint16_t n = strm_n(c, end);
            if (!strm_chunk(c, n)) return;
            if (all_ff(c->sbuf, n)) { c->pos += n; c->s_have = 0; continue; }
            cmd_flash(c, CF_C_PROGRAM, c->pos, n);
            c->last.src = 8;
            return;
        }
        case SP_TAIL:                      /* the cart ends up holding the image and nothing else */
            if (c->s_blk < c->s_nblk) c->s_blk = c->s_nblk;
            sec_geom(c, c->s_blk, &s0, &sz);
            if (c->s_blk >= c->nsec || !sz || s0 >= strm_writable(c)) { c->sp = SP_HASH0; c->step = 0; c->pos = 0; continue; }
            if (c->step == 1) { cmd_flash(c, CF_C_ERASE, s0, 0); c->n_erase++; return; }
            if (c->pos < s0) c->pos = s0;
            if (c->pos >= s0 + sz) { c->s_blk++; c->pos = 0; continue; }
            cmd_flash(c, CF_C_BLANK, c->pos, chunk_in_bank(c->pos, s0 + sz, BANK));
            return;
        case SP_HASH0: {
            sec_geom(c, 0, &s0, &sz);
            const uint32_t end = sz < c->s_wr_end ? sz : c->s_wr_end;
            if (c->step == 0) { cf_sha256_init((cf_sha256_t *)&c->s_sha); c->s_crc = 0; c->s_crc0 = 0; c->step = 1; c->s_have = 0; }
            if (c->pos >= end) { c->sp = SP_HASHCART; c->pos = end; c->bank_retry = 0; continue; }
            const uint16_t n = strm_n(c, end);
            if (!strm_chunk(c, n)) return;
            cf_sha256_update((cf_sha256_t *)&c->s_sha, c->sbuf, n);
            c->s_crc = cf_crc32(c->s_crc, c->sbuf, n);
            c->s_crc0 = cf_crc32(c->s_crc0, c->sbuf, n);
            c->pos += n; c->s_have = 0;
            continue;
        }
        case SP_HASHCART:
            if (c->pos >= c->j.strm_size) {                 /* cartspeed2: the image checked before the save RAM and block 0 */
                const int g = src_gate(c);
                if (g > 0) {
                    if (!c->s_wait_t0) c->s_wait_t0 = c->now ? c->now : 1;
                    if (c->now - c->s_wait_t0 <= CF_STREAM_WAIT_MS) { strm_wait(c); return; }
                    logf_(c, "cf: stream: the image's check did not finish: paused (it resumes where it stopped)");
                    finish(c, CF_D_PAUSED);
                    return;
                }
                c->s_wait_t0 = 0;
                if (g < 0) {
                    logf_(c, "cf: stream: the staged image did not check out (crc32 / sha256): the first block stays empty");
                    memset(c->j.done, 0, sizeof c->j.done);
                    c->j.strm_tries++;
                    finish(c, CF_D_VERIFY);
                    return;
                }
            }
            if (c->pos >= c->j.strm_size && strm_fast(c)) {
                logf_(c, "cf: stream: every bank of the cart has the staged image's CRC16 (fast verify): writing the first block");
                c->sp = SP_RAMCLR; c->pos = c->opt.ram_clear ? 0 : CF_RAM_SIZE; c->bank_retry = 0;
                continue;
            }
            if (c->pos >= c->j.strm_size) {
                uint8_t d[32];
                cf_sha256_final((cf_sha256_t *)&c->s_sha, d);
                const int sha_ok = !c->opt.strm_have_sha || !memcmp(d, c->j.strm_sha, 32);
                if (c->s_crc != c->j.strm_crc32 || !sha_ok) {
                    logf_(c, "cf: stream: the cart does NOT hold the image (crc32 %08x, wanted %08x; sha256 %s): the first block stays empty",
                          (unsigned)c->s_crc, (unsigned)c->j.strm_crc32, sha_ok ? "ok" : "differs");
                    memset(c->j.done, 0, sizeof c->j.done);      /* a resume writes all of it again */
                    c->j.strm_tries++;
                    finish(c, CF_D_VERIFY);
                    return;
                }
                logf_(c, "cf: stream: the cart read back has the image's crc32%s: writing the first block", c->opt.strm_have_sha ? " and sha256" : "");
                c->sp = SP_RAMCLR; c->pos = c->opt.ram_clear ? 0 : CF_RAM_SIZE; c->bank_retry = 0;
                continue;
            }
            if (strm_fast(c)) { (void)strm_crc_bank(c, c->j.strm_size); return; }
            cmd_flash(c, CF_C_READ, c->pos, chunk_in_bank(c->pos, c->j.strm_size, 1024));
            return;
        case SP_RAMCLR:
            if (c->pos == 0 && !c->bank_retry) c->n_sav_fail = 0;
            if (c->pos >= CF_RAM_SIZE) { c->sp = SP_BLK0; c->step = 0; c->pos = CF_STRM_HEAD; c->s_have = 0; c->retry = 0; continue; }
            cmd_ram(c, CF_C_RAMWRITE, c->pos, 1024);
            c->last.src = c->opt.ram_clear == 2 ? 5 : 3;     /* 5: the staged save (sav_read), 3: zeros */
            c->last.src_off = c->pos;
            return;
        case SP_BLK0: {                    /* everything but the first chunk, then the first chunk (the header) */
            sec_geom(c, 0, &s0, &sz);
            const uint32_t end = sz < c->s_wr_end ? sz : c->s_wr_end;
            if (c->step == 2) { cmd_flash(c, CF_C_ERASE, 0, 0); c->n_erase++; return; }        /* a failed chunk: start block 0 again */
            if (c->step == 0 && c->pos >= end) { c->step = 1; c->pos = 0; c->s_have = 0; c->s_entry = 0; }
            /* the head: every piece but the one with the entry point (0x100-0x103), then that
               one, the very last bytes of the whole install. Until it is there the cart runs FF. */
            if (c->step == 1 && c->pos >= CF_STRM_HEAD) { c->step = 3; c->pos = c->s_entry ? c->s_entry - 1 : CF_STRM_HEAD; c->s_have = 0; }
            if (c->step == 3 && (!c->s_entry || c->pos != c->s_entry - 1)) { c->sp = SP_BLK0CHK; c->step = 0; c->pos = 0; c->s_crc0_cart = 0; c->bank_retry = 0; continue; }
            const uint16_t n = strm_n(c, c->step == 0 ? end : CF_STRM_HEAD);
            if (c->step == 1 && c->pos <= 0x100u && c->pos + n > 0x100u) { c->s_entry = c->pos + 1; c->pos += n; c->s_have = 0; continue; }
            if (!strm_chunk(c, n)) return;
            if (all_ff(c->sbuf, n)) { c->pos += n; c->s_have = 0; continue; }
            cmd_flash(c, CF_C_PROGRAM, c->pos, n);
            c->last.src = 8;
            return;
        }
        case SP_BLK0CHK: {
            sec_geom(c, 0, &s0, &sz);
            const uint32_t end = sz < c->s_wr_end ? sz : c->s_wr_end;
            if (c->step == 1) { cmd_flash(c, CF_C_ERASE, 0, 0); c->n_erase++; return; }        /* wrong: never leave it startable */
            if (c->pos >= end) {
                if (!strm_fast(c) && c->s_crc0_cart != c->s_crc0) {
                    logf_(c, "cf: stream: the first block read back differs (crc32 %08x, wanted %08x): erased again", (unsigned)c->s_crc0_cart, (unsigned)c->s_crc0);
                    c->step = 1;
                    continue;
                }
                logf_(c, "cf: stream: \"%s\" is on the cart, %u bytes, verified", c->j.strm_title, (unsigned)c->j.strm_size);
                status(c, "DONE", 1, 1);
                finish(c, CF_D_OK);
                return;
            }
            if (strm_fast(c)) { (void)strm_crc_bank(c, end); return; }
            cmd_flash(c, CF_C_READ, c->pos, chunk_in_bank(c->pos, end, 1024));
            return;
        }
        case SP_WPTEST: {
            const int w = wp_next(c);
            if (w <= 0) return;                  /* a command, or the job was finished */
            c->sp = SP_ERASE0; c->step = 0; c->pos = 0;
            continue;
        }
        default:
            finish(c, CF_D_FAILED);
            return;
        }
    }
}

/* ---------------------------------------------------------------- EXPRESS (cartspeed1, FPGA 18.26+)
 * A staged image (USB or Wi-Fi: already checked by crc32 + sha256) is written by the FPGA itself:
 * the bytes go over the QSPI bus into the FPGA's 4 KiB ring, the ops and posts over the UART, and
 * the FPGA's express stream (cart_xs.v) erases, programs (every byte read back and compared by the
 * FPGA) and reads blocks back for their crc32. The writer only waits meanwhile (WAIT). Nothing is
 * stop-and-wait: up to XS_MAXQ ops are in the FPGA's queue, the ring is refilled as the status
 * says bytes are consumed, posts follow one another without waiting for their answers.
 *
 * Order (the cart never starts half written):
 *   SYNC   ABORT, RESET, CFG (timing, session on): the FPGA's stream from zero
 *   E0     the journal says "in flux"; block 0 erased FIRST, read back: all FF
 *   BLOCKS block b = 1..n-1 not yet done: ERASE b, PROG its non-FF range, CRC b = the crc32 of
 *          the staged bytes (FF past the image): journalled done
 *   TAIL   blocks past the image up to the writable end: CRC; one not all FF is erased + read back
 *   RAM    the safe bus timing; the save RAM 1 KiB at a time: RAMW (zeros, or the staged save) and
 *          RAMCRC; a KiB that does not read back is counted (the note after the job), not retried
 *   B0     block 0 from 0x104 on, then 0..0xFF, then the four entry bytes 0x100-0x103 LAST;
 *          CRC of block 0 = the staged bytes; else block 0 is erased again (never left startable)
 * Any error or a crc32 that is not the staged one: ABORT, the block again from SYNC (the safe
 * timing from then on); after max_retries + 1 such restarts the job ends unfinished (resume later:
 * the done blocks are journalled). */
enum { XS_IDLE, XS_SYNC, XS_SYNCW, XS_E0, XS_BLOCKS, XS_TAIL, XS_TAILW, XS_RAM, XS_RAMW, XS_B0SYNC, XS_B0SYNCW, XS_B0, XS_B0W, XS_B0BAD, XS_DONE };
enum { XK_NONE, XK_BLANK0, XK_VERIFY, XK_TAILCHK, XK_TAILV, XK_BLK0V, XK_SYNC, XK_OFF, XK_B0ERASE, XK_RAMV, XK_TAILE /* fix: erase it, then TAILV */,
       XK_RECHK /* cartspeed2: a block done before this resume, read back against the staged bytes */ };
#define CF_XS_O_RAMW   5u
#define CF_XS_O_RAMCRC 6u
#define XS_MAXQ 3                 /* ops sent and not finished: the FPGA runs one and queues two */
#define XS_Q 4
#define XS_FIX 8
#define XS_ASK_MS 300u
#define XS_DEAD_MS 15000u

static const uint8_t k_xs_timing[3][5] = { { 4, 7, 4, 7, 4 }, { 2, 4, 2, 4, 2 }, { 1, 2, 1, 3, 1 } };

static void xs_pkt(cf_esp_t *c, const uint8_t *p, uint8_t n) { if (c->h.xs_send) c->h.xs_send(c->h.user, p, n); }
static void xs_simple(cf_esp_t *c, uint8_t t) { xs_pkt(c, &t, 1); }
static int xs_room(const cf_esp_t *c) { return c->xs->qn < XS_MAXQ; }
/* cartspeed2: an image still arriving (img_avail) or still being checked (img_gate) */
static uint32_t src_avail(cf_esp_t *c) { return c->h.img_avail ? c->h.img_avail(c->h.user) : 0xFFFFFFFFu; }
static int src_gate(cf_esp_t *c) { return c->h.img_gate ? c->h.img_gate(c->h.user) : 0; }
/* the staged image is not the image: what was written from it is void (a resume writes it all again) */
static void xs_void(cf_esp_t *c)
{
    logf_(c, "cf: express: the staged image did not check out (crc32 / sha256): the blocks written from it are void, the first block stays empty");
    xs_simple(c, CF_XS_P_ABORT);
    xs_simple(c, CF_XS_P_RESET);
    c->xs->st = XS_DONE;
    memset(c->j.done, 0, sizeof c->j.done);
    c->j.strm_tries++;
    finish(c, CF_D_VERIFY);
}
static void xs_waited(cf_esp_t *c)
{
    struct cf_xs *x = c->xs;
    if (x->t_wait) { x->ms_wait += c->now - x->t_wait; x->t_wait = 0; }
}
/* 1 = wait (the planner comes back), 0 = go on, -1 = the job ended */
static int xs_wait_tick(cf_esp_t *c, const char *what)
{
    struct cf_xs *x = c->xs;
    if (!x->t_wait) x->t_wait = c->now ? c->now : 1;
    if (c->now - x->t_wait > CF_STREAM_WAIT_MS) {
        logf_(c, "cf: express: %s for %u s: paused (it resumes where it stopped)", what, (unsigned)(CF_STREAM_WAIT_MS / 1000u));
        xs_simple(c, CF_XS_P_ABORT);
        xs_simple(c, CF_XS_P_RESET);
        x->st = XS_DONE;
        finish(c, CF_D_PAUSED);
        return -1;
    }
    return 1;
}
/* bytes [0, need) of the image in the staging area? */
static int xs_need(cf_esp_t *c, uint32_t need)
{
    if (need > c->j.strm_size) need = c->j.strm_size;
    const int g = src_gate(c);
    if (g < 0) { xs_void(c); return -1; }
    if (g == 0 || src_avail(c) >= need) { xs_waited(c); return 0; }
    return xs_wait_tick(c, "the image did not arrive");
}
/* the whole image there and checked (before the save RAM and block 0)? */
static int xs_gate(cf_esp_t *c)
{
    const int g = src_gate(c);
    if (g < 0) { xs_void(c); return -1; }
    if (g == 0) {
        if (c->xs->t_wait) { c->xs->ms_gate += c->now - c->xs->t_wait; c->xs->t_wait = 0; }
        return 0;
    }
    return xs_wait_tick(c, "the image's check did not finish");
}
/* one op into the FPGA's queue */
static void xs_op(cf_esp_t *c, uint8_t op, uint8_t kind, int blk, uint32_t f, uint32_t n, uint8_t arg, uint32_t want)
{
    /* RAMW data: blk = 1 zeros, 2 the staged save (sav_read) */
    struct cf_xs *x = c->xs;
    x->seq = (uint8_t)(x->seq == 255 ? 1 : x->seq + 1);
    const int i = (x->qh + x->qn) % XS_Q;
    x->q[i].seq = x->seq; x->q[i].op = op; x->q[i].kind = kind; x->q[i].blk = (int16_t)blk;
    x->q[i].f = f; x->q[i].n = n; x->q[i].want = want; x->q[i].pos0 = x->pos_next;
    x->qn++;
    if (op == CF_XS_O_PROG || op == CF_XS_O_RAMW) x->pos_next += n;
    uint8_t p[10] = { CF_XS_P_OP, x->seq, op, (uint8_t)f, (uint8_t)(f >> 8), (uint8_t)(f >> 16),
                      (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), arg };
    xs_pkt(c, p, 10);
    if (!x->t_ask) x->t_ask = c->now ? c->now : 1;
}
static void xs_cfg(cf_esp_t *c, uint8_t kind, int on)
{
    const uint8_t *t = k_xs_timing[on ? (c->xs->timing < 3 ? c->xs->timing : 0) : 0];
    xs_op(c, CF_XS_O_CFG, kind, -1, (uint32_t)t[0] | (uint32_t)t[1] << 8 | (uint32_t)t[2] << 16,
          (uint32_t)t[3] | (uint32_t)t[4] << 8 | (uint32_t)(on ? 1 : 0) << 16, 0, 0);
    if (kind == XK_SYNC || kind == XK_OFF) { c->xs->sync_seq = c->xs->seq; c->xs->t_sync = c->now ? c->now : 1; }
}
/* crc32 of the staged bytes of [s0, s0 + sz) (FF past the image), and their non-FF range */
static void xs_pump(cf_esp_t *c);
/* (the scan reads into sbuf[0, 1 KiB), the ring is fed from sbuf[1 KiB, 2 KiB) between its pieces:
   a 64 KiB scan takes tens of ms, the 4 KiB ring lasts ~40 ms of programming) */
#define XS_FFRUN 512u      /* an FF run this long splits a block's PROG (the FPGA never pulls it through the ring:
                              unit 2, Tetris: its FF runs held the ring's read port and every 3rd post was refused) */
static int xs_scan(cf_esp_t *c, uint32_t s0, uint32_t sz, uint32_t *crc, uint32_t *nz0, uint32_t *nz1)
{
    struct cf_xs *sx = c->xs;
    sx->nseg = 0;
    uint32_t seg_s = 0xFFFFFFFFu, last_nz = 0;
    uint32_t x = 0, a = 0xFFFFFFFFu, b = 0, xi = 0;
    for (uint32_t o = s0; o < s0 + sz; ) {
        const uint16_t k = (uint16_t)(s0 + sz - o > CF_XS_CHUNK ? CF_XS_CHUNK : s0 + sz - o);
        if (o >= c->j.strm_size) memset(c->sbuf, 0xFF, k);
        else {
            const uint16_t m = (uint16_t)(c->j.strm_size - o < k ? c->j.strm_size - o : k);
            if (c->h.chunk(c->h.user, o, m, c->sbuf) <= 0) return -1;
            xi = cf_crc32(xi, c->sbuf, m);
            if (m < k) memset(c->sbuf + m, 0xFF, k - m);
        }
        x = cf_crc32(x, c->sbuf, k);
        for (uint16_t i = 0; i < k; i++) if (c->sbuf[i] != 0xFF) {
            const uint32_t at = o + i;
            if (a == 0xFFFFFFFFu) a = at;
            b = at + 1;
            if (seg_s == 0xFFFFFFFFu) seg_s = at;
            else if (at - last_nz > XS_FFRUN) {             /* a long FF run ends a segment */
                if (sx->nseg < XS_SEG) { sx->seg[sx->nseg][0] = seg_s; sx->seg[sx->nseg][1] = last_nz + 1; sx->nseg++; seg_s = at; }
            }
            last_nz = at;
        }
        o += k;
        xs_pump(c);
        if (!c->sbuf || !c->armed || !c->xs || c->xs->st == XS_DONE) return -1;   /* the pump ended the job */
    }
    c->s_have = 0;
    if (seg_s != 0xFFFFFFFFu) {                             /* the last segment (the table full: it runs to the end) */
        if (sx->nseg < XS_SEG) { sx->seg[sx->nseg][0] = seg_s; sx->seg[sx->nseg][1] = last_nz + 1; sx->nseg++; }
        else sx->seg[XS_SEG - 1][1] = last_nz + 1;
    }
    *crc = x; *nz0 = a; *nz1 = b;
    sx->wimg = xi;
    return 0;
}
/* cartspeed2: the block just scanned against the crc32 the source took of the same bytes as they came
   (img_blk_crc): 0 the same (or not known), -1 not: the staging area does not hold the image (void) */
static int xs_blk_src(cf_esp_t *c, uint32_t s0, uint32_t sz)
{
    uint32_t want = 0;
    if (!c->h.img_blk_crc || c->h.img_blk_crc(c->h.user, s0, sz, &want) != 1 || want == c->xs->wimg) return 0;
    logf_(c, "cf: express: the staged block at %06x reads crc32 %08x, the download had %08x", (unsigned)s0, (unsigned)c->xs->wimg, (unsigned)want);
    xs_void(c);
    return -1;
}

static void xs_restart(cf_esp_t *c, const char *why)
{
    struct cf_xs *x = c->xs;
    xs_simple(c, CF_XS_P_ABORT);
    x->n_resync++;
    if (++x->retries > (uint16_t)(c->opt.max_retries + 1) * 2u) {
        logf_(c, "cf: express: %s; %u restarts: stopped (the done blocks are kept: resume)", why, (unsigned)x->retries);
        xs_simple(c, CF_XS_P_RESET);
        x->st = XS_DONE;
        finish(c, CF_D_FAILED);
        return;
    }
    if (x->timing) { logf_(c, "cf: express: %s: the safe bus timing from here", why); x->timing = 0; }
    else logf_(c, "cf: express: %s: from a fresh start", why);
    c->n_retries++;
    x->qn = 0; x->fixn = 0; x->pstep = 0;
    /* where it was: block 0 last (B0) restarts B0 (its erase first); else the blocks / tail again */
    /* block 0's own phase restarts itself; anything earlier from SYNC (done blocks are skipped, the
       tail and the save RAM are cheap to do again) */
    x->st = (x->st >= XS_B0SYNC && x->st <= XS_B0BAD) ? XS_B0SYNC : XS_SYNC;
    if (x->st == XS_B0SYNC) x->b0_step = 0;
}

static void xs_begin(cf_esp_t *c)
{
    struct cf_xs *x = c->xs;
    memset(x, 0, sizeof *x);
    x->on = 1; x->st = XS_SYNC; x->timing = c->opt.xs_timing < 3 ? c->opt.xs_timing : 0;
    x->t_start = c->now; x->naks_seen = 0xFF;
    x->seq = (uint8_t)(1u + (c->now ^ (c->now >> 8)) % 200u);   /* not the seq an earlier session left in the FPGA */
    logf_(c, "cf: express stream: the FPGA writes the cart (bus timing %u)", (unsigned)x->timing);
}

/* the ring: QSPI write + post, as far as the status lets */
static void xs_pump(cf_esp_t *c)
{
    struct cf_xs *x = c->xs;
    uint8_t *const buf = c->sbuf + CF_XS_CHUNK;   /* sbuf is CF_PROG_CHUNK (2 KiB): the second half */
    for (int k = 0; k < 4 && x->have; k++) {
        if (x->feed >= x->pos_next || x->feed >= x->cons + CF_XS_RING) return;
        /* the PROG op that holds stream position feed */
        int qi = -1;
        for (int i = 0; i < x->qn; i++) {
            const int j = (x->qh + i) % XS_Q;
            if ((x->q[j].op == CF_XS_O_PROG || x->q[j].op == CF_XS_O_RAMW) && x->feed >= x->q[j].pos0 && x->feed < x->q[j].pos0 + x->q[j].n) { qi = j; break; }
        }
        if (qi < 0) return;
        uint32_t len = CF_XS_CHUNK - x->feed % CF_XS_CHUNK;
        const uint32_t opend = x->q[qi].pos0 + x->q[qi].n;
        if (x->feed + len > opend) len = opend - x->feed;
        const uint32_t qlen = (len + 3u) & ~3u;              /* the QSPI write in whole 32-bit words (see below) */
        if (x->feed + qlen > x->cons + CF_XS_RING) return;   /* whole chunks only: the next KiB consumed opens one */
        const uint32_t off = x->q[qi].f + (x->feed - x->q[qi].pos0);
        if (x->q[qi].op == CF_XS_O_RAMW) {
            if (x->q[qi].blk == 2 && c->h.sav_read) { if (c->h.sav_read(c->h.user, off, buf, len)) memset(buf, 0, len); }
            else memset(buf, 0, len);
            goto have;
        }
        uint32_t m = len;
        if (off >= c->j.strm_size) m = 0; else if (off + m > c->j.strm_size) m = c->j.strm_size - off;
        if (m && c->h.chunk(c->h.user, off, (uint16_t)m, buf) <= 0) { logf_(c, "cf: express: the staged image cannot be read"); x->st = XS_DONE; finish(c, CF_D_GONE); return; }
        if (m < len) memset(buf + m, 0xFF, len - m);
    have:
        c->s_have = 0;
        /* The ESP32's SPI DMA moves 32-bit words: a write whose length is not a multiple of 4 lost its
           tail on unit 2 (Tetris, a post starting at 0x2af9 refused every time it was sent again). The
           write is padded (FF) to whole words; the bytes past the post land in ring slots of positions
           not yet posted (the next post writes them), the post itself names only len bytes. */
        for (uint32_t p = len; p < qlen; p++) buf[p] = 0xFF;
        if (c->h.xs_ring(c->h.user, (uint16_t)(x->feed % CF_XS_RING), buf, (uint16_t)qlen)) return;   /* the bus was busy: next time */
        const uint32_t crc = cf_crc32(0, buf, len), e = x->feed + len;
        const uint8_t p[11] = { CF_XS_P_POST, (uint8_t)e, (uint8_t)(e >> 8), (uint8_t)(e >> 16), (uint8_t)(e >> 24),
                                (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16), (uint8_t)(crc >> 24) };
        xs_pkt(c, p, 11);
        x->n_posts++;
        x->feed = e;
        x->t_post = c->now;
    }
}

/* the planner: the next ops while the FPGA's queue has room */
static void xs_plan(cf_esp_t *c)
{
    struct cf_xs *x = c->xs;
    uint32_t s0, sz;
    for (int guard = 0; guard < 16 && c->armed && x->st != XS_DONE; guard++) {
        switch (x->st) {
        case XS_SYNC:
            xs_simple(c, CF_XS_P_ABORT);
            xs_simple(c, CF_XS_P_RESET);
            x->qn = 0; x->fixn = 0; x->pos_next = 0; x->feed = 0; x->have = 0; x->cons = x->valid = 0;
            xs_cfg(c, XK_SYNC, 1);
            x->st = XS_SYNCW;
            return;
        case XS_SYNCW:
        case XS_TAILW:
        case XS_RAMW:
        case XS_B0SYNCW:
        case XS_B0W:
            return;                                   /* results move these on */
        case XS_E0:
            if (x->e0_done) { x->st = XS_BLOCKS; x->pblk = 1; x->pstep = 0; continue; }
            if (!xs_room(c) || x->qn > 2) return;
            c->j.phase = CF_PH_WRITE;                 /* from here the cart is in flux */
            jr_save(c);
            sec_geom(c, 0, &s0, &sz);
            xs_op(c, CF_XS_O_ERASE, XK_NONE, 0, 0, 0, c->chip->erase_op, 0);
            xs_op(c, CF_XS_O_CRC, XK_BLANK0, 0, 0, sz, 0, 0);
            x->e0_done = 1; c->n_erase++;
            x->st = XS_BLOCKS; x->pblk = 1; x->pstep = 0;
            continue;
        case XS_BLOCKS:
            if (x->fixn && xs_room(c)) {             /* a CRC result that was missed: read the block again */
                x->fixn--;
                xs_op(c, CF_XS_O_CRC, x->fix[x->fixn].kind, x->fix[x->fixn].blk, x->fix[x->fixn].f, x->fix[x->fixn].n, 0, x->fix[x->fixn].want);
                continue;
            }
            while (x->pstep == 0 && x->pblk < c->s_nblk && done_get(c, x->pblk)) {
                if (c->resume && !((x->rechk[x->pblk >> 3] >> (x->pblk & 7)) & 1)) break;   /* cartspeed2: read it back once */
                x->pblk++;
            }
            if (x->pblk >= c->s_nblk) { x->st = XS_TAIL; x->tblk = c->s_nblk; continue; }
            if (!xs_room(c)) return;
            sec_geom(c, x->pblk, &s0, &sz);
            if (x->pstep == 0 && done_get(c, x->pblk)) {
                /* cartspeed2: a block written before the interruption: the cart must hold the staged
                   bytes (the image's, once its check passes before block 0); one crc32 read, no write */
                if (xs_need(c, s0 + sz)) return;
                if (xs_scan(c, s0, sz, &x->want, &x->nz0, &x->nz1)) { if (x->st != XS_DONE && c->armed) { logf_(c, "cf: express: the staged image cannot be read"); x->st = XS_DONE; finish(c, CF_D_GONE); } return; }
                if (xs_blk_src(c, s0, sz)) return;
                x->rechk[x->pblk >> 3] |= (uint8_t)(1u << (x->pblk & 7));
                xs_op(c, CF_XS_O_CRC, XK_RECHK, x->pblk, s0, sz, 0, x->want);
                x->pblk++;
                continue;
            }
            if (x->pstep == 0) {
                if (xs_need(c, s0 + sz)) return;          /* cartspeed2: the block is still arriving */
                if (xs_scan(c, s0, sz, &x->want, &x->nz0, &x->nz1)) { if (x->st != XS_DONE && c->armed) { logf_(c, "cf: express: the staged image cannot be read"); x->st = XS_DONE; finish(c, CF_D_GONE); } return; }
                if (xs_blk_src(c, s0, sz)) return;
                xs_op(c, CF_XS_O_ERASE, XK_NONE, x->pblk, s0, 0, c->chip->erase_op, 0); c->n_erase++;
                x->pstep = 1;
            } else if (x->pstep >= 1 && x->pstep - 1 < x->nseg) {  /* the block's non-FF segments, one PROG each */
                const int k = x->pstep - 1;
                xs_op(c, CF_XS_O_PROG, XK_NONE, x->pblk, x->seg[k][0], x->seg[k][1] - x->seg[k][0], 0, 0);
                x->pstep++;
            } else {
                xs_op(c, CF_XS_O_CRC, XK_VERIFY, x->pblk, s0, sz, 0, x->want);
                x->pstep = 0; x->pblk++;
            }
            continue;
        case XS_TAIL:
            if (x->fixn && xs_room(c)) {
                x->fixn--;
                if (x->fix[x->fixn].kind == XK_TAILE) {             /* not blank: erase it, then read it back */
                    if (x->qn > XS_MAXQ - 2) { x->fixn++; return; }
                    xs_op(c, CF_XS_O_ERASE, XK_NONE, x->fix[x->fixn].blk, x->fix[x->fixn].f, 0, c->chip->erase_op, 0); c->n_erase++;
                    xs_op(c, CF_XS_O_CRC, XK_TAILV, x->fix[x->fixn].blk, x->fix[x->fixn].f, x->fix[x->fixn].n, 0, 0);
                    continue;
                }
                /* a read-back whose result was missed: read it again (never erase again for that) */
                xs_op(c, CF_XS_O_CRC, x->fix[x->fixn].kind, x->fix[x->fixn].blk, x->fix[x->fixn].f, x->fix[x->fixn].n, 0, x->fix[x->fixn].want);
                continue;
            }
            sec_geom(c, x->tblk, &s0, &sz);
            if (x->tblk >= c->nsec || !sz || s0 + sz > strm_writable(c)) {
                if (x->qn || x->fixn) { if (x->fixn) return; x->st = XS_TAILW; return; }
                /* every block of the image must be verified before the save RAM and block 0 (a result
                   that was missed and could not be queued again: that block once more) */
                for (int b = 1; b < c->s_nblk; b++) if (!done_get(c, b)) { x->st = XS_BLOCKS; x->pblk = b; x->pstep = 0; break; }
                if (x->st == XS_BLOCKS) continue;
                /* cartspeed2: the save RAM and block 0 only once the whole image is checked */
                if (xs_gate(c)) return;
                x->st = XS_RAM; x->pblk = 0; x->pstep = 0; continue;
            }
            if (!xs_room(c)) return;
            xs_op(c, CF_XS_O_CRC, XK_TAILCHK, x->tblk, s0, sz, 0, 0);
            x->tblk++;
            continue;
        case XS_RAM:
            if (!c->opt.ram_clear) { x->st = XS_B0; x->b0_step = 0; c->sp = SP_BLK0; continue; }
            if (x->pstep == 0) {                       /* the safe timing for the FRAM, session stays on */
                if (x->qn) return;
                const uint8_t t = x->timing; x->timing = 0; xs_cfg(c, XK_NONE, 1); x->timing = t;
                x->pstep = 1; x->pblk = 0; c->n_sav_fail = 0; c->sp = SP_RAMCLR; c->pos = 0;
                continue;
            }
            if (x->pblk >= (int)(CF_RAM_SIZE / 1024u)) { x->st = XS_RAMW; return; }
            if (x->qn > XS_MAXQ - 2) return;
            {
                const uint32_t o = (uint32_t)x->pblk * 1024u;
                uint32_t want;
                if (c->opt.ram_clear == 2 && c->h.sav_read) { if (c->h.sav_read(c->h.user, o, c->sbuf, 1024)) memset(c->sbuf, 0, 1024); }
                else memset(c->sbuf, 0, 1024);
                want = cf_crc32(0, c->sbuf, 1024);
                c->s_have = 0;
                xs_op(c, CF_XS_O_RAMW, XK_NONE, c->opt.ram_clear == 2 ? 2 : 1, o, 1024, 0, 0);
                xs_op(c, CF_XS_O_RAMCRC, XK_RAMV, x->pblk, o, 1024, 0, want);
                x->pblk++;
            }
            continue;
        case XS_B0SYNC:
            xs_simple(c, CF_XS_P_ABORT);
            xs_simple(c, CF_XS_P_RESET);
            x->qn = 0; x->fixn = 0; x->pos_next = 0; x->feed = 0; x->have = 0; x->cons = x->valid = 0;
            xs_cfg(c, XK_SYNC, 1);
            x->st = XS_B0SYNCW;
            return;
        case XS_B0: {
            sec_geom(c, 0, &s0, &sz);
            const uint32_t end = sz < c->s_wr_end ? sz : c->s_wr_end;
            if (x->b0_step == 0) {                    /* a restart: block 0 erased again first */
                if (xs_gate(c)) return;
                if (xs_scan(c, 0, sz, &x->want, &x->nz0, &x->nz1)) { if (x->st != XS_DONE && c->armed) { x->st = XS_DONE; finish(c, CF_D_GONE); } return; }
                if (xs_blk_src(c, 0, sz)) return;
                if (x->retries) { xs_op(c, CF_XS_O_ERASE, XK_NONE, 0, 0, 0, c->chip->erase_op, 0); c->n_erase++; }
                x->b0_step = 1; x->pstep = 0;
                continue;
            }
            if (!xs_room(c)) return;
            if (x->b0_step == 1) {                   /* from 0x104 to the end: its non-FF segments */
                if (x->pstep < x->nseg) {
                    uint32_t a = x->seg[x->pstep][0], e = x->seg[x->pstep][1];
                    if (a < 0x104u) a = 0x104u;
                    if (e > end) e = end;
                    if (a < e) xs_op(c, CF_XS_O_PROG, XK_NONE, 0, a, e - a, 0, 0);
                    x->pstep++;
                    continue;
                }
                x->b0_step = 2; continue;
            }
            if (x->b0_step == 2) { xs_op(c, CF_XS_O_PROG, XK_NONE, 0, 0, 0x100u, 0, 0); x->b0_step = 3; continue; }
            if (x->b0_step == 3) { xs_op(c, CF_XS_O_PROG, XK_NONE, 0, 0x100u, 4, 0, 0); x->b0_step = 4; continue; }   /* the entry point: last */
            xs_op(c, CF_XS_O_CRC, XK_BLK0V, 0, 0, sz, 0, x->want);
            x->st = XS_B0W;
            return;
        }
        case XS_B0BAD:
            if (!xs_room(c)) return;
            xs_op(c, CF_XS_O_ERASE, XK_B0ERASE, 0, 0, 0, c->chip->erase_op, 0); c->n_erase++;
            x->st = XS_B0W;
            return;
        default:
            return;
        }
    }
}

/* one finished op */
static void xs_result(cf_esp_t *c, int i, int have_crc)
{
    struct cf_xs *x = c->xs;
    const uint8_t kind = x->q[i].kind;
    const int blk = x->q[i].blk;
    if (x->q[i].op == CF_XS_O_PROG) c->n_program_bytes += x->q[i].n;
    if (x->q[i].op == CF_XS_O_CRC) c->n_read_bytes += x->q[i].n;
    if ((x->q[i].op == CF_XS_O_CRC || x->q[i].op == CF_XS_O_RAMCRC) && !have_crc && kind != XK_NONE) {
        /* its result was in a status that never came (two ops ended within one status): read it again */
        x->n_missed++;
        if (x->fixn < XS_FIX) { x->fix[x->fixn].kind = kind; x->fix[x->fixn].blk = (int16_t)blk; x->fix[x->fixn].f = x->q[i].f; x->fix[x->fixn].n = x->q[i].n; x->fix[x->fixn].want = x->q[i].want; x->fixn++; }
        if (x->st == XS_B0W && kind == XK_BLK0V) { x->st = XS_B0; x->b0_step = 4; }
        if (x->st == XS_TAILW) x->st = XS_TAIL;
        if (kind == XK_RAMV) {                         /* that KiB again (and the ones after it) */
            x->fixn--;
            if (x->st == XS_RAMW) x->st = XS_RAM;
            if ((int)(x->q[i].f / 1024u) < x->pblk) x->pblk = (int)(x->q[i].f / 1024u);
        }
        return;
    }
    switch (kind) {
    case XK_SYNC:
        if (x->st == XS_SYNCW) x->st = XS_E0;
        else if (x->st == XS_B0SYNCW) x->st = XS_B0;
        break;
    case XK_RAMV:
        if (x->crc != x->q[i].want && x->ram_tries < 2) {     /* that KiB again (a write op lost on the line) */
            x->ram_tries++;
            if ((int)(x->q[i].f / 1024u) < x->pblk) x->pblk = (int)(x->q[i].f / 1024u);
            if (x->st == XS_RAMW) x->st = XS_RAM;
            break;
        }
        x->ram_tries = 0;
        if (x->crc != x->q[i].want) {
            c->n_sav_fail++;
            logf_(c, "cf: express: the save RAM did not take the data at %05x: left as it is", (unsigned)x->q[i].f);
        }
        c->pos = x->q[i].f + 1024u;
        break;
    case XK_BLANK0:
        if (!x->all_ff) { xs_restart(c, "block 0 is not blank after its erase"); return; }
        break;
    case XK_VERIFY:
        if (x->crc != x->q[i].want) {
            logf_(c, "cf: express: block %d reads back crc32 %08x, the staged bytes %08x", blk, (unsigned)x->crc, (unsigned)x->q[i].want);
            xs_restart(c, "a block did not verify");
            return;
        }
        done_set(c, blk, 1);
        jr_save(c);
        x->verified++;
        { uint32_t s0, sz; sec_geom(c, blk, &s0, &sz); if (s0 + sz > c->pos) c->pos = s0 + sz; }
        break;
    case XK_RECHK:
        if (x->crc != x->q[i].want) {
            x->n_rebad++;
            logf_(c, "cf: express: block %d (done before the interruption) reads back crc32 %08x, the staged bytes %08x: written again", blk, (unsigned)x->crc, (unsigned)x->q[i].want);
            done_set(c, blk, 0);
            jr_save(c);
        } else x->n_rechk++;
        break;
    case XK_TAILCHK:
        if (!x->all_ff && x->fixn < XS_FIX) {
            x->fix[x->fixn].kind = XK_TAILE; x->fix[x->fixn].blk = (int16_t)blk; x->fix[x->fixn].f = x->q[i].f; x->fix[x->fixn].n = x->q[i].n; x->fix[x->fixn].want = 0; x->fixn++;
            if (x->st == XS_TAILW) x->st = XS_TAIL;
        }
        break;
    case XK_TAILV:
        if (!x->all_ff) { xs_restart(c, "a block past the image is not blank after its erase"); return; }
        break;
    case XK_BLK0V:
        if (x->crc != x->q[i].want) {
            logf_(c, "cf: express: block 0 reads back crc32 %08x, the staged bytes %08x", (unsigned)x->crc, (unsigned)x->q[i].want);
            if (x->retries < (uint16_t)(c->opt.max_retries + 1) * 2u - 1u) { xs_restart(c, "block 0 did not verify"); return; }   /* erased and written again */
            logf_(c, "cf: express: block 0: erased again (never left startable)");
            x->st = XS_B0BAD;
            break;
        }
        logf_(c, "cf: express: \"%s\" is on the cart, %u bytes, every block verified (crc32), %u ms, %u posts, %u refused, %u restarts; waited %u ms for the image, %u ms for its check; %u blocks of an earlier session read back (%u written again)",
              c->j.strm_title, (unsigned)c->j.strm_size, (unsigned)(c->now - x->t_start), (unsigned)x->n_posts, (unsigned)x->n_naks, (unsigned)x->n_resync,
              (unsigned)x->ms_wait, (unsigned)x->ms_gate, (unsigned)x->n_rechk + x->n_rebad, (unsigned)x->n_rebad);
        if (x->m_nprog)   /* the chip itself, measured by the FPGA (hclk 16.777216 MHz) */
            logf_(c, "cf: express: measured: program wait %u ns per byte (%u bytes, since the last restart), erase busy up to %u us, bus timing %u",
                  (unsigned)((uint64_t)x->m_pbusy * 59605u / 1000u / x->m_nprog), (unsigned)x->m_nprog, (unsigned)((uint64_t)x->m_ebusy_max * 59605u / 1000000u), (unsigned)x->timing);
        xs_cfg(c, XK_NONE, 0);                        /* safe timing, session off */
        x->st = XS_DONE;
        status(c, "DONE", 1, 1);
        finish(c, CF_D_OK);
        break;
    case XK_B0ERASE:
        x->st = XS_DONE;
        memset(c->j.done, 0, sizeof c->j.done);
        c->j.strm_tries++;
        finish(c, CF_D_VERIFY);
        break;
    default:
        break;
    }
}

void cf_esp_xs_status(cf_esp_t *c, const uint8_t *p, uint8_t n)
{
    struct cf_xs *x = c->xs;
    if (!x || n < 21 || p[0] != 1) return;
    x->n_status++;
    x->t_status = c->now; x->t_ask = 0;
    if (!x->on || x->st == XS_DONE || x->st == XS_IDLE || !c->armed) return;
    x->have = 1;
    x->flags = p[1]; x->done_seq = p[2]; x->err = p[3];
    x->cons = (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
    x->valid = (uint32_t)p[8] | (uint32_t)p[9] << 8 | (uint32_t)p[10] << 16 | (uint32_t)p[11] << 24;
    x->crc = (uint32_t)p[12] | (uint32_t)p[13] << 8 | (uint32_t)p[14] << 16 | (uint32_t)p[15] << 24;
    x->faddr = (uint32_t)p[16] | (uint32_t)p[17] << 8 | (uint32_t)p[18] << 16;
    x->all_ff = (p[19] >> 7) & 1;
    x->run_seq = p[20];
    if (n >= 32) {
        x->m_pbusy = (uint32_t)p[21] | (uint32_t)p[22] << 8 | (uint32_t)p[23] << 16 | (uint32_t)p[24] << 24;
        x->m_ebusy = (uint32_t)p[25] | (uint32_t)p[26] << 8 | (uint32_t)p[27] << 16 | (uint32_t)p[28] << 24;
        x->m_nprog = (uint32_t)p[29] | (uint32_t)p[30] << 8 | (uint32_t)p[31] << 16;
        if (x->m_ebusy > x->m_ebusy_max) x->m_ebusy_max = x->m_ebusy;
    }
    const uint8_t naks = (p[19] >> 4) & 7;
    /* the stream restarted after a SYNC: statuses from before the RESET are not ours (a restart that
       has not sent its SYNC yet ignores them too: unit 2, a stale error re-restarted it 5 times) */
    if (x->st == XS_SYNC || x->st == XS_B0SYNC) return;
    if (x->st == XS_SYNCW || x->st == XS_B0SYNCW) {
        if (x->done_seq != x->sync_seq || x->err) return;
        x->naks_seen = naks;
    }
    if (x->err) {
        static const char *const k[] = { "", "a byte did not program in time", "a byte read back wrong", "an erase did not end in time",
                                         "no data came for the ring", "its queue overflowed", "it was sent an unknown op", "it lost its session", "it was stopped" };
        logf_(c, "cf: express: the FPGA reports: %s at %06x", x->err < 9 ? k[x->err] : "an error", (unsigned)x->faddr);
        xs_restart(c, "the FPGA stopped");
        if (x->st != XS_DONE && c->armed) xs_plan(c);
        return;
    }
    if (x->naks_seen == 0xFF) x->naks_seen = naks;
    if (naks != x->naks_seen) {                    /* a post was refused (its crc32 or its place): from `valid` again */
        if (x->n_naks < 6) logf_(c, "cf: express: a post refused (%u): fpga valid %06x consumed %06x, flags %02x; sent to %06x, ops to %06x",
                                 (unsigned)((naks - x->naks_seen) & 7), (unsigned)x->valid, (unsigned)x->cons, x->flags, (unsigned)x->feed, (unsigned)x->pos_next);
        x->naks_seen = naks; x->n_naks++;
        if (x->valid < x->feed) x->feed = x->valid;
    }
    /* the FPGA waits for bytes it never got (a post lost on the line: unit 2, the LAST post of an op
       has no later post to be refused for it): every byte it has is consumed and it says starved */
    if ((x->flags & CF_XS_F_STARVED) && x->cons == x->valid && x->valid < x->feed && c->now - x->t_post > 400u) {
        /* (only when the last post went out long ago: posts still on the line are not lost; re-sending
           them made duplicates, each refused, each a rewind: unit 2, the CF image went 34 s -> 151 s) */
        x->n_lost++;
        logf_(c, "cf: express: the FPGA is waiting at %06x (posts to %06x lost): sent again", (unsigned)x->valid, (unsigned)x->feed);
        x->feed = x->valid;
    }
    /* finished ops, in order */
    while (x->qn) {
        const int i = x->qh;
        const uint8_t d = (uint8_t)(x->done_seq - x->q[i].seq);
        if (d >= 128) break;                       /* not yet */
        x->qh = (x->qh + 1) % XS_Q; x->qn--;
        xs_result(c, i, d == 0);
        if (x->st == XS_DONE || !c->armed) return;
        if (x->st == XS_SYNC || x->st == XS_B0SYNC) break;   /* a restart */
    }
    if (x->st == XS_TAILW && !x->qn && !x->fixn) x->st = XS_TAIL;   /* cartspeed2: its checks (every block done, the image checked) before the save RAM */
    if (x->st == XS_RAMW && !x->qn) {
        logf_(c, "cf: express: %u blocks verified, the save RAM %s, %u ms so far", (unsigned)x->verified,
              c->n_sav_fail ? "partly written" : c->opt.ram_clear == 2 ? "written" : "cleared", (unsigned)(c->now - x->t_start));
        x->st = XS_B0; x->b0_step = 0; c->sp = SP_BLK0;
    }
    xs_plan(c);
    xs_pump(c);
}

/* from stream_next while the express stream runs: plan, feed, keep the writer waiting */
static void xs_next(cf_esp_t *c)
{
    struct cf_xs *x = c->xs;
    xs_plan(c);
    xs_pump(c);
    if (!c->armed || x->st == XS_DONE) return;
    /* a SYNC whose CFG never finished (a packet lost on the line): send the SYNC again */
    if ((x->st == XS_SYNCW || x->st == XS_B0SYNCW) && c->now - x->t_sync > 2000u) {
        if (++x->n_resync_lost > 8) { logf_(c, "cf: express: the FPGA does not take a new start"); x->st = XS_DONE; finish(c, CF_D_FAILED); return; }
        logf_(c, "cf: express: no answer to the new start: again");
        x->st = x->st == XS_SYNCW ? XS_SYNC : XS_B0SYNC;
        xs_plan(c);
        if (!c->armed || x->st == XS_DONE) return;
    }
    /* nothing heard: ask; nothing for long: the FPGA is not there any more */
    if (x->qn || x->st == XS_SYNCW || x->st == XS_B0SYNCW) {
        const uint32_t last = x->t_status > x->t_ask ? x->t_status : x->t_ask;
        if (c->now - last > XS_ASK_MS) { xs_simple(c, CF_XS_P_STATUS); x->t_ask = c->now; }
        if (x->t_status && c->now - x->t_status > XS_DEAD_MS) { x->t_status = c->now; xs_restart(c, "no status from the FPGA for 15 s"); }
        else if (!x->t_status && c->now - x->t_start > XS_DEAD_MS) { x->t_start = c->now; xs_restart(c, "the FPGA never answered"); }
    }
    if (c->armed && c->ph != CF_PH_DONE) strm_wait(c);
}

static void stream_result(cf_esp_t *c, uint8_t res, const uint8_t *r, uint16_t n)
{
    const uint8_t cmd = c->last.cmd;
    if (cmd == CF_C_WAIT) { stream_next(c); return; }
    switch (c->sp) {
    case SP_WPTEST:
        if (wp_result(c, res, r, n) < 0) return;
        break;
    case SP_TOP:
        if (res != CF_R_OK) {
            logf_(c, "cf: stream: the image is longer than the writable flash and the cart's protected top block is not blank: refused, nothing written");
            finish(c, CF_D_BAD_IMAGE);
            return;
        }
        c->pos += c->last.len;
        break;
    case SP_ERASE0:
        if (cmd == CF_C_ERASE) {
            if (res != CF_R_OK) { logf_(c, "cf: stream: erase of the first block: result %u", res); finish(c, CF_D_FAILED); return; }
            c->step = 1; c->pos = 0;
        } else if (res != CF_R_OK) {
            if (++c->retry > c->opt.max_retries) { finish(c, CF_D_FAILED); return; }
            c->step = 0;
        } else c->pos += c->last.len;
        break;
    case SP_BLOCKS:
        if (cmd == CF_C_ERASE) {
            if (res != CF_R_OK) { logf_(c, "cf: stream: erase of block %d: result %u", c->s_blk, res); finish(c, CF_D_FAILED); return; }
            c->step = 1; c->pos = 0;
        } else if (res != CF_R_OK) {
            if (++c->retry > c->opt.max_retries) { logf_(c, "cf: stream: block %d: result %u after %d tries", c->s_blk, res, c->retry); finish(c, CF_D_FAILED); return; }
            c->n_retries++;
            c->step = 0; c->pos = 0; c->s_have = 0;          /* the whole block again */
        } else if (cmd == CF_C_BLANK) c->pos += c->last.len;
        else { c->n_program_bytes += c->last.len; c->pos += c->last.len; c->s_have = 0; }
        break;
    case SP_TAIL:
        if (cmd == CF_C_ERASE) {
            if (res != CF_R_OK) { finish(c, CF_D_FAILED); return; }
            c->step = 0; c->pos = 0;
            if (++c->retry > c->opt.max_retries + 1) { finish(c, CF_D_FAILED); return; }
        } else if (res != CF_R_OK) c->step = 1;
        else { c->pos += c->last.len; c->retry = 0; }
        break;
    case SP_HASHCART:
    case SP_BLK0CHK:
        if (cmd == CF_C_ERASE) { finish(c, CF_D_VERIFY); return; }
        if (cmd == CF_C_CRC) {                             /* fast verify: one bank's CRC16 from the Game Boy side */
            if (res == CF_R_OK && n >= 2 && (uint16_t)(r[0] | r[1] << 8) == c->s_want16) { c->bank_retry = 0; c->pos += c->last.len; break; }
            if (++c->bank_retry <= 2) break;               /* the same bank again */
            logf_(c, "cf: stream: the cart's bank at %06x does not have the staged image's CRC16 (result %u)", (unsigned)c->pos, res);
            if (c->sp == SP_BLK0CHK) { c->step = 1; c->bank_retry = 0; break; }      /* the first block: erased again, never left startable */
            memset(c->j.done, 0, sizeof c->j.done);
            c->j.strm_tries++;
            finish(c, CF_D_VERIFY);
            return;
        }
        if (res != CF_R_OK || n != c->last.len) {
            if (++c->bank_retry > 3) { logf_(c, "cf: stream: the cart does not read back at %06x (result %u)", (unsigned)c->pos, res); finish(c, CF_D_FAILED); return; }
            break;
        }
        c->bank_retry = 0;
        c->n_read_bytes += n;
        if (c->sp == SP_HASHCART) { cf_sha256_update((cf_sha256_t *)&c->s_sha, r, n); c->s_crc = cf_crc32(c->s_crc, r, n); }
        else c->s_crc0_cart = cf_crc32(c->s_crc0_cart, r, n);
        c->pos += n;
        break;
    case SP_RAMCLR:
        if (res != CF_R_OK && ++c->bank_retry <= 2) break;
        if (res != CF_R_OK) { c->n_sav_fail++; logf_(c, "cf: stream: the save RAM did not take the data at %05x (result %u): left as it is", (unsigned)c->pos, res); }
        c->bank_retry = 0;
        c->pos += 1024;
        break;
    case SP_BLK0:
        if (cmd == CF_C_ERASE) {
            if (res != CF_R_OK) { finish(c, CF_D_FAILED); return; }
            c->step = 0; c->pos = CF_STRM_HEAD; c->s_have = 0;
        } else if (res != CF_R_OK) {
            if (++c->retry > c->opt.max_retries) { logf_(c, "cf: stream: the first block: result %u after %d tries", res, c->retry); c->sp = SP_BLK0CHK; c->step = 1; break; }
            c->n_retries++;
            c->step = 2;
        } else { c->n_program_bytes += c->last.len; c->pos += c->last.len; c->s_have = 0; }
        break;
    default:
        break;
    }
    stream_next(c);
}

/* ---------------------------------------------------------------- CF_JOB_USBREAD / CF_JOB_USBRAM
 * The owner's own backup and restore over the USB console (docs/CART_BACKUP_USB_PROTOCOL.md).
 * Frames to the computer: A5 'C' 'B' 'U', type ('R' ROM / 'S' save), offset u32 LE, length u16 LE,
 * data, crc32 LE over type..data. Nothing is stored on the ESP32. */
static void usb_line(cf_esp_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void usb_line(cf_esp_t *c, const char *fmt, ...)
{
    char b[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (c->h.host_line) c->h.host_line(c->h.user, b); else if (c->h.log) c->h.log(c->h.user, b);
}

static void usbread_next(cf_esp_t *c)
{
    const int ram = c->opt.rd_what == 1;
    const uint32_t end = c->opt.rd_off + c->opt.rd_len;
    if (c->pos >= end) {
        usb_line(c, "CBU1 end %s %u %u %08x", ram ? "sav" : "rom", (unsigned)c->opt.rd_off, (unsigned)c->opt.rd_len, (unsigned)c->rd_crc);
        finish(c, CF_D_OK);
        return;
    }
    if (ram) { cmd_ram(c, CF_C_READ, c->pos, (uint16_t)(end - c->pos < 1024 ? end - c->pos : 1024)); return; }
    /* Most of a cart is usually empty, and every byte crosses two 115200 links (FPGA to ESP32,
       ESP32 to the computer: ~5.6 KiB/s on a unit). A whole bank that reads FF twice on the GB is
       named in one line instead of sent. */
    if (c->pos % BANK == 0 && c->rd_bank_at != c->pos + 1) {      /* a new bank: decide once */
        c->rd_bank_at = c->pos + 1;
        c->rd_bstate = (c->opt.rd_skip && end - c->pos >= BANK) ? 0 : 2;
    }
    if (c->rd_bstate < 2) { cmd_flash(c, CF_C_BLANK, c->pos, (uint16_t)BANK); return; }
    cmd_flash(c, CF_C_READ, c->pos, chunk_in_bank(c->pos, end, 1024));
}

static void begin_usbread(cf_esp_t *c)
{
    char t[17];
    cf_title_from_header(c->hdr, t);
    const unsigned w = cf_image_compat(c->hdr);
    /* xs=1 (cartspeed1): a staged install here goes the express way (the FPGA writes the cart) */
    const unsigned xs = (c->h.xs_avail && c->h.xs_send && c->h.xs_ring && c->h.xs_avail(c->h.user) && c->chip->cmdset == CF_CS_AMD_AAA) ? 1u : 0u;
    /* cartspeed2: fpga=MAJ.MIN (the page's fast copy needs the bridge of 18.27+); clr=bg: the staged copy is
       erased after `done` by the firmware itself (also after a restart): the page does not wait for it */
    const unsigned fv = c->h.fpga_ver ? c->h.fpga_ver(c->h.user) : 0u;
    usb_line(c, "CBU1 info proto=1 flash=%u writable=%u ram=%u chunk=1024 skipblank=1 bin=1 engine=%u xs=%u fpga=%u.%u clr=bg stg=%06x type=%02x romcode=%02x ramcode=%02x header=%s title=\"%s\"",
             (unsigned)c->chip->size, (unsigned)strm_writable(c), (unsigned)CF_RAM_SIZE, c->engine ? 1u : 0u, xs, fv >> 8, fv & 0xFFu, (unsigned)(cf_stage_hold ? 0u : cf_stage_rom_off), c->hdr[0x47], c->hdr[0x48], c->hdr[0x49],
             w & CF_W_HEADER ? "bad" : "ok", t);
    if (c->opt.rd_what == 2) { finish(c, CF_D_OK); return; }
    const uint32_t cap = c->opt.rd_what == 1 ? CF_RAM_SIZE : c->chip->size;
    if (c->opt.rd_off > cap) c->opt.rd_off = cap;
    if (!c->opt.rd_len || c->opt.rd_off + c->opt.rd_len > cap) c->opt.rd_len = cap - c->opt.rd_off;
    free(c->sbuf);
    c->sbuf = malloc(CF_PROG_CHUNK);
    if (!c->sbuf) { finish(c, CF_D_ABORTED); return; }
    c->ph = CF_PH_USBREAD; c->pos = c->opt.rd_off; c->rd_crc = 0; c->bank_retry = 0;
    c->rd_bstate = 2; c->rd_bank_at = 0;
    usbread_next(c);
}

static void usbread_result(cf_esp_t *c, uint8_t res, const uint8_t *r, uint16_t n)
{
    if (c->last.cmd == CF_C_BLANK) {
        if (res != CF_R_OK && c->opt.rd_dirty_from && c->pos >= c->opt.rd_dirty_from) {
            usb_line(c, "CBU1 dirty %u", (unsigned)c->pos);   /* 1.0.5: past the source, not blank: no need to read it */
            finish(c, CF_D_OK);
            return;
        }
        if (res != CF_R_OK) c->rd_bstate = 2;             /* something in it (or a bad read): send it */
        else if (c->rd_bstate == 0) c->rd_bstate = 1;     /* once more, as every read is done twice */
        else {
            static const uint8_t ff[64] = { [0 ... 63] = 0xFF };
            for (uint32_t i = 0; i < BANK; i += sizeof ff) c->rd_crc = cf_crc32(c->rd_crc, ff, sizeof ff);
            usb_line(c, "CBU1 blank %u %u", (unsigned)c->pos, (unsigned)BANK);
            c->pos += BANK;
        }
        usbread_next(c);
        return;
    }
    if (res != CF_R_OK || n != c->last.len || n > 1024) {
        if (++c->bank_retry > 3) { usb_line(c, "CBU1 fail read %u result %u", (unsigned)c->pos, res); finish(c, CF_D_FAILED); return; }
        usbread_next(c);
        return;
    }
    uint8_t *f = c->sbuf;
    memcpy(f, "\xA5" "CBU", 4);
    f[4] = c->opt.rd_what == 1 ? 'S' : 'R';
    f[5] = (uint8_t)c->pos; f[6] = (uint8_t)(c->pos >> 8); f[7] = (uint8_t)(c->pos >> 16); f[8] = (uint8_t)(c->pos >> 24);
    f[9] = (uint8_t)n; f[10] = (uint8_t)(n >> 8);
    memcpy(f + 11, r, n);
    const uint32_t crc = cf_crc32(0, f + 4, 7u + n);
    f[11 + n] = (uint8_t)crc; f[12 + n] = (uint8_t)(crc >> 8); f[13 + n] = (uint8_t)(crc >> 16); f[14 + n] = (uint8_t)(crc >> 24);
    if (c->h.host_frame) c->h.host_frame(c->h.user, f, 15u + n);
    c->rd_crc = cf_crc32(c->rd_crc, r, n);
    c->n_read_bytes += n;
    c->bank_retry = 0;
    c->pos += n;
    usbread_next(c);
}

static void usbram_next(cf_esp_t *c)
{
    if (!ovl_write_ok(c)) return;
    if (c->pos >= CF_RAM_SIZE) { usb_line(c, "CBU1 done sav %08x", (unsigned)c->rd_crc); finish(c, CF_D_OK); return; }
    const uint16_t n = c->opt.strm_chunk;
    if (!strm_chunk(c, n)) return;
    cmd_ram(c, CF_C_RAMWRITE, c->pos, n);
    c->last.src = 8;
}

static void begin_usbram(cf_esp_t *c)
{
    free(c->sbuf);
    c->sbuf = malloc(CF_PROG_CHUNK);
    if (!c->sbuf || !c->h.chunk) { finish(c, CF_D_ABORTED); return; }
    if (!c->opt.strm_chunk || c->opt.strm_chunk > 1024 || 0x2000u % c->opt.strm_chunk) c->opt.strm_chunk = 64;
    c->ph = CF_PH_USBRAM; c->pos = 0; c->s_have = 0; c->s_wait_t0 = 0; c->rd_crc = 0; c->bank_retry = 0;
    usbram_next(c);
}

static void usbram_result(cf_esp_t *c, uint8_t res)
{
    if (c->last.cmd == CF_C_WAIT) { usbram_next(c); return; }
    if (res != CF_R_OK) {
        if (++c->bank_retry > 2) { usb_line(c, "CBU1 fail sav %u result %u", (unsigned)c->pos, res); finish(c, CF_D_FAILED); return; }
    } else {
        c->rd_crc = cf_crc32(c->rd_crc, c->sbuf, c->last.len);
        c->bank_retry = 0;
        c->pos += c->last.len;
        c->s_have = 0;
    }
    usbram_next(c);
}

int cf_esp_stream_pending(const cf_esp_t *c, char id33[33], char title17[17], uint8_t *src)
{
    if (c->j.job != CF_JOB_STREAM || !(c->j.phase == CF_PH_WRITE || c->j.phase == CF_PH_FAILED)) return 0;
    if (id33) memcpy(id33, c->j.strm_id, 33);
    if (title17) memcpy(title17, c->j.strm_title, 17);
    if (src) *src = c->j.strm_src;
    return 1;
}

void cf_esp_stream_cancel(cf_esp_t *c)
{
    if (!cf_esp_stream_pending(c, NULL, NULL, NULL) || c->armed) return;
    logf_(c, "cf: the unfinished install of \"%s\" is given up: the cart has no first block until a game is written to it", c->j.strm_title);
    c->j.phase = CF_PH_IDLE;
    c->j.job = CF_JOB_NONE;
    c->j.result = CF_D_ABORTED;
    memset(c->j.done, 0, sizeof c->j.done);
    jr_save(c);
}

static void plan_next(cf_esp_t *c)
{
    if (c->ph == CF_PH_SELFTEST) { selftest_next(c); return; }
    if (c->ph == CF_PH_STREAM) { stream_next(c); return; }
    if (c->ph == CF_PH_USBREAD) { usbread_next(c); return; }
    if (c->ph == CF_PH_USBRAM) { usbram_next(c); return; }
    if (c->ph == CF_PH_WPTEST) { if (wp_next(c) > 0) write_plan_go(c, 0); return; }
    for (;;) {
        switch (c->ph) {
        case CF_PH_ID:
            memset(&c->last, 0, sizeof c->last);
            c->last.cmd = CF_C_ID;
            c->last.mbc = CP_MBC5;
            c->last.cmdset = c->id_try == 0 ? CF_CS_AMD_AAA : CF_CS_AMD_555;
            if (c->chip && !c->have_cfi) {        /* second ID: with the chip's CFI query */
                c->last.cmdset = c->chip->cmdset;
                c->last.cfi_mode = c->chip->cfi_mode;
                c->last.cfi_addr = c->chip->cfi_addr;
            }
            return;
        case CF_PH_BAK_ROM: {
            if (c->unit >= c->j.chip_size) {
                uint32_t crc = 0;
                uint8_t b[512];
                for (uint32_t o = 0; o < c->j.chip_size; o += sizeof b) {
                    c->h.bak_read(c->h.user, o, b, sizeof b);
                    crc = cf_crc32(crc, b, sizeof b);
                }
                c->j.bak_rom_crc32 = crc;
                c->j.bak_rom_size = c->j.chip_size;
                c->ph = CF_PH_BAK_RAM; c->bank_retry = 0;
                c->pos = 0;
                continue;
            }
            if (c->step == 0) { cmd_flash(c, CF_C_BLANK, c->unit, (uint16_t)BANK); return; }
            if (c->step == 1) {
                if (c->pos >= c->unit + BANK) { c->step = 2; continue; }
                cmd_flash(c, CF_C_READ, c->pos, chunk_in_bank(c->pos, c->unit + BANK, CF_READ_CHUNK));
                return;
            }
            cmd_flash(c, CF_C_CRC, c->unit, (uint16_t)BANK);
            return;
        }
        case CF_PH_SAVE:
            if (c->j.job == CF_JOB_RAMTEST) {
                if (c->step > 4) {
                    c->ramtest_ok = 1;
                    logf_(c, "cf: ramtest PASS: 1 KiB of RAM bank 1 took a pattern and its original bytes back (engine cycles)");
                    finish(c, CF_D_OK);
                    return;
                }
                if (c->step == 0 || c->step == 2 || c->step == 4) cmd_ram(c, CF_C_READ, CF_RAMTEST_OFF, 1024);
                else { cmd_ram(c, CF_C_RAMWRITE, CF_RAMTEST_OFF, 1024); c->last.src = c->step == 1 ? 6 : 7; c->last.src_off = 0; }
                return;
            }
            if (c->pos >= CF_RAM_SIZE) {
                logf_(c, "cf: save %s: 32 KiB, crc32 %08x", c->j.job == CF_JOB_SAVE_GET ? "taken from the cart" :
                      c->j.job == CF_JOB_SAVE_PUT ? "written to the cart and read back" : "cleared (00), read back", (unsigned)c->save_crc);
                finish(c, CF_D_OK);
                return;
            }
            if (c->j.job == CF_JOB_SAVE_GET) cmd_ram(c, CF_C_READ, c->pos, 1024);
            else {
                cmd_ram(c, CF_C_RAMWRITE, c->pos, 1024);
                c->last.src = c->j.job == CF_JOB_SAVE_PUT ? 5 : 3;
                c->last.src_off = c->pos;
            }
            return;
        case CF_PH_GBTEST:
            memset(&c->last, 0, sizeof c->last);
            c->last.cmd = CF_C_GBTEST;
            c->last.mbc = CP_MBC5;
            return;
        case CF_PH_RAMDUMP:
            if (c->pos >= CF_RAM_SIZE) {
                logf_(c, "cf: ramdump done: crc32 %08x", (unsigned)c->ramdump_crc);
                finish(c, CF_D_OK);
                return;
            }
            cmd_ram(c, CF_C_READ, c->pos, CF_RAMDUMP_CHUNK);
            c->last.flags |= CF_F_ENG;
            return;
        case CF_PH_BAK_RAM:
            if (c->pos >= CF_RAM_SIZE) {
                uint32_t crc = 0;
                uint8_t b[512];
                for (uint32_t o = 0; o < CF_RAM_SIZE; o += sizeof b) {
                    c->h.ram_read(c->h.user, o, b, sizeof b);
                    crc = cf_crc32(crc, b, sizeof b);
                }
                c->j.bak_ram_crc32 = crc;
                c->j.bak_ok = 1;
                {
                    int n = 0;
                    for (int k = 0; k < 32; k++) n += (c->j.ram_present >> k) & 1;
                    logf_(c, "cf: save RAM: %d of 32 KiB read stable%s", n, n < 32 ? " (the rest floats: not backed up)" : "");
                }
                memcpy(c->j.orig_hdr, c->hdr, 80);
                jr_save(c);
                logf_(c, "cf: backup complete: rom %u bytes crc32 %08x, ram crc32 %08x (kept on this Chromatic)",
                      (unsigned)c->j.bak_rom_size, (unsigned)c->j.bak_rom_crc32, (unsigned)c->j.bak_ram_crc32);
                after_backup(c);
                if (c->ph == CF_PH_DONE) return;
                continue;
            }
            cmd_ram(c, CF_C_READ, c->pos, 1024);
            return;
        case CF_PH_WRITE: {
            if (c->oi >= c->plan_n) { c->ph = CF_PH_VERIFY; c->oi = 0; c->pos = 0; c->piece_mismatch = 0; continue; }
            const int s = plan_sector(c, c->oi);
            if (done_get(c, s)) { c->oi++; c->step = 0; c->pos = 0; continue; }
            uint32_t s0, sz;
            sec_geom(c, s, &s0, &sz);
            const uint32_t end = s0 + sz;
            if (c->step == 0 || c->step == 2) {       /* compare pieces / blank check pieces */
                if (c->pos == 0) { c->pos = s0; c->piece_mismatch = 0; }
                if (c->pos >= end) {
                    c->pos = 0;
                    if (c->step == 0) {
                        if (!c->piece_mismatch) { done_set(c, s, 1); jr_save(c); c->oi++; continue; }   /* already right */
                        c->step = 1;
                    } else {
                        if (c->piece_mismatch) {
                            if (++c->retry > c->opt.max_retries) { finish(c, CF_D_FAILED); return; }
                            c->n_retries++;
                            c->step = 1;
                            continue;
                        }
                        c->step = 3;
                    }
                    continue;
                }
                const uint16_t n = chunk_in_bank(c->pos, end, BANK);
                if (c->step == 0 && c->j.job == CF_JOB_ROUNDTRIP && !tgt_all_ff(c, c->pos, n, NULL)) {
                    c->piece_mismatch = 1;               /* the round trip rewrites it anyway */
                    c->pos += n;
                    continue;
                }
                if (c->step == 2 || tgt_all_ff(c, c->pos, n, NULL)) cmd_flash(c, CF_C_BLANK, c->pos, n);
                else cmd_flash(c, CF_C_CRC, c->pos, n);
                return;
            }
            if (c->step == 1) { cmd_flash(c, CF_C_ERASE, s0, 0); c->n_erase++; return; }
            /* step 3: program */
            if (c->pos == 0) c->pos = s0;
            while (c->pos < end) {
                const uint16_t n = chunk_in_bank(c->pos, end, CF_PROG_CHUNK);
                if (!tgt_all_ff(c, c->pos, n, NULL)) {
                    cmd_flash(c, CF_C_PROGRAM, c->pos, n);
                    c->last.src = 1;
                    c->last.src_off = c->pos;
                    return;
                }
                c->pos += n;
            }
            done_set(c, s, 1);
            jr_save(c);
            status(c, c->j.job == CF_JOB_RESTORE ? "RESTORE" : c->j.job == CF_JOB_ROUNDTRIP ? "ROUNDTRIP" : "WRITE",
                   (uint32_t)c->oi + 1, (uint32_t)c->plan_n);
            c->oi++; c->step = 0; c->pos = 0; c->retry = 0;
            continue;
        }
        case CF_PH_VERIFY: {
            if (c->oi >= c->plan_n) {
                if (c->piece_mismatch) {
                    if (c->j.verify_pass) { finish(c, CF_D_FAILED); return; }
                    c->j.verify_pass = 1;
                    jr_save(c);
                    c->ph = CF_PH_WRITE; c->oi = 0; c->step = 0; c->pos = 0;
                    continue;
                }
                c->ph = CF_PH_RAM; c->pos = 0; c->bank_retry = 0; c->save_crc = 0;
                /* INSTALL clears the save RAM for the new game: first the game being replaced
                   keeps its save, in the save store (`cart save put` brings it back after that
                   game is installed again). Not on a resumed job: the RAM may be half cleared. */
                c->ram_kept = !(c->j.job == CF_JOB_INSTALL && c->opt.ram_clear && c->h.sav_write && !c->resume);
                continue;
            }
            const int s = plan_sector(c, c->oi);
            uint32_t s0, sz;
            sec_geom(c, s, &s0, &sz);
            if (c->pos == 0) c->pos = s0;
            if (c->pos >= s0 + sz) { c->oi++; c->pos = 0; continue; }
            const uint16_t n = chunk_in_bank(c->pos, s0 + sz, BANK);
            if (tgt_all_ff(c, c->pos, n, NULL)) cmd_flash(c, CF_C_BLANK, c->pos, n);
            else cmd_flash(c, CF_C_CRC, c->pos, n);
            return;
        }
        case CF_PH_RAM: {
            if (!c->ram_kept) {
                if (c->pos < CF_RAM_SIZE) { cmd_ram(c, CF_C_READ, c->pos, 1024); return; }
                logf_(c, "cf: the replaced game's save is kept in the save store (32 KiB, crc32 %08x)", (unsigned)c->save_crc);
                c->ram_kept = 1; c->pos = 0;
            }
            const int want = (c->j.job == CF_JOB_RESTORE) || (c->j.job == CF_JOB_INSTALL && c->opt.ram_clear);   /* not ROUNDTRIP */
            while (want && c->pos < CF_RAM_SIZE && !((c->j.ram_present >> (c->pos / 1024)) & 1)) c->pos += 1024;
            if (!want || c->pos >= CF_RAM_SIZE) { finish(c, CF_D_OK); return; }
            cmd_ram(c, CF_C_RAMWRITE, c->pos, 1024);
            c->last.src = c->j.job == CF_JOB_RESTORE ? 2 : 3;
            c->last.src_off = c->pos;
            return;
        }
        default:
            return;
        }
    }
}

/* ---------------------------------------------------------------- results */
static void on_hello(cf_esp_t *c, const uint8_t *r, uint16_t n)
{
    c->session = 1;
    c->seq = 1;
    c->prev_seq = 0;
    c->chip = NULL;
    c->have_cfi = 0;
    c->id_try = 0;
    c->wp_done = 0;
    if (n < 82) { finish(c, CF_D_BAD_HEADER); return; }
    memcpy(c->hdr, r + 2, 80);
    c->engine = (r[1] & CF_HF_ENGINE) != 0;
    if (!c->engine && c->opt.gb_timed)
        logf_(c, "cf: no cart bus engine: the GB-timed route (UNTESTED for program / erase on a ModRetro cart)");
    if (!c->engine && !c->opt.gb_timed) {
        logf_(c, "cf: no cart bus engine on this FPGA (or not allowed): ModRetro cart flash ignores GB-timed writes; refused");
        finish(c, CF_D_NO_ENGINE);
        return;
    }
    c->resume = c->j.phase == CF_PH_WRITE || c->j.phase == CF_PH_FAILED;
    logf_(c, "cf: writer v%u hello, cart \"%.16s\" header %s%s", r[0], (const char *)c->hdr + 0x34,
          cf_header_ok(c->hdr) ? "valid" : "INVALID", c->resume ? ", resuming an interrupted write" : "");
    if (!c->resume) {
        /* a cart whose header is invalid (a foreign ROM that does not start) is still a cart we
           can restore or overwrite: no refusal */
        if (c->j.job < CF_JOB_STREAM &&      /* stream / USB jobs keep no backup: any cart */
            c->j.bak_ok && memcmp(c->hdr, c->j.orig_hdr, 80) && memcmp(c->hdr, c->j.tgt_hdr, 80)) {
            logf_(c, "cf: this is not the cart the stored backup is from: refused");
            finish(c, CF_D_BAD_HEADER);
            return;
        }
    }
    c->ph = CF_PH_ID;
    plan_next(c);
}

static void on_id(cf_esp_t *c, uint8_t res, const uint8_t *r, uint16_t n)
{
    if (n < CF_ID_RESULT || res == CF_R_RESET || !r[144]) {
        logf_(c, "cf: the chip did not return to read mode after ID: stop (power-cycle the console)");
        finish(c, CF_D_FAILED);
        return;
    }
    if (!c->chip) {
        const cf_chip_t *k = cf_chip_match(r);
        logf_(c, "cf: ID (cmdset %u): %02x %02x %02x %02x -> %s", c->last.cmdset, r[0], r[1], r[2], r[3],
              k ? k->part : "no match");
        if (!k) {
            if (++c->id_try < 2) { plan_next(c); return; }
            finish(c, CF_D_WRONG_CHIP);
            return;
        }
        if (k->cmdset != c->last.cmdset) { finish(c, CF_D_WRONG_CHIP); return; }
        if (!k->verified && !k->bottom_test && !c->opt.allow_unverified) {
            logf_(c, "cf: %s is not verified on a real cart yet: refused", k->part);
            finish(c, CF_D_WRONG_CHIP);
            return;
        }
        if (!k->verified && k->bottom_test)
            logf_(c, "cf: %s: allowed; a write first tests that its first %u KiB block takes a write (write protection)", k->part,
                  (unsigned)(k->protect_bottom / 1024u));
        const int idx = (int)(k - cf_chips);
        if (c->resume && c->j.chip != idx) { finish(c, CF_D_WRONG_CHIP); return; }
        c->chip = k;
        if (k->geometry_from_cfi) { plan_next(c); return; }     /* second ID, with CFI */
    } else {
        cf_region_t r2[CF_MAX_REGIONS];
        uint32_t sz = 0;
        const int nr = cf_cfi_regions(r + 16, r2, CF_MAX_REGIONS, &sz);
        if (nr < 1 || sz != c->chip->size) {
            logf_(c, "cf: %s: CFI geometry unreadable / size %u != %u: refused", c->chip->part,
                  (unsigned)sz, (unsigned)c->chip->size);
            finish(c, CF_D_WRONG_CHIP);
            return;
        }
        memcpy(c->reg, r2, sizeof r2);
        c->nreg = nr;
        c->have_cfi = 1;
    }
    if (!c->have_cfi) {
        c->nreg = 0;
        for (int i = 0; i < CF_MAX_REGIONS && c->chip->regions[i].count; i++) c->reg[c->nreg++] = c->chip->regions[i];
    }
    uint32_t total = 0;
    c->nsec = 0;
    for (int i = 0; i < c->nreg; i++) { total += c->reg[i].size * c->reg[i].count; c->nsec += c->reg[i].count; }
    if (total != c->chip->size || c->nsec > CF_MAX_SECTORS) { finish(c, CF_D_WRONG_CHIP); return; }
    c->j.chip = (uint8_t)(c->chip - cf_chips);
    c->j.chip_size = c->chip->size;
    c->j.mbc = CP_MBC5;
    logf_(c, "cf: chip %s, %u KiB, %d erase units", c->chip->part, (unsigned)(c->chip->size / 1024), c->nsec);
    if (c->j.job == CF_JOB_STREAM) { begin_stream(c); return; }
    if (c->j.job == CF_JOB_USBREAD) { begin_usbread(c); return; }
    if (c->j.job == CF_JOB_USBRAM) { begin_usbram(c); return; }
    if (c->j.job >= CF_JOB_SAVE_GET && c->j.job <= CF_JOB_RAMTEST) {   /* the save RAM on its own: never the cart backup */
        if ((c->j.job == CF_JOB_SAVE_GET && !c->h.sav_write) || (c->j.job == CF_JOB_SAVE_PUT && !c->h.sav_read)) { finish(c, CF_D_FAILED); return; }
        if (c->j.job == CF_JOB_RAMTEST) {
            free(c->tbuf);
            c->tbuf = malloc(1024);
            if (!c->tbuf) { finish(c, CF_D_FAILED); return; }
        }
        c->ph = CF_PH_SAVE; c->pos = 0; c->step = 0; c->bank_retry = 0; c->save_crc = 0; c->ramtest_ok = 0;
        plan_next(c);
        return;
    }
    if (c->j.job == CF_JOB_GBTEST) {                   /* read-only experiment */
        c->ph = CF_PH_GBTEST;
        memset(c->gbtest, 0, sizeof c->gbtest);
        plan_next(c);
        return;
    }
    if (c->j.job == CF_JOB_RAMDUMP) {                  /* read-only: never the backup, never a write */
        c->ph = CF_PH_RAMDUMP; c->pos = 0; c->bank_retry = 0; c->ramdump_crc = 0;
        logf_(c, "cf: ramdump: 32 KiB of cart RAM through the engine, read only");
        plan_next(c);
        return;
    }
    if (c->resume) { if (c->j.job == CF_JOB_SELFTEST) begin_selftest(c); else begin_write_plan(c); return; }
    if (c->j.job == CF_JOB_BACKUP && c->j.bak_ok) {
        logf_(c, "cf: a backup of this cart is already stored: kept, nothing read");   /* never overwritten */
        finish(c, CF_D_OK);
        return;
    }
    if (c->j.job == CF_JOB_BACKUP || (c->j.job != CF_JOB_RESTORE && !c->j.bak_ok)) {
        c->ph = CF_PH_BAK_ROM;
        c->pos = 0; c->unit = 0; c->step = 0; c->bank_retry = 0;
        c->j.ram_present = 0;
        plan_next(c);
        return;
    }
    if (c->j.job == CF_JOB_SELFTEST) { begin_selftest(c); return; }
    begin_write_plan(c);
}

/* ---------------------------------------------------------------- SELFTEST
 * In an erase unit that is blank in the backup, in the upper half of the chip and not
 * protected: blank check, program a 4 KiB pattern, CRC it, erase the unit, blank check. */
static uint8_t st_pattern(uint32_t i) { return (uint8_t)(i * 37u + 11u + (i >> 8)); }
#define ST_LEN 4096u
static void begin_selftest(cf_esp_t *c)
{
    c->st_size = 0;
    for (int i = 0; i < c->nsec; i++) {
        uint32_t s0, sz;
        sec_geom(c, i, &s0, &sz);
        if (s0 < c->chip->size / 2 || s0 < c->chip->protect_bottom || s0 + sz > c->chip->size - c->chip->protect_top) continue;
        uint8_t b[512];
        int blank = 1;
        for (uint32_t o = 0; o < sz && blank; o += sizeof b) {
            c->h.bak_read(c->h.user, s0 + o, b, sizeof b);
            for (unsigned k = 0; k < sizeof b; k++) if (b[k] != 0xFF) { blank = 0; break; }
        }
        if (blank) { c->st_blk = s0; c->st_size = sz; break; }
    }
    if (!c->st_size) { logf_(c, "cf: selftest: no blank erase unit in the upper half"); finish(c, CF_D_ABORTED); return; }
    if (!c->h.power_ok || !c->h.power_ok(c->h.user)) { finish(c, CF_D_ABORTED); return; }
    c->j.phase = CF_PH_WRITE;            /* a RESTORE would put this unit back to FF */
    c->j.tgt_size = c->j.bak_rom_size;
    jr_save(c);
    logf_(c, "cf: selftest in the blank unit at %06x (%u KiB)", (unsigned)c->st_blk, (unsigned)(c->st_size / 1024));
    c->ph = CF_PH_SELFTEST;
    c->step = 0;
    c->pos = c->st_blk;
    c->piece_mismatch = 0;
    plan_next(c);
}
static void selftest_next(cf_esp_t *c)
{
    for (;;) {
        const uint32_t end = c->st_blk + c->st_size;
        switch (c->step) {
        case 0: case 4:                                        /* blank check the unit */
            if (c->pos >= end) {
                if (c->piece_mismatch) {
                    logf_(c, "cf: selftest: unit %06x not blank (%s)", (unsigned)c->st_blk, c->step == 0 ? "before" : "after the erase");
                    finish(c, CF_D_FAILED);
                    return;
                }
                if (c->step == 4) {
                    logf_(c, "cf: selftest PASS: %u-byte pattern programmed + verified at %06x, unit erased back to blank",
                          ST_LEN, (unsigned)c->st_blk);
                    c->j.phase = CF_PH_IDLE;
                    jr_save(c);
                    finish(c, CF_D_OK);
                    return;
                }
                c->step = 1; c->pos = c->st_blk;
                continue;
            }
            cmd_flash(c, CF_C_BLANK, c->pos, chunk_in_bank(c->pos, end, BANK));
            return;
        case 1:                                                /* program the pattern */
            if (c->pos >= c->st_blk + ST_LEN) { c->step = 2; c->pos = c->st_blk; continue; }
            cmd_flash(c, CF_C_PROGRAM, c->pos, chunk_in_bank(c->pos, c->st_blk + ST_LEN, CF_PROG_CHUNK));
            c->last.src = 4;
            c->last.src_off = c->pos - c->st_blk;
            return;
        case 2:                                                /* CRC of the pattern */
            if (c->pos >= c->st_blk + ST_LEN) { c->step = 3; continue; }
            cmd_flash(c, CF_C_CRC, c->pos, (uint16_t)(c->st_blk + ST_LEN - c->pos));
            return;
        case 3:                                                /* erase the unit back */
            cmd_flash(c, CF_C_ERASE, c->st_blk, 0);
            c->n_erase++;
            return;
        default:
            return;
        }
    }
}
static void selftest_result(cf_esp_t *c, uint8_t res, const uint8_t *r, uint16_t n)
{
    const uint8_t cmd = c->last.cmd;
    const uint32_t f = (uint32_t)c->last.bank * BANK + (c->last.addr - 0x4000u);
    if (cmd == CF_C_BLANK) { if (res != CF_R_OK) c->piece_mismatch = 1; c->pos = f + c->last.len; }
    else if (cmd == CF_C_PROGRAM) {
        if (res != CF_R_OK) { logf_(c, "cf: selftest: program at %06x: result %u", (unsigned)f, res); finish(c, CF_D_FAILED); return; }
        c->n_program_bytes += c->last.len;
        c->pos = f + c->last.len;
    } else if (cmd == CF_C_CRC) {
        uint16_t want = 0xFFFF;
        for (uint32_t i = 0; i < c->last.len; i++) { const uint8_t b = st_pattern(f - c->st_blk + i); want = cf_crc16(want, &b, 1); }
        if (res != CF_R_OK || n < 2 || (uint16_t)(r[0] | r[1] << 8) != want) {
            logf_(c, "cf: selftest: pattern CRC mismatch"); finish(c, CF_D_FAILED); return;
        }
        c->pos = f + c->last.len;
    } else if (cmd == CF_C_ERASE) {
        if (res != CF_R_OK) { logf_(c, "cf: selftest: erase result %u", res); finish(c, CF_D_FAILED); return; }
        c->step = 4; c->pos = c->st_blk; c->piece_mismatch = 0;
    }
    selftest_next(c);
}

static size_t b64_enc(const uint8_t *b, size_t n, char *o);
static void on_result(cf_esp_t *c, uint8_t res, const uint8_t *r, uint16_t n)
{
    switch (c->ph) {
    case CF_PH_WPTEST:
        if (wp_result(c, res, r, n) < 0) return;
        plan_next(c);
        return;
    case CF_PH_STREAM:
        stream_result(c, res, r, n);
        return;
    case CF_PH_USBREAD:
        usbread_result(c, res, r, n);
        return;
    case CF_PH_USBRAM:
        usbram_result(c, res);
        return;
    case CF_PH_SAVE: {
        if (c->j.job == CF_JOB_RAMTEST) {
            int ok = res == CF_R_OK;
            if (ok && (c->step == 0 || c->step == 2 || c->step == 4)) {
                if (n != 1024) ok = 0;
                else if (c->step == 0) memcpy(c->tbuf, r, 1024);
                else if (c->step == 2) { for (unsigned i = 0; i < 1024 && ok; i++) if (r[i] != ramtest_pattern(i)) ok = 0; }
                else if (memcmp(c->tbuf, r, 1024)) ok = 0;
            }
            if (!ok) {
                /* a failed step after the pattern went in: put the original back once more, then stop */
                logf_(c, "cf: ramtest FAILED at step %d (result %u)%s", c->step, res, c->step >= 1 && c->step < 3 ? ": writing the original bytes back" : "");
                if (c->step >= 1 && c->step < 3) { c->step = 3; c->ramtest_ok = 0; c->bank_retry = 99; plan_next(c); return; }
                finish(c, CF_D_FAILED);
                return;
            }
            if (c->bank_retry == 99 && c->step == 3) { finish(c, CF_D_FAILED); return; }
            c->step++;
            plan_next(c);
            return;
        }
        if (res != CF_R_OK || (c->j.job == CF_JOB_SAVE_GET && n != 1024)) {
            if (++c->bank_retry > 2) {
                logf_(c, "cf: save job stopped at %05x: result %u%s", (unsigned)c->pos, res, res == CF_R_UNSTABLE ? " (the RAM does not read the same twice)" : "");
                finish(c, CF_D_FAILED);
                return;
            }
            plan_next(c);
            return;
        }
        if (c->j.job == CF_JOB_SAVE_GET) {
            if (c->h.sav_write(c->h.user, c->pos, r, n)) { finish(c, CF_D_FAILED); return; }
            c->save_crc = cf_crc32(c->save_crc, r, n);
        } else {
            uint8_t b[256];
            for (uint32_t o = 0; o < 1024; o += sizeof b) {
                if (c->j.job == CF_JOB_SAVE_PUT) c->h.sav_read(c->h.user, c->pos + o, b, sizeof b); else memset(b, 0, sizeof b);
                c->save_crc = cf_crc32(c->save_crc, b, sizeof b);
            }
        }
        c->bank_retry = 0;
        c->pos += 1024;
        plan_next(c);
        return;
    }
    case CF_PH_GBTEST: {
        if (res != CF_R_OK || n < 196) { logf_(c, "cf: gbtest: result %u, %u bytes", res, n); finish(c, CF_D_FAILED); return; }
        const int a = !memcmp(r, r + 64, 64), b = !memcmp(r, r + 128, 64);
        int uni = 1;
        for (int i = 1; i < 64; i++) if (r[i] != r[0]) uni = 0;
        const int idok = c->chip && r[192] == c->chip->id[0] && r[193] == c->chip->id[1];
        c->gbtest[0] = 1; c->gbtest[1] = (uint8_t)a; c->gbtest[2] = (uint8_t)b;
        c->gbtest[3] = r[192]; c->gbtest[4] = r[193]; c->gbtest[5] = r[194]; c->gbtest[6] = r[195]; c->gbtest[7] = (uint8_t)idok;
        logf_(c, "cf: gbtest: RAM bank 0 by engine: %02x %02x %02x %02x %02x %02x %02x %02x ..%s", r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
              uni ? " (64 equal bytes: a weak comparison)" : "");
        logf_(c, "cf: gbtest: GB-timed RAM read, engine-enabled: %s (%02x %02x %02x %02x)", a ? "SAME as the engine" : "DIFFERENT", r[64], r[65], r[66], r[67]);
        logf_(c, "cf: gbtest: GB-timed RAM read, GB enable 0A@1000: %s (%02x %02x %02x %02x)", b ? "SAME as the engine" : "DIFFERENT", r[128], r[129], r[130], r[131]);
        logf_(c, "cf: gbtest: GB-timed flash ID (AA@1AAA 55@1555 90@1AAA, read 4000): %02x %02x -> %s; after exit %02x %02x",
              r[192], r[193], idok ? "the chip's ID: GB-TIMED FLASH COMMANDS WORK" : "not the ID", r[194], r[195]);
        finish(c, CF_D_OK);
        return;
    }
    case CF_PH_RAMDUMP: {
        if (res == CF_R_UNSTABLE && c->bank_retry < 2) { c->bank_retry++; plan_next(c); return; }
        char line[48 + 4 * (CF_RAMDUMP_CHUNK / 3 + 1)];
        int k = snprintf(line, sizeof line, "cf: ramdump %05x %s ", (unsigned)c->pos,
                         res == CF_R_OK && n == CF_RAMDUMP_CHUNK ? "ok" : res == CF_R_UNSTABLE ? "unstable" : "error");
        if (res == CF_R_OK && n == CF_RAMDUMP_CHUNK) {
            b64_enc(r, n, line + k);
            c->ramdump_crc = cf_crc32(c->ramdump_crc, r, n);
        } else if (res != CF_R_UNSTABLE) {
            if (++c->bank_retry > 3) { finish(c, CF_D_FAILED); return; }
            plan_next(c);
            return;
        }
        if (c->h.log) c->h.log(c->h.user, line);
        c->bank_retry = 0;
        c->pos += CF_RAMDUMP_CHUNK;
        plan_next(c);
        return;
    }
    case CF_PH_SELFTEST:
        selftest_result(c, res, r, n);
        return;
    case CF_PH_ID:
        on_id(c, res, r, n);
        return;
    case CF_PH_BAK_ROM: {
        if (c->step == 0) {
            if (res == CF_R_OK) {                /* blank bank: nothing to transfer */
                uint8_t ff[512];
                memset(ff, 0xFF, sizeof ff);
                for (uint32_t o = 0; o < BANK; o += sizeof ff)
                    if (c->h.bak_write(c->h.user, c->unit + o, ff, sizeof ff)) { finish(c, CF_D_FAILED); return; }
                c->unit += BANK;
            } else {
                c->step = 1;
                c->pos = c->unit;
                c->bank_crc = 0xFFFF;
            }
        } else if (c->step == 1) {
            if (res != CF_R_OK || n != c->last.len) {
                if (++c->bank_retry > 3) { finish(c, CF_D_FAILED); return; }
                c->n_retries++;
                plan_next(c);
                return;
            }
            if (c->h.bak_write(c->h.user, c->pos, r, n)) {
                logf_(c, "cf: the backup does not fit on this Chromatic: export it over USB first");
                finish(c, CF_D_FAILED);
                return;
            }
            c->bank_crc = cf_crc16(c->bank_crc, r, n);
            c->n_read_bytes += n;
            c->pos += n;
        } else {
            const uint16_t got = (uint16_t)(n >= 2 ? r[0] | (r[1] << 8) : 0);
            if (res != CF_R_OK || got != c->bank_crc) {
                logf_(c, "cf: backup bank %u crc mismatch (%04x != %04x): read again", (unsigned)(c->unit / BANK),
                      got, c->bank_crc);
                if (++c->bank_retry > 2) { finish(c, CF_D_FAILED); return; }
                c->n_retries++;
                c->pos = c->unit;
                c->step = 1;
                c->bank_crc = 0xFFFF;
                plan_next(c);
                return;
            }
            c->step = 0;
            c->bank_retry = 0;
            c->unit += BANK;
        }
        status(c, "BACKUP", c->unit, c->j.chip_size);
        plan_next(c);
        return;
    }
    case CF_PH_BAK_RAM: {
        const uint32_t kib = c->pos / 1024;
        if (res == CF_R_UNSTABLE && c->bank_retry < 1) { c->bank_retry++; plan_next(c); return; }
        if (res == CF_R_UNSTABLE) {                  /* floating: not memory, never a backup */
            uint8_t ff[1024];
            memset(ff, 0xFF, sizeof ff);
            if (c->h.ram_write(c->h.user, c->pos, ff, sizeof ff)) { finish(c, CF_D_FAILED); return; }
            c->j.ram_present &= ~(1u << kib);
        } else if (res != CF_R_OK || n != c->last.len) {
            if (++c->bank_retry > 3) { finish(c, CF_D_FAILED); return; }
            plan_next(c);
            return;
        } else {
            if (c->h.ram_write(c->h.user, c->pos, r, n)) { finish(c, CF_D_FAILED); return; }
            c->j.ram_present |= 1u << kib;
        }
        c->bank_retry = 0;
        c->pos += 1024;
        plan_next(c);
        return;
    }
    case CF_PH_WRITE:
    case CF_PH_VERIFY: {
        const uint8_t cmd = c->last.cmd;
        const uint32_t f = (uint32_t)c->last.bank * BANK + (c->last.addr - 0x4000u);
        if (cmd == CF_C_CRC || cmd == CF_C_BLANK) {
            int ok = res == CF_R_OK;
            if (ok && cmd == CF_C_CRC) {
                uint16_t want;
                tgt_all_ff(c, f, c->last.len, &want);
                ok = n >= 2 && (uint16_t)(r[0] | (r[1] << 8)) == want;
            }
            if (!ok && c->ph == CF_PH_WRITE && c->step == 2 && n >= 6) {
                /* blank check after an erase: a handful of bits that will not erase is left to
                   the program compare (it fails only if the data needs a 1 there); many
                   non-FF bytes mean a wrong erase opcode / geometry: stop. */
                const unsigned cnt = (unsigned)(r[4] | (r[5] << 8));
                if (cnt <= 64) {
                    logf_(c, "cf: %u byte(s) at %06x did not erase to FF: left to the program compare", cnt,
                          (unsigned)(f + (unsigned)(r[0] | (r[1] << 8))));
                    ok = 1;
                }
            }
            if (!ok) {
                c->piece_mismatch = 1;
                if (c->ph == CF_PH_VERIFY) {
                    const int s = plan_sector(c, c->oi);
                    logf_(c, "cf: verify: sector %d differs: rewrite", s);
                    done_set(c, s, 0);
                }
            }
            c->pos = f + c->last.len;
        } else if (cmd == CF_C_ERASE) {
            if (res == CF_R_TIMEOUT) {           /* the chip may still be busy: no further write */
                logf_(c, "cf: erase at %06x timed out: stopped (a write into a busy chip can suspend or corrupt)", (unsigned)f);
                finish(c, CF_D_FAILED);
                return;
            }
            if (res != CF_R_OK) {
                logf_(c, "cf: erase at %06x: result %u", (unsigned)f, res);
                c->n_retries++;
                if (++c->retry > c->opt.max_retries) { finish(c, CF_D_FAILED); return; }
                plan_next(c);
                return;
            }
            c->step = 2;
            c->pos = 0;
        } else if (cmd == CF_C_PROGRAM) {
            if (res == CF_R_TIMEOUT) {
                logf_(c, "cf: program at %06x timed out: stopped", (unsigned)f);
                finish(c, CF_D_FAILED);
                return;
            }
            if (res != CF_R_OK) {
                logf_(c, "cf: program at %06x: result %u%s", (unsigned)f, res, res == CF_R_MISMATCH ? " (verify)" : "");
                c->n_retries++;
                if (++c->retry > c->opt.max_retries) { finish(c, CF_D_FAILED); return; }
                c->step = 1;
                c->pos = 0;
                plan_next(c);
                return;
            }
            c->n_program_bytes += c->last.len;
            c->pos = f + c->last.len;
        }
        plan_next(c);
        return;
    }
    case CF_PH_RAM:
        if (!c->ram_kept) {
            if (res == CF_R_OK && n == 1024 && c->h.sav_write(c->h.user, c->pos, r, n) == 0) {
                c->save_crc = cf_crc32(c->save_crc, r, n);
                c->bank_retry = 0;
                c->pos += 1024;
            } else if (++c->bank_retry > 2) {
                logf_(c, "cf: the replaced game's save could not be read at %05x (result %u): not kept", (unsigned)c->pos, res);
                c->ram_kept = 1; c->pos = 0;
            }
            plan_next(c);
            return;
        }
        if (res != CF_R_OK) {
            if (++c->retry > c->opt.max_retries) { finish(c, CF_D_FAILED); return; }
            plan_next(c);
            return;
        }
        c->pos += c->last.len;
        plan_next(c);
        return;
    default:
        return;
    }
}

/* ---------------------------------------------------------------- the wire */
static uint16_t encode(cf_esp_t *c, uint8_t *o, uint16_t cap)
{
    const cf_cmd_t *m = &c->last;
    memset(o, 0, CF_ANS_DATA);
    o[0] = c->seq;
    o[1] = m->cmd;
    uint8_t *a = o + CF_ANS_HDR;
    a[0] = m->mbc;
    a[1] = (uint8_t)m->bank; a[2] = (uint8_t)(m->bank >> 8);
    a[3] = (uint8_t)m->addr; a[4] = (uint8_t)(m->addr >> 8);
    a[5] = (uint8_t)m->len;  a[6] = (uint8_t)(m->len >> 8);
    a[7] = (uint8_t)(m->flags | (c->opt.gb_timed ? CF_F_GBT : 0)); a[8] = m->cmdset; a[9] = m->op;
    a[10] = (uint8_t)m->tmo; a[11] = (uint8_t)(m->tmo >> 8);
    a[12] = m->cfi_mode;
    a[13] = (uint8_t)m->cfi_addr; a[14] = (uint8_t)(m->cfi_addr >> 8);
    uint16_t n = CF_ANS_DATA;
    if (m->src && m->len && n + m->len <= cap) {
        uint8_t *d = o + n;
        if (m->src == 1) tgt_read(c, m->src_off, d, m->len);
        else if (m->src == 2) c->h.ram_read(c->h.user, m->src_off, d, m->len);
        else if (m->src == 4) for (uint16_t i = 0; i < m->len; i++) d[i] = st_pattern(m->src_off + i);
        else if (m->src == 5) c->h.sav_read(c->h.user, m->src_off, d, m->len);
        else if (m->src == 6) for (uint16_t i = 0; i < m->len; i++) d[i] = ramtest_pattern(i);
        else if (m->src == 7 && c->tbuf) memcpy(d, c->tbuf, m->len);
        else if (m->src == 8 && c->sbuf) memcpy(d, c->sbuf, m->len);
        else if (m->src == 9 && m->len == 1) d[0] = c->wp_nv;      /* the write test's one byte */
        else memset(d, 0, m->len);
        n = (uint16_t)(n + m->len);
    }
    if (m->cmd == CF_C_WAIT) a[10] = c->xs && c->xs->on && c->ph == CF_PH_STREAM ? 50 : 20;   /* ms: how soon the writer asks again (express: the FPGA writes meanwhile) */
    return n;
}

uint8_t cf_esp_op(void *user, uint8_t op, const uint8_t *body, uint16_t blen,
                  uint8_t *out, uint16_t cap, uint16_t *out_len, uint32_t *total)
{
    cf_esp_t *c = (cf_esp_t *)user;
    *out_len = 0;
    c->last_traffic = c->now;
    if (op == NW_OP_CF_LOAD) {
        if (!c->armed && !c->probe_armed) return NW_S_DENIED;
        if (blen < 2) return NW_S_BAD_REQUEST;
        const uint8_t *img = cf_writer_bin;
        uint32_t size = sizeof cf_writer_bin;
        if (c->probe_armed) {
            img = c->probe_flashid ? cf_probe_flashid_bin : cf_probe_bin;
            size = c->probe_flashid ? sizeof cf_probe_flashid_bin : sizeof cf_probe_bin;
        }
        const uint32_t off = (uint32_t)(body[0] | (body[1] << 8));
        if (off > size) return NW_S_BAD_REQUEST;
        uint32_t k = size - off;
        if (k > cap) k = cap;
        memcpy(out, img + off, k);
        *out_len = (uint16_t)k;
        *total = size;
        return NW_S_OK;
    }
    if (op != NW_OP_CF_NEXT) return NW_S_UNSUPPORTED;
    if (blen < CF_REQ_HDR) return NW_S_BAD_REQUEST;
    const uint8_t seq = body[0], res = body[1];
    const uint16_t rlen = (uint16_t)(body[2] | (body[3] << 8));
    if (rlen > blen - CF_REQ_HDR) return NW_S_BAD_REQUEST;
    const uint8_t *r = body + CF_REQ_HDR;
    c->n_cmds++;
    if (!c->armed) {
        cmd_done(c, CF_D_NOJOB);
        c->done_pending = 0;             /* nothing armed: leave the FPGA alone */
    } else if (res == CF_R_HELLO) {
        /* No cart access before the FPGA itself says the patch overlay is off. The writer has
           just started (the core left its reset), so a report asked for NOW is from after the
           power cycle that ended the overlay. Until it comes the writer is told to say hello
           again; 3 s without a clean report: refused, nothing was read or written. */
        int go = 1;
        if (c->h.overlay_state && c->ovl_ok < 2) {
            if (c->ovl_ok == 0) { c->ovl_ok = 1; c->ovl_t0 = c->now; (void)c->h.overlay_state(c->h.user, 1); go = 0; }
            else {
                const int st = c->h.overlay_state(c->h.user, 0);
                if (st == 0) c->ovl_ok = 2;
                else if (c->now - c->ovl_t0 > 3000u) {
                    logf_(c, "cf: the FPGA %s: no cart job runs under a patch; refused, the cart was not touched",
                          st == 1 ? "still serves the game through its patch overlay" : "did not report its patch overlay's state");
                    go = -1;
                } else go = 0;
            }
        }
        if (go > 0) on_hello(c, r, rlen);
        else if (go < 0) finish(c, CF_D_OVERLAY);
        else { memset(&c->last, 0, sizeof c->last); c->last.cmd = CF_C_HELLO; c->seq = 0; }
        if (go > 0 && c->h.overlay_state && c->armed && c->ph != CF_PH_DONE) {
            /* the belt for the jobs that WRITE what a computer sends (a backup made under a patch
               must never be baked into a cart): a second, fresh report is asked for now and must
               say "off" when the write is about to begin (ovl_write_ok) */
            c->ovl_ok = 3;
            c->ovl_t0 = c->now;
            (void)c->h.overlay_state(c->h.user, 1);
        }
    } else if (!c->session) {
        memset(&c->last, 0, sizeof c->last);
        c->last.cmd = CF_C_HELLO;
        c->seq = 0;
    } else if (seq == c->seq && c->ph != CF_PH_DONE) {
        c->prev_seq = c->seq;
        c->seq = (uint8_t)(c->seq == 255 ? 1 : c->seq + 1);
        on_result(c, res, r, rlen);
    } else if (seq == c->prev_seq || c->ph == CF_PH_DONE) {
        c->n_dup++;                      /* our answer was lost: the same command again */
    } else {
        memset(&c->last, 0, sizeof c->last);
        c->last.cmd = CF_C_HELLO;
        c->session = 0;
    }
    *out_len = encode(c, out, cap);
    *total = *out_len;
    return NW_S_OK;
}

void cf_esp_tick(cf_esp_t *c, uint32_t now_ms)
{
    c->now = now_ms;
    if (c->done_pending && now_ms - c->done_at > 300) {
        c->done_pending = 0;
        c->armed = 0;
        c->session = 0;
        if (c->h.engine_allow) c->h.engine_allow(c->h.user, 0);
        if (c->h.boot_select) c->h.boot_select(c->h.user, 0);
        if (c->h.overlay_hold) c->h.overlay_hold(c->h.user, 0);   /* BEFORE the last cycle: the game boots as the owner set it */
        if (c->h.cart_cycle) c->h.cart_cycle(c->h.user);      /* GB reset with the cart: boot the result */
        status(c, c->st.result == CF_D_OK ? "DONE" : "STOPPED", 1, 1);
    }
    if (c->probe_armed && !c->probe_pending && (c->probe_end_pending || now_ms - c->last_traffic > 180000u)) {
        logf_(c, "cf: probe session %s (%u lines)", c->probe_end_pending ? "complete" : "timed out", (unsigned)c->probe_lines);
        c->probe_armed = 0;
        c->probe_end_pending = 0;
        free(c->pbuf); c->pbuf = NULL;
        if (c->h.boot_select) c->h.boot_select(c->h.user, 0);
        if (c->h.overlay_hold) c->h.overlay_hold(c->h.user, 0);
    }
    /* No cart writer at all (a stock FPGA: no engine, no alternate boot image) shows within
       seconds: 25 s without its first hello ends the job (result 0xEE = 238). A writer that
       was there and went silent gets 120 s, and then the cart is power-cycled back to its game. */
    const int never = c->ph == CF_PH_HELLO && !c->session;
    if (c->armed && !c->done_pending && now_ms - c->last_traffic > (never ? 25000u : 120000u)) {
        logf_(c, never ? "cf: no cart writer answered in 25 s (this FPGA cannot reach the cart): disarmed" : "cf: no writer traffic for 120 s: disarmed");
        if (!never && c->h.cart_cycle) c->want_cycle_after = 1;
        c->armed = 0;
        c->session = 0;
        if (c->h.engine_allow) c->h.engine_allow(c->h.user, 0);
        if (c->h.boot_select) c->h.boot_select(c->h.user, 0);
        if (c->h.overlay_hold) c->h.overlay_hold(c->h.user, 0);
        if (c->want_cycle_after) { c->want_cycle_after = 0; c->h.cart_cycle(c->h.user); }   /* every engine session ends in a GB reset */
    }
}

void cf_esp_init(cf_esp_t *c, const cf_host_t *h)
{
    memset(c, 0, sizeof *c);
    c->h = *h;
    jr_load(c);
}

void cf_esp_reboot(cf_esp_t *c)
{
    const cf_host_t h = c->h;
    free(c->xs); c->xs = NULL;
    cf_esp_init(c, &h);
}

int cf_esp_pending(const cf_esp_t *c, uint8_t *job, uint8_t *phase)
{
    if (job) *job = c->j.job;
    if (phase) *phase = c->j.phase;
    return c->j.phase == CF_PH_WRITE || c->j.phase == CF_PH_FAILED;
}

int cf_esp_arm(cf_esp_t *c, const cf_job_t *job)
{
    if (job->job == CF_JOB_RESTORE && !c->j.bak_ok) return CF_D_NO_BACKUP;
    if (job->job == CF_JOB_INSTALL && !c->h.img_size) return CF_D_BAD_IMAGE;
    if (cf_esp_pending(c, NULL, NULL) && job->job == CF_JOB_BACKUP) return CF_D_ABORTED;   /* resume or restore first */
    {
        const int pend = cf_esp_pending(c, NULL, NULL);
        const int spend = cf_esp_stream_pending(c, NULL, NULL, NULL);
        /* an unfinished streamed install: only the SAME image may go on; anything else waits for a cancel */
        if (spend && !(job->job == CF_JOB_STREAM && !strcmp(job->strm_id, c->j.strm_id) && job->strm_size == c->j.strm_size && job->strm_crc32 == c->j.strm_crc32)) return CF_D_ABORTED;
        if (pend && !spend && job->job >= CF_JOB_RAMDUMP) return CF_D_ABORTED;              /* never over a pending write */
    }
    if (job->job == CF_JOB_STREAM && (!job->strm_id[0] || job->strm_size < 0x8000u)) return CF_D_BAD_IMAGE;
    if (!c->h.power_ok || !c->h.power_ok(c->h.user)) return CF_D_ABORTED;
    c->opt = *job;
    if (!c->opt.max_retries) c->opt.max_retries = 2;
    if (c->j.job != job->job && cf_esp_pending(c, NULL, NULL)) c->j.n_power_resumes++;
    if (c->j.job == job->job && cf_esp_pending(c, NULL, NULL)) c->j.n_power_resumes++;
    if (job->job == CF_JOB_STREAM && !cf_esp_stream_pending(c, NULL, NULL, NULL)) {
        memset(c->j.strm_id, 0, sizeof c->j.strm_id);
        memset(c->j.strm_title, 0, sizeof c->j.strm_title);
        snprintf(c->j.strm_id, sizeof c->j.strm_id, "%s", job->strm_id);
        snprintf(c->j.strm_title, sizeof c->j.strm_title, "%s", job->strm_title);
        c->j.strm_src = job->strm_src;
        c->j.strm_size = job->strm_size;
        c->j.strm_crc32 = job->strm_crc32;
        memcpy(c->j.strm_sha, job->strm_sha, 32);
        c->j.strm_tries = 0;
    }
    /* the same unfinished image from another source (a 1.0.3-rc2 Wi-Fi install, ranged per chunk, now
       written from the staging area): the blocks already done stay done */
    if (job->job == CF_JOB_STREAM && cf_esp_stream_pending(c, NULL, NULL, NULL)) c->j.strm_src = job->strm_src;
    c->j.job = job->job;
    jr_save(c);
    c->armed = 1;
    c->session = 0;
    c->ph = CF_PH_HELLO;
    c->done_pending = 0;
    c->last_traffic = c->now;
    logf_(c, "cf: armed job %u%s", job->job, cf_esp_pending(c, NULL, NULL) ? " (continues an interrupted write)" : "");
    c->ovl_ok = 0;
    if (c->h.overlay_hold) c->h.overlay_hold(c->h.user, 1);     /* BEFORE the power cycle: the overlay ends with it */
    if (c->h.engine_allow) c->h.engine_allow(c->h.user, 1);
    if (c->h.boot_select) c->h.boot_select(c->h.user, 1);
    if (c->h.cart_cycle) c->h.cart_cycle(c->h.user);
    return 0;
}

/* ---------------------------------------------------------------- `cart probe` over USB */
/* the probe's work area (was ~9 KB of static RAM: the idle firmware needs that heap for TLS) */
#define CF_PB_FR     0u
#define CF_PB_FR_N   (4u + 3u + NW_TX_SIZE + 4u)
#define CF_PB_LINE   (CF_PB_FR + CF_PB_FR_N)
#define CF_PB_LINE_N (16u + 220u + 4u * (NW_TX_SIZE / 3u + 1u))
#define CF_PB_BODY   (CF_PB_LINE + CF_PB_LINE_N + 2u)
#define CF_PB_BODY_N (NW_RX_SIZE - NW_RESP_HDR)
#define CF_PB_TOTAL  (CF_PB_BODY + CF_PB_BODY_N)
static const char k_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_enc(const uint8_t *b, size_t n, char *o)
{
    size_t k = 0;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t)b[i] << 16 | (i + 1 < n ? (uint32_t)b[i + 1] << 8 : 0) | (i + 2 < n ? b[i + 2] : 0);
        o[k++] = k_b64[v >> 18 & 63];
        o[k++] = k_b64[v >> 12 & 63];
        o[k++] = i + 1 < n ? k_b64[v >> 6 & 63] : '=';
        o[k++] = i + 2 < n ? k_b64[v & 63] : '=';
    }
    o[k] = 0;
    return k;
}

static int b64_val(char ch)
{
    const char *p = ch ? strchr(k_b64, ch) : NULL;
    return p ? (int)(p - k_b64) : -1;
}

static int b64_dec(const char *s, uint8_t *o, size_t cap)
{
    size_t k = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (; *s && *s != ' ' && *s != '\n' && *s != '\r'; s++) {
        if (*s == '=') break;
        const int v = b64_val(*s);
        if (v < 0) return -1;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (k >= cap) return -1;
            o[k++] = (uint8_t)(acc >> bits);
        }
    }
    return (int)k;
}

int cf_esp_arm_probe(cf_esp_t *c, int flashid)
{
    if (c->armed) return CF_D_ABORTED;                      /* a cart job owns the writer */
    if (!c->h.host_line) return CF_D_ABORTED;
    if (!c->pbuf) c->pbuf = malloc(CF_PB_TOTAL);          /* ~9 KB, only for the probe session */
    if (!c->pbuf) return CF_D_ABORTED;
    c->probe_armed = 1;
    c->probe_flashid = flashid ? 1 : 0;
    c->probe_pending = 0;
    c->probe_end_pending = 0;
    c->probe_lines = 0;
    c->last_traffic = c->now;
    logf_(c, "cf: probe armed (%s): netboot + cart power cycle", flashid ? "FLASH-ID build: writes ID command bytes" : "read-only");
    if (c->h.overlay_hold) c->h.overlay_hold(c->h.user, 1);     /* the probe reads with CPU reads too */
    if (c->h.boot_select) c->h.boot_select(c->h.user, 1);
    if (c->h.cart_cycle) c->h.cart_cycle(c->h.user);
    return 0;
}

int cf_probe_owns(void *user, const uint8_t *game_id)
{
    const cf_esp_t *c = (const cf_esp_t *)user;
    return c->probe_armed && !memcmp(game_id, CP_GAME_ID, 16);
}

void cf_probe_call(void *user, nw_esp_t *e, uint8_t id, const nw_call_t *k)
{
    cf_esp_t *c = (cf_esp_t *)user;
    c->last_traffic = c->now;
    if (k->op == NW_OP_GET && !strcmp(k->path, "cp/probe.bin")) {   /* served here, never over the line */
        const uint8_t *img = c->probe_flashid ? cf_probe_flashid_bin : cf_probe_bin;
        const uint16_t n = (uint16_t)(c->probe_flashid ? sizeof cf_probe_flashid_bin : sizeof cf_probe_bin);
        nw_esp_complete(e, id, NW_S_OK, 0, n, img, n);
        return;
    }
    const int bulk = k->op == NW_OP_POST && (!strcmp(k->path, "cp/r") || !strcmp(k->path, "cp/x"));
    if (bulk && c->h.host_frame) {
        if (!strcmp(k->path, "cp/r")) {                 /* [kind, bank, off, len, sum16, rr] + data */
            uint16_t sum = 0;
            const uint16_t len = k->body_len >= 10 ? (uint16_t)(k->body[5] | k->body[6] << 8) : 0xFFFF;
            if (len == 0xFFFF || 10u + len != k->body_len) { nw_esp_complete(e, id, NW_S_BAD_REQUEST, 0, 0, NULL, 0); return; }
            for (uint16_t i = 0; i < len; i++) sum = (uint16_t)(sum + k->body[10 + i]);
            if (sum != (uint16_t)(k->body[7] | k->body[8] << 8)) {
                c->probe_bad_sums++;                    /* the probe sends it again */
                nw_esp_complete(e, id, NW_S_BAD_REQUEST, 0, 0, NULL, 0);
                return;
            }
        }
        uint8_t *const fr = c->pbuf + CF_PB_FR;
        if (!c->pbuf) { nw_esp_complete(e, id, NW_S_SERVER_ERROR, 0, 0, NULL, 0); return; }
        const uint16_t n = k->body_len;
        memcpy(fr, "\xA5" "CPB", 4);
        fr[4] = (uint8_t)k->path[3];
        fr[5] = (uint8_t)n; fr[6] = (uint8_t)(n >> 8);
        memcpy(fr + 7, k->body, n);
        const uint32_t crc = cf_crc32(0, fr + 4, 3u + n);
        fr[7 + n] = (uint8_t)crc; fr[8 + n] = (uint8_t)(crc >> 8); fr[9 + n] = (uint8_t)(crc >> 16); fr[10 + n] = (uint8_t)(crc >> 24);
        nw_esp_complete(e, id, NW_S_OK, 0, 2, (const uint8_t *)"OK", 2);   /* answer first: the GB reads on */
        c->probe_frames++;
        c->h.host_frame(c->h.user, fr, 11u + n);
        return;
    }
    if (!c->pbuf) { nw_esp_complete(e, id, NW_S_SERVER_ERROR, 0, 0, NULL, 0); return; }
    char *const line = (char *)c->pbuf + CF_PB_LINE;         /* room for the host's "\n" after it */
    const int h = snprintf(line, CF_PB_LINE_N, "CPX %u %c %s ", id, k->op == NW_OP_GET ? 'G' : 'P', k->path);
    b64_enc(k->body, k->op == NW_OP_POST ? k->body_len : 0, line + h);
    c->probe_esp = e;
    c->probe_id = id;
    c->probe_pending = 1;
    if (!strcmp(k->path, "cp/e")) c->probe_end_pending = 1;
    c->probe_lines++;
    c->h.host_line(c->h.user, line);
}

int cf_esp_host_answer(cf_esp_t *c, const char *args)
{
    unsigned id = 0, st = 0;
    int used = 0;
    if (sscanf(args, "%u %u %n", &id, &st, &used) < 2) return -1;
    if (!c->probe_pending || id != c->probe_id || !c->probe_esp) return -1;   /* stale: the window timed it out */
    if (!c->pbuf) return -1;
    uint8_t *const body = c->pbuf + CF_PB_BODY;
    const int n = b64_dec(args + used, body, CF_PB_BODY_N);
    if (n < 0) return -1;
    c->probe_pending = 0;
    c->last_traffic = c->now;
    nw_esp_complete(c->probe_esp, (uint8_t)id, (uint8_t)st, 0, (uint32_t)n, body, (uint16_t)n);
    return 0;
}
