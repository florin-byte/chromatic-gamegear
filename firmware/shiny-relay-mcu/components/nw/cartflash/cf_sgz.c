/* cf_sgz.c: SGZ1 unpack (lane gamecomp1; the format: cf_sgz.h). The inflater is cf_inflate.c (small state, output in
 * pieces), the same code on the unit and in the Mac host tests. SHA-256: mbedtls on the unit (the hardware engine),
 * cf_sha256 on the host. */
#include <stdlib.h>
#include <string.h>

#include "cf_sgz.h"

#include "cf_inflate.h"
#ifdef ESP_PLATFORM
#include "mbedtls/sha256.h"
typedef mbedtls_sha256_context sgz_sha_t;
static void sha_init(sgz_sha_t *s) { mbedtls_sha256_init(s); (void)mbedtls_sha256_starts(s, 0); }
static void sha_upd(sgz_sha_t *s, const uint8_t *b, uint32_t n) { (void)mbedtls_sha256_update(s, b, n); }
static void sha_fin(sgz_sha_t *s, uint8_t d[32]) { (void)mbedtls_sha256_finish(s, d); mbedtls_sha256_free(s); }
#else
#include "cf_sha256.h"
typedef cf_sha256_t sgz_sha_t;
static void sha_init(sgz_sha_t *s) { cf_sha256_init(s); }
static void sha_upd(sgz_sha_t *s, const uint8_t *b, uint32_t n) { cf_sha256_update(s, b, n); }
static void sha_fin(sgz_sha_t *s, uint8_t d[32]) { cf_sha256_final(s, d); }
#endif

#ifdef ESP_PLATFORM
#include "esp_rom_crc.h"
/* the chip's ROM crc32 (zlib's: the same result as cf_crc32) */
static uint32_t crc_z(uint32_t crc, const uint8_t *b, uint32_t n) { return esp_rom_crc32_le(crc, b, n); }
#else
uint32_t cf_crc32(uint32_t crc, const uint8_t *b, uint32_t n);   /* cf_esp.c: zlib's crc32 (table) */
static uint32_t crc_z(uint32_t crc, const uint8_t *b, uint32_t n) { return cf_crc32(crc, b, n); }
#endif
#ifdef ESP_PLATFORM
#include "esp_timer.h"
static int64_t now_us(void) { return esp_timer_get_time(); }
#else
#include <time.h>
static int64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }
#endif
cf_sgz_prof_t cf_sgz_prof;
volatile uint32_t g_sgz_mark;   /* gamecomp1: the block being unpacked + 1 (rc where) */
/* zlib's crc32_combine: the whole game's crc32 from the blocks' (no second pass over the bytes) */
static uint32_t gf2_times(const uint32_t *mat, uint32_t vec) { uint32_t sum = 0; while (vec) { if (vec & 1u) sum ^= *mat; vec >>= 1; mat++; } return sum; }
static void gf2_square(uint32_t *sq, const uint32_t *mat) { for (int n = 0; n < 32; n++) sq[n] = gf2_times(mat, mat[n]); }
/* the operator that moves a crc32 over len2 zero bytes, built once per length (a block length repeats: the cost is one
   32-step product per block, not the ~25k steps of a fresh combine) */
static struct { uint32_t len, op[32]; } _CombOp;
static uint32_t crc_combine_fast(uint32_t c1, uint32_t c2, uint32_t len2)
{
    if (!len2) return c1;
    if (_CombOp.len != len2) {
        for (int n = 0; n < 32; n++) _CombOp.op[n] = 0;
        /* column n of the operator = the zero-extension of the crc with only bit n set: crc_combine(1 << n, 0, len2) */
        extern uint32_t crc_combine_slow(uint32_t c1, uint32_t c2, uint32_t len2);
        for (int n = 0; n < 32; n++) _CombOp.op[n] = crc_combine_slow(1u << n, 0, len2);
        _CombOp.len = len2;
    }
    return gf2_times(_CombOp.op, c1) ^ c2;
}
uint32_t crc_combine_slow(uint32_t c1, uint32_t c2, uint32_t len2);
static uint32_t crc_combine(uint32_t c1, uint32_t c2, uint32_t len2) { return crc_combine_fast(c1, c2, len2); }
uint32_t crc_combine_slow(uint32_t c1, uint32_t c2, uint32_t len2)
{
    uint32_t even[32], odd[32], row = 1;
    if (!len2) return c1;
    odd[0] = 0xEDB88320u;
    for (int n = 1; n < 32; n++) { odd[n] = row; row <<= 1; }
    gf2_square(even, odd);
    gf2_square(odd, even);
    do {
        gf2_square(even, odd);
        if (len2 & 1u) c1 = gf2_times(even, c1);
        len2 >>= 1;
        if (!len2) break;
        gf2_square(odd, even);
        if (len2 & 1u) c1 = gf2_times(odd, c1);
        len2 >>= 1;
    } while (len2);
    return c1 ^ c2;
}                                 /* the last unpack's time, by part (microseconds) */
typedef struct { cf_sgz_read_fn rd; void *u; int64_t *t; } prof_rd_t;
static int prof_read(void *u, uint32_t off, uint8_t *b, uint32_t n) { prof_rd_t *p = u; const int64_t t = now_us(); const int e = p->rd(p->u, off, b, n); *p->t += now_us() - t; return e; }

#ifdef CF_SGZ_HOST_TEST                  /* (the Mac test: pieces over cf_sgz_test_piece_max refused, as a fragmented heap does) */
uint32_t cf_sgz_test_piece_max;
#define PIECE_ALLOC(n) ((cf_sgz_test_piece_max && (n) > cf_sgz_test_piece_max) ? NULL : malloc(n))
#else
#define PIECE_ALLOC(n) malloc(n)
#endif
#define SEG_SHIFT_MIN 10u              /* (1 KiB: the smallest piece a fragmented heap is asked for) */
#define SEG_SHIFT 11u                  /* the output window in pieces of 2 KiB (separate allocations: unit 1 at a play had 16.8 KB free, the largest block 6.4 KB) */
#define TBL_CHUNK 256u                 /* 32 table entries per read */
#define RAWBIT    0x80000000u

static uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int cf_sgz_is(const uint8_t *b, uint32_t n) { return n >= 4 && b[0] == 'S' && b[1] == 'G' && b[2] == 'Z' && b[3] == '1'; }

int cf_sgz_parse(const uint8_t *h, uint32_t total, cf_sgz_hdr_t *o)
{
    if (total < CF_SGZ_HDR || !cf_sgz_is(h, total)) return CF_SGZ_E_MAGIC;
    memset(o, 0, sizeof *o);
    if (h[4] != 1) return CF_SGZ_E_HEADER;
    o->shift = h[5];
    if (o->shift < 12 || o->shift > 14) return CF_SGZ_E_HEADER;
    o->nblocks = (uint16_t)(h[6] | h[7] << 8);
    o->rom_size = le32(h + 8);
    o->rom_crc32 = le32(h + 12);
    memcpy(o->sha256, h + 16, 32);
    memcpy(o->cart, h + 48, 28);
    o->data_size = le32(h + 76);
    if (o->rom_size < CF_SGZ_ROM_MIN || o->rom_size > CF_SGZ_ROM_MAX) return CF_SGZ_E_HEADER;
    const uint32_t bs = 1u << o->shift;
    if (o->nblocks != (o->rom_size + bs - 1u) >> o->shift) return CF_SGZ_E_HEADER;
    for (int i = 80; i < 92; i++) if (h[i]) return CF_SGZ_E_HEADER;
    o->data_start = CF_SGZ_HDR + 8u * o->nblocks;
    o->total = total;
    if ((uint64_t)o->data_start + o->data_size != total) return CF_SGZ_E_TABLE;
    return 0;
}

uint32_t cf_sgz_heap_need(uint8_t shift) { return (uint32_t)sizeof(cf_infl_t) + (uint32_t)sizeof(sgz_sha_t) + TBL_CHUNK + (1u << shift) + 64u; }

const char *cf_sgz_why(int e)
{
    switch (e) {
    case 0: return "ok";
    case CF_SGZ_E_READ: return "read";
    case CF_SGZ_E_WRITE: return "write";
    case CF_SGZ_E_MEM: return "memory";
    case CF_SGZ_E_MAGIC: return "not SGZ1";
    case CF_SGZ_E_HEADER: return "header";
    case CF_SGZ_E_HCRC: return "header crc";
    case CF_SGZ_E_TABLE: return "block table";
    case CF_SGZ_E_INFLATE: return "inflate";
    case CF_SGZ_E_BCRC: return "block crc";
    case CF_SGZ_E_ROM: return "rom hash";
    case CF_SGZ_E_BIG: return "block size";
    default: return "?";
    }
}

typedef struct {
    cf_infl_t inf;
    sgz_sha_t sha;
    uint8_t tbl[TBL_CHUNK];
    uint8_t *seg[8];                   /* the block's bytes in pieces of 2 KiB (16 KiB: eight) */
} sgz_work_t;

/* table entry i into (w, c), reading the table 32 entries at a time */
static int tbl_get(cf_sgz_read_fn rd, void *u, sgz_work_t *m, const cf_sgz_hdr_t *h, uint32_t i, uint32_t *at, uint32_t *w, uint32_t *c)
{
    const uint32_t first = i & ~31u;
    if (*at != first) {
        const uint32_t n = (h->nblocks - first < 32u ? h->nblocks - first : 32u) * 8u;
        if (rd(u, CF_SGZ_HDR + first * 8u, m->tbl, n)) return CF_SGZ_E_READ;
        *at = first;
    }
    *w = le32(m->tbl + (i - first) * 8u);
    *c = le32(m->tbl + (i - first) * 8u + 4u);
    return 0;
}

static int run(cf_sgz_read_fn rd_fast, cf_sgz_read_fn rd, cf_sgz_write_fn wr, void *u, uint32_t total, cf_sgz_hdr_t *hdr_out, void (*pct)(void *u, int p))
{
    /* rd: the header and the block table (and a block again); rd_fast: the blocks' bytes first (a reader that may be wrong
       now and then: each block that does not inflate or check is read again with rd) */
    uint8_t h[CF_SGZ_HDR];
    cf_sgz_hdr_t H;
    if (total < CF_SGZ_HDR) return CF_SGZ_E_MAGIC;
    memset(&cf_sgz_prof, 0, sizeof cf_sgz_prof);
    if (rd(u, 0, h, CF_SGZ_HDR)) return CF_SGZ_E_READ;
    int e = cf_sgz_parse(h, total, &H);
    if (e) return e;
    if (H.shift > CF_SGZ_SHIFT_MAX) return CF_SGZ_E_BIG;
    if (hdr_out) *hdr_out = H;
    const uint32_t bs = 1u << H.shift;
    uint8_t sh = H.shift < SEG_SHIFT ? H.shift : SEG_SHIFT;
    sgz_work_t *m = calloc(1, sizeof *m);
    if (!m) { cf_sgz_prof.mem_at = (uint32_t)sizeof *m; return CF_SGZ_E_MEM; }
    /* the pieces; a heap in fragments (unit 1, 18.31, 2026-10-22: a library play of an 8 KiB-block game, 15.7 KB free, the
       largest block 6 KiB, the 4th 2 KiB piece refused): pieces half as big, down to 1 KiB, while they fit the eight slots */
    for (;;) {
        const uint32_t ss_try = 1u << sh, n_try = bs >> sh;
        uint32_t got = 0;
        while (got < n_try && (m->seg[got] = PIECE_ALLOC(ss_try))) got++;
        if (got == n_try) break;
        for (uint32_t k = 0; k < got; k++) { free(m->seg[k]); m->seg[k] = NULL; }
        if (sh <= SEG_SHIFT_MIN || (bs >> (sh - 1u)) > sizeof m->seg / sizeof m->seg[0]) { free(m); cf_sgz_prof.mem_at = 0x80000000u | (got << 16) | ss_try; return CF_SGZ_E_MEM; }
        sh--;
    }
    const uint32_t ss = 1u << sh;
    cf_sgz_prof.seg_shift = sh;
    uint32_t at = ~0u;
    /* pass 1: the header crc32 over bytes 0..91 and the table, every length, their sum */
    uint32_t hc = crc_z(0, h, 92), sum = 0;
    for (uint32_t i = 0; i < H.nblocks && !e; i += 32u) {
        const uint32_t n = (H.nblocks - i < 32u ? H.nblocks - i : 32u) * 8u;
        if (rd(u, CF_SGZ_HDR + i * 8u, m->tbl, n)) { e = CF_SGZ_E_READ; break; }
        at = i;
        hc = crc_z(hc, m->tbl, n);
        for (uint32_t k = 0; k < n; k += 8u) {
            const uint32_t w = le32(m->tbl + k), b = i + k / 8u;
            const uint32_t len = b + 1u < H.nblocks ? bs : H.rom_size - b * bs;
            const uint32_t L = w & 0xFFFFFFu;
            if ((w & 0x7F000000u) || L < 1u || L > bs || ((w & RAWBIT) && L != len)) { e = CF_SGZ_E_TABLE; break; }
            sum += L;
        }
    }
    if (!e && hc != le32(h + 92)) e = CF_SGZ_E_HCRC;
    if (!e && sum != H.data_size) e = CF_SGZ_E_TABLE;
    /* pass 2: the blocks */
    uint32_t src = H.data_start, rcrc = 0;
    const int sha_on = !e;
    if (sha_on) sha_init(&m->sha);
    for (uint32_t b = 0; b < H.nblocks && !e; b++) {
        g_sgz_mark = b + 1u;
        uint32_t w, c;
        if ((e = tbl_get(rd, u, m, &H, b, &at, &w, &c))) break;
        const uint32_t len = b + 1u < H.nblocks ? bs : H.rom_size - b * bs;
        const uint32_t L = w & 0xFFFFFFu;
        uint32_t bc = 0;
        int64_t tc = 0;
        for (int att = 0; att < 2; att++) {
            cf_sgz_read_fn r = (att || !rd_fast) ? rd : rd_fast;
            int be = 0;
            if (w & RAWBIT) {
                for (uint32_t o = 0; o < L && !be; o += ss) if (r(u, src + o, m->seg[o >> sh], L - o < ss ? L - o : ss)) be = CF_SGZ_E_READ;
            } else {
                int64_t trd = 0;
                prof_rd_t pr = { r, u, &trd };
                const int64_t t0 = now_us();
                const int ie = cf_inflate(&m->inf, prof_read, &pr, src, L, m->seg, sh, len);
                cf_sgz_prof.read_us += trd; cf_sgz_prof.inflate_us += now_us() - t0 - trd;
                if (ie) be = CF_SGZ_E_INFLATE;
            }
            tc = now_us();
            bc = 0;
            if (!be) for (uint32_t o = 0; o < len; o += ss) bc = crc_z(bc, m->seg[o >> sh], len - o < ss ? len - o : ss);
            if (!be && bc != c) be = CF_SGZ_E_BCRC;
            if (!be) break;
            if (att || r == rd) { e = be; cf_sgz_prof.fail_block = b; cf_sgz_prof.fail_src = src; cf_sgz_prof.fail_len = L; break; }
            cf_sgz_prof.retries++;                           /* (read again, the checked way) */
        }
        if (e) break;
        rcrc = crc_combine(rcrc, bc, len);               /* (the block's crc32, already made: no second pass) */
        src += L;
        for (uint32_t o = 0; o < len && !e; o += ss) {
            const uint32_t k = len - o < ss ? len - o : ss;
            const int64_t ts = now_us();
            sha_upd(&m->sha, m->seg[o >> sh], k);
            cf_sgz_prof.sha_us += now_us() - ts;
            const int64_t tw = now_us();
            if (!o) cf_sgz_prof.check_us += tw - tc;         /* (the block's crc32 + the first piece's sha256; later pieces' sha256 in sha_us) */
            if (wr && wr(u, b * bs + o, m->seg[o >> sh], k)) e = CF_SGZ_E_WRITE;
            cf_sgz_prof.write_us += now_us() - tw;
        }
        if (e) break;
        if (pct) pct(u, (int)((b + 1u) * 100u / H.nblocks));
    }
    if (sha_on) {
        uint8_t d[32];
        sha_fin(&m->sha, d);
        if (!e && (rcrc != H.rom_crc32 || memcmp(d, H.sha256, 32))) e = CF_SGZ_E_ROM;
    }
    for (int i = 0; i < 8; i++) free(m->seg[i]);
    free(m);
    return e;
}

int cf_sgz_unpack(cf_sgz_read_fn rd, cf_sgz_write_fn wr, void *u, uint32_t total, cf_sgz_hdr_t *hdr_out, void (*pct)(void *u, int p))
{
    return run(NULL, rd, wr, u, total, hdr_out, pct);
}
int cf_sgz_unpack2(cf_sgz_read_fn rd_fast, cf_sgz_read_fn rd, cf_sgz_write_fn wr, void *u, uint32_t total, cf_sgz_hdr_t *hdr_out, void (*pct)(void *u, int p))
{
    return run(rd_fast, rd, wr, u, total, hdr_out, pct);
}

int cf_sgz_verify(cf_sgz_read_fn rd, void *u, uint32_t total, cf_sgz_hdr_t *hdr_out) { return run(NULL, rd, NULL, u, total, hdr_out, NULL); }
