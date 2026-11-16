/* cf_sgz_test.c: SGZ1 unpack (cf_sgz.c) against the golden vectors (host/sgz_golden, made by host/sgz.py;
 * the backend and the cart page test the same files). Extra arguments: more .sgz/.rom pairs (real games).
 *   ./cf_sgz_test sgz_golden [a.sgz a.rom ...]
 * Built twice: cf_sgz_test (CF_SGZ_SHIFT_MAX 14: every block size the format has) and cf_sgz_test_unit (the
 * unit's 13: a 16 KiB container is refused with "block size"). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cf_sgz.h"
#include "cf_inflate.h"
#include <zlib.h>

static int pass, fail;
#define CHECK(c, ...) do { if (c) pass++; else { fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t *load(const char *p, uint32_t *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long k = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)k + 1);
    if (fread(b, 1, (size_t)k, f) != (size_t)k) { fclose(f); free(b); return NULL; }
    fclose(f);
    *n = (uint32_t)k;
    return b;
}

typedef struct {
    const uint8_t *z; uint32_t zn;
    uint8_t *out; uint32_t cap, wrote, next;
    int fail_write_at;                 /* the nth write fails (0: none) */
    unsigned nreads, nwrites;
} io_t;
static int rd(void *u, uint32_t off, uint8_t *b, uint32_t n)
{
    io_t *s = u;
    s->nreads++;
    if ((uint64_t)off + n > s->zn) return -1;
    memcpy(b, s->z + off, n);
    return 0;
}
static int wr(void *u, uint32_t off, const uint8_t *b, uint32_t n)
{
    io_t *s = u;
    s->nwrites++;
    if (s->fail_write_at && (int)s->nwrites == s->fail_write_at) return -1;
    if (off != s->next || (uint64_t)off + n > s->cap) return -1;    /* in order, never past the ROM */
    memcpy(s->out + off, b, n);
    s->next = off + n;
    s->wrote += n;
    return 0;
}

/* every 7th read of 64+ bytes comes back with one byte wrong (the fast reader of a library load at a short margin) */
int flaky_rd(void *u, uint32_t off, uint8_t *b, uint32_t n)
{
    static uint32_t k;
    const int e = rd(u, off, b, n);
    if (!e && n >= 64 && (++k % 7u) == 0) b[n / 2] ^= 0x5A;
    return e;
}
static int unpack_file(const uint8_t *z, uint32_t zn, uint8_t *out, uint32_t cap, cf_sgz_hdr_t *h, io_t *s)
{
    memset(s, 0, sizeof *s);
    s->z = z; s->zn = zn; s->out = out; s->cap = cap;
    return cf_sgz_unpack(rd, wr, s, zn, h, NULL);
}

static void ok_pair(const char *zp, const char *rp, int expect_big)
{
    uint32_t zn = 0, rn = 0;
    uint8_t *z = load(zp, &zn), *r = load(rp, &rn);
    CHECK(z && r, "load %s %s", zp, rp);
    if (!z || !r) return;
    uint8_t *out = calloc(1, rn);
    cf_sgz_hdr_t h;
    io_t s;
    const clock_t t0 = clock();
    const int e = unpack_file(z, zn, out, rn, &h, &s);
    const double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
    if (expect_big) { CHECK(e == CF_SGZ_E_BIG && s.wrote == 0, "%s: e=%d (%s), want block size", zp, e, cf_sgz_why(e)); }
    else {
        CHECK(e == 0, "%s: e=%d (%s)", zp, e, cf_sgz_why(e));
        CHECK(e != 0 || (s.wrote == rn && !memcmp(out, r, rn)), "%s: the ROM differs", zp);
        CHECK(e != 0 || (h.rom_size == rn), "%s: header rom_size", zp);
        printf("ok %s: %u -> %u bytes (%.3f), %u blocks of %u, %u reads, %.1f ms on this Mac\n", zp, zn, rn, (double)zn / rn, h.nblocks, 1u << h.shift, s.nreads, ms);
        /* verify alone: same answer, no writes */
        CHECK(cf_sgz_verify(rd, &s, zn, NULL) == 0, "%s: verify", zp);
        /* a sink that fails halfway: an error, never past it */
        if (h.nblocks > 1) {
            memset(out, 0, rn);
            s.next = s.wrote = s.nwrites = 0; s.fail_write_at = 2;
            const int e2 = cf_sgz_unpack(rd, wr, &s, zn, NULL, NULL);
            CHECK(e2 == CF_SGZ_E_WRITE && s.wrote == 2048u,   /* (written in pieces of 2 KiB) */ "%s: a failing sink e=%d wrote %u", zp, e2, s.wrote);
        }
        /* a fast reader that is wrong now and then (a stale window): every block that does not check is read again */
        {
            int bad_reads = 0;
            io_t fs;
            memset(&fs, 0, sizeof fs); fs.z = z; fs.zn = zn; fs.out = out; fs.cap = rn;
            memset(out, 0, rn);
            cf_sgz_read_fn flaky = NULL;
            (void)flaky;
            extern int flaky_rd(void *u, uint32_t off, uint8_t *b, uint32_t n);
            (void)bad_reads;
            const int e4 = cf_sgz_unpack2(flaky_rd, rd, wr, &fs, zn, NULL, NULL);
            CHECK(e4 == 0 && fs.wrote == rn && !memcmp(out, r, rn), "%s: a flaky fast reader: e=%d, %u blocks read again", zp, e4, cf_sgz_prof.retries);
        }
        /* a heap in fragments: no piece over 1 KiB -> the unpack takes 1 KiB pieces, same bytes; none over 512 -> out of memory */
        {
            extern uint32_t cf_sgz_test_piece_max;
            io_t ps;
            memset(out, 0, rn);
            cf_sgz_test_piece_max = 1024;
            const int e5 = unpack_file(z, zn, out, rn, NULL, &ps);
            if (h.shift > 13u) CHECK(e5 == CF_SGZ_E_MEM && ps.wrote == 0, "%s: 16 KiB blocks in 1 KiB pieces (16 > 8 slots): e=%d", zp, e5);
            else CHECK(e5 == 0 && ps.wrote == rn && !memcmp(out, r, rn), "%s: 1 KiB pieces: e=%d (%s)", zp, e5, cf_sgz_why(e5));
            CHECK(e5 != 0 || h.shift < 11u || cf_sgz_prof.seg_shift == 10u, "%s: 1 KiB pieces: seg_shift %u", zp, cf_sgz_prof.seg_shift);
            cf_sgz_test_piece_max = 512;
            const int e6 = unpack_file(z, zn, out, rn, NULL, &ps);
            CHECK(h.shift < 10u || (e6 == CF_SGZ_E_MEM && ps.wrote == 0), "%s: no piece fits: e=%d wrote %u", zp, e6, ps.wrote);
            cf_sgz_test_piece_max = 0;
        }
        /* a source shorter than said: refused before any write */
        io_t t;
        memset(out, 0, rn);
        const int e3 = unpack_file(z, zn - 1, out, rn, NULL, &t);
        CHECK(e3 != 0 && t.wrote == 0, "%s: a short container e=%d wrote %u", zp, e3, t.wrote);
    }
    free(out); free(z); free(r);
}

static void bad_file(const char *zp, int want)
{
    uint32_t zn = 0;
    uint8_t *z = load(zp, &zn);
    CHECK(z != NULL, "load %s", zp);
    if (!z) return;
    uint8_t *out = calloc(1, 0x40000);
    io_t s;
    const int e = unpack_file(z, zn, out, 0x40000, NULL, &s);
    CHECK(e == want, "%s: e=%d (%s), want %d (%s)", zp, e, cf_sgz_why(e), want, cf_sgz_why(want));
    /* refusals found in the header / table write nothing; a block's own failure only what came before it */
    if (want == CF_SGZ_E_MAGIC || want == CF_SGZ_E_HEADER || want == CF_SGZ_E_HCRC || want == CF_SGZ_E_TABLE) CHECK(s.wrote == 0, "%s wrote %u", zp, s.wrote);
    printf("refused %s: %s\n", zp, cf_sgz_why(e));
    free(out); free(z);
}


/* cf_inflate against zlib: random data of many shapes, every level 0..9 and strategy, raw deflate; each must decode
   byte for byte into 8 KiB pieces; then corrupted / truncated / padded streams must be refused (never past the output) */
typedef struct { const uint8_t *p; uint32_t n; } mem_t;
static int mrd(void *u, uint32_t off, uint8_t *b, uint32_t n) { const mem_t *m = u; if ((uint64_t)off + n > m->n) return -1; memcpy(b, m->p + off, n); return 0; }
static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }
static void infl_fuzz(void)
{
    static uint8_t src[16384], z[20000], o0[8192], o1[8192];
    uint8_t *seg[2] = { o0, o1 };
    static cf_infl_t st;
    int ok = 0, bad = 0, refused = 0, nref = 0;
    for (int t = 0; t < 3000; t++) {
        const uint32_t n = 1 + rnd() % 16384u;
        const int shape = t % 6;
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t r = rnd();
            src[i] = shape == 0 ? (uint8_t)r : shape == 1 ? (uint8_t)(i * 7 + (i >> 9)) : shape == 2 ? (uint8_t)"GAME BOY TEXT "[i % 14]
                   : shape == 3 ? 0xFF : shape == 4 ? (uint8_t)((r & 0xF) == 0 ? r >> 8 : src[i ? i - 1 : 0]) : (uint8_t)(i < 64 ? r : src[i - 1 - (r % 64)]);
        }
        z_stream zs; memset(&zs, 0, sizeof zs);
        const int lvl = t % 10, strat = (t / 10) % 4;   /* default, filtered, huffman only, rle */
        deflateInit2(&zs, lvl, Z_DEFLATED, -15, 1 + (t % 9), strat == 0 ? Z_DEFAULT_STRATEGY : strat == 1 ? Z_FILTERED : strat == 2 ? Z_HUFFMAN_ONLY : Z_RLE);
        zs.next_in = src; zs.avail_in = n; zs.next_out = z; zs.avail_out = sizeof z;
        deflate(&zs, Z_FINISH);
        const uint32_t zn = (uint32_t)zs.total_out;
        deflateEnd(&zs);
        mem_t m = { z, zn };
        memset(o0, 0, sizeof o0); memset(o1, 0, sizeof o1);
        const int e = cf_inflate(&st, mrd, &m, 0, zn, seg, 13, n);
        int same = e == 0;
        for (uint32_t i = 0; same && i < n; i++) same = seg[i >> 13][i & 8191] == src[i];
        if (same) ok++; else { bad++; if (bad < 4) printf("  inflate: case %d (n %u, level %d, strategy %d): e=%d\n", t, n, lvl, strat, e); }
        /* refusals: one wrong length, a stream cut short, a byte added, a flipped bit somewhere (most are refused; none may overrun) */
        if (cf_inflate(&st, mrd, &m, 0, zn, seg, 13, n + 1) == 0) bad++;
        if (n > 1 && cf_inflate(&st, mrd, &m, 0, zn, seg, 13, n - 1) == 0) bad++;
        if (zn > 1 && cf_inflate(&st, mrd, &m, 0, zn - 1, seg, 13, n) == 0) bad++;
        z[zn] = 0; m.n = zn + 1;
        if (cf_inflate(&st, mrd, &m, 0, zn + 1, seg, 13, n) == 0) bad++;
        m.n = zn;
        z[rnd() % zn] ^= (uint8_t)(1u << (rnd() % 8));
        nref++;
        if (cf_inflate(&st, mrd, &m, 0, zn, seg, 13, n) != 0) refused++;
        if (st.out_pos > n) bad++;
    }
    CHECK(bad == 0, "cf_inflate: %d streams decoded byte for byte, %d wrong", ok, bad);
    printf("cf_inflate: %d zlib streams (levels 0-9, 4 strategies, 9 memLevels) byte for byte; wrong lengths, cut, padded refused; "
           "a flipped bit refused by the decoder itself %d of %d times (the rest: the block crc32 catches)\n", ok, refused, nref);
}

int main(int argc, char **argv)
{
    infl_fuzz();
    const char *d = argc > 1 ? argv[1] : "sgz_golden";
    char a[512], b[512];
    static const char *ok[] = { "g1_32k_s13", "g2_odd_s12", "g3_64k_s14", "g4_128k_s13" };
    for (unsigned i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        snprintf(a, sizeof a, "%s/%s.sgz", d, ok[i]);
        snprintf(b, sizeof b, "%s/%s.rom", d, ok[i]);
        ok_pair(a, b, CF_SGZ_SHIFT_MAX < 14 && strstr(ok[i], "_s14") != NULL);
    }
    static const struct { const char *n; int e; } bad[] = {
        { "x1_magic", CF_SGZ_E_MAGIC }, { "x2_version", CF_SGZ_E_HEADER }, { "x3_header_crc", CF_SGZ_E_HCRC },
        { "x4_block_data", CF_SGZ_E_INFLATE }, { "x5_block_crc", CF_SGZ_E_BCRC }, { "x6_sha256", CF_SGZ_E_ROM },
        { "x7_truncated", CF_SGZ_E_TABLE }, { "x8_trailing", CF_SGZ_E_TABLE }, { "x9_raw_length", CF_SGZ_E_TABLE },
        { "xa_flags", CF_SGZ_E_HEADER },
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        snprintf(a, sizeof a, "%s/%s.sgz", d, bad[i].n);
        bad_file(a, bad[i].e);
    }
    for (int i = 2; i + 1 < argc; i += 2) ok_pair(argv[i], argv[i + 1], 0);
    printf("heap for an unpack (cf_inflate state %u bytes, its 1 KiB input included): 4 KiB blocks %u, 8 KiB %u, 16 KiB %u (in pieces of at most 8 KiB)\n", (unsigned)sizeof(cf_infl_t),
           cf_sgz_heap_need(12), cf_sgz_heap_need(13), cf_sgz_heap_need(14));
    printf("cf_sgz: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
