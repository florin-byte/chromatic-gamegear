/* cf_pack_test.c (lane ui110dev): the unit's SGZ1 packer (cf_pack.c). Every container it makes is
 *   - written into a sink that takes each byte ONCE (as flash does) and refuses anything outside the container,
 *   - unpacked by the unit's own unpacker (cf_sgz.c: every check: header crc, table, block crc, ROM crc + sha256),
 *   - and each DEFLATE block inflated by zlib too (a second decoder: the backend's and the page's are zlib-like),
 * for synthetic ROMs (zeros, noise, text, patterns, every length edge, 4 / 8 / 16 KiB blocks, chains on and off) and
 * for the real games given as arguments (their size against sgz.py's zlib level 9 is printed).
 *   ./cf_pack_test [ROM ...] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

#include "cf_pack.h"
#include "cf_sgz.h"

static int pass, fail;
#define CHECK(c, ...) do { if (c) pass++; else { fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct {
    const uint8_t *rom; uint32_t rn;
    uint8_t *z; uint8_t *seen; uint32_t cap;
    int twice, outside, fail_read_at, fail_write_at;
    unsigned nreads, nwrites;
} sink_t;
static int src_rd(void *u, uint32_t off, uint8_t *b, uint32_t n)
{
    sink_t *s = u;
    s->nreads++;
    if (s->fail_read_at && (int)s->nreads == s->fail_read_at) return -1;
    if ((uint64_t)off + n > s->rn) return -1;
    memcpy(b, s->rom + off, n);
    return 0;
}
static int sink_wr(void *u, uint32_t off, const uint8_t *b, uint32_t n)
{
    sink_t *s = u;
    s->nwrites++;
    if (s->fail_write_at && (int)s->nwrites == s->fail_write_at) return -1;
    if ((uint64_t)off + n > s->cap) { s->outside++; return -1; }
    for (uint32_t i = 0; i < n; i++) { if (s->seen[off + i]) s->twice++; s->seen[off + i] = 1; s->z[off + i] = b[i]; }
    return 0;
}
typedef struct { const uint8_t *z; uint32_t zn; uint8_t *out; uint32_t cap, next; } io_t;
static int io_rd(void *u, uint32_t off, uint8_t *b, uint32_t n) { io_t *s = u; if ((uint64_t)off + n > s->zn) return -1; memcpy(b, s->z + off, n); return 0; }
static int io_wr(void *u, uint32_t off, const uint8_t *b, uint32_t n) { io_t *s = u; if (off != s->next || (uint64_t)off + n > s->cap) return -1; memcpy(s->out + off, b, n); s->next = off + n; return 0; }

static uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* zlib, block by block (an independent decoder of what the unit wrote) */
static int zlib_blocks(const uint8_t *z, uint32_t zn, const uint8_t *rom, uint32_t rn)
{
    const uint32_t shift = z[5], bs = 1u << shift, nb = z[6] | (uint32_t)z[7] << 8, ds = 96 + 8 * nb;
    uint32_t at = ds;
    uint8_t *o = malloc(bs + 16);
    for (uint32_t i = 0; i < nb; i++) {
        const uint32_t w = le32(z + 96 + 8 * i), L = w & 0xFFFFFFu, n = rn - i * bs < bs ? rn - i * bs : bs;
        if (at + L > zn) { free(o); return -1; }
        if (w & 0x80000000u) { if (L != n || memcmp(z + at, rom + i * bs, n)) { free(o); return -2; } }
        else {
            z_stream st;
            memset(&st, 0, sizeof st);
            if (inflateInit2(&st, -15) != Z_OK) { free(o); return -3; }
            st.next_in = (Bytef *)(z + at); st.avail_in = L; st.next_out = o; st.avail_out = bs + 16;
            const int r = inflate(&st, Z_FINISH);
            const uint32_t got = (uint32_t)st.total_out, used = (uint32_t)st.total_in;
            inflateEnd(&st);
            if (r != Z_STREAM_END || got != n || used != L || memcmp(o, rom + i * bs, n)) { free(o); return -4; }
        }
        at += L;
    }
    free(o);
    return at == zn ? 0 : -5;
}

static uint32_t zlib9_size(const uint8_t *rom, uint32_t rn, uint32_t shift)
{
    const uint32_t bs = 1u << shift, nb = (rn + bs - 1) / bs;
    uint32_t tot = 96 + 8 * nb;
    uint8_t *o = malloc(bs * 2 + 64);
    for (uint32_t i = 0; i < nb; i++) {
        const uint32_t n = rn - i * bs < bs ? rn - i * bs : bs;
        z_stream st;
        memset(&st, 0, sizeof st);
        deflateInit2(&st, 9, Z_DEFLATED, -15, 9, Z_DEFAULT_STRATEGY);
        st.next_in = (Bytef *)(rom + i * bs); st.avail_in = n; st.next_out = o; st.avail_out = bs * 2 + 64;
        deflate(&st, Z_FINISH);
        const uint32_t L = (uint32_t)st.total_out;
        deflateEnd(&st);
        tot += L < n ? L : n;
    }
    free(o);
    return tot;
}

/* the in-order sink (cf_pack_seq) */
typedef struct { const uint8_t *rom; uint32_t rn; uint8_t *z; uint32_t n, cap; uint32_t begun; int fail_begin; } seqs_t;
static int sq_rd(void *u, uint32_t off, uint8_t *b, uint32_t n) { seqs_t *q = u; if ((uint64_t)off + n > q->rn) return -1; memcpy(b, q->rom + off, n); return 0; }
static int sq_begin(void *u, uint32_t total) { seqs_t *q = u; q->begun = total; return q->fail_begin; }
static int sq_wr(void *u, const uint8_t *b, uint32_t n) { seqs_t *q = u; if (!q->begun || q->n + n > q->cap) return -1; memcpy(q->z + q->n, b, n); q->n += n; return 0; }

/* one pack + every check; *zsize the container's length (0: NOGAIN); returns cf_pack's result */
static int one(const char *name, const uint8_t *rom, uint32_t rn, uint8_t shift, uint32_t *zsize, int quiet)
{
    sink_t s;
    memset(&s, 0, sizeof s);
    s.rom = rom; s.rn = rn; s.cap = rn + 4096; s.z = calloc(1, s.cap); s.seen = calloc(1, s.cap);
    uint32_t total = 0;
    const clock_t c0 = clock();
    const int e = cf_pack(src_rd, sink_wr, &s, rn, shift, 0, &total, NULL);
    const double ms = (double)(clock() - c0) * 1000.0 / CLOCKS_PER_SEC;
    if (zsize) *zsize = e == 0 ? total : 0;
    if (e == CF_PACK_E_NOGAIN) { free(s.z); free(s.seen); return e; }
    CHECK(e == 0, "%s: pack %d", name, e);
    CHECK(s.twice == 0 && s.outside == 0, "%s: %d bytes written twice, %d outside", name, s.twice, s.outside);
    if (e) { free(s.z); free(s.seen); return e; }
    uint32_t holes = 0;
    for (uint32_t i = 0; i < total; i++) holes += !s.seen[i];
    CHECK(holes == 0, "%s: %u bytes of the container never written", name, holes);
    for (uint32_t i = total; i < s.cap; i++) if (s.seen[i]) { CHECK(0, "%s: written past the end", name); break; }
    /* the unit's unpacker */
    io_t io = { s.z, total, malloc(rn), rn, 0 };
    cf_sgz_hdr_t h;
    const int u = cf_sgz_unpack(io_rd, io_wr, &io, total, &h, NULL);
    CHECK(u == 0 && io.next == rn && !memcmp(io.out, rom, rn), "%s: cf_sgz_unpack %d (%s), %u of %u bytes", name, u, cf_sgz_why(u), io.next, rn);
    CHECK(cf_sgz_verify(io_rd, &io, total, NULL) == 0, "%s: cf_sgz_verify", name);
    CHECK(u != 0 || (h.rom_size == rn && h.shift == shift && !memcmp(h.cart, rom + 0x134, 28)), "%s: header fields", name);
    /* zlib */
    const int zr = zlib_blocks(s.z, total, rom, rn);
    CHECK(zr == 0, "%s: zlib block check %d", name, zr);
    /* in order (cf_pack_seq): the same bytes, begin told the length first */
    {
        seqs_t q = { rom, rn, malloc(rn + 4096), 0, rn + 4096, 0, 0 };
        uint32_t t2 = 0;
        const int e2 = cf_pack_seq(sq_rd, sq_begin, sq_wr, &q, rn, shift, 0, &t2, NULL);
        CHECK(e2 == 0 && t2 == total && q.begun == total && q.n == total && !memcmp(q.z, s.z, total), "%s: in order: %d, %u of %u bytes, the same container", name, e2, q.n, total);
        free(q.z);
    }
    if (!quiet) {
        const uint32_t z9 = zlib9_size(rom, rn, shift);
        printf("  %-18s %8u -> %8u (%.3f; zlib 9 %.3f, +%.1f %%) shift %u, blocks dyn %u fix %u raw %u, chains %u, %.0f ms\n", name, rn, total, (double)total / rn,
               (double)z9 / rn, ((double)total / z9 - 1) * 100, shift, cf_pack_prof.dyn, cf_pack_prof.fix, cf_pack_prof.raw, cf_pack_prof.chains, ms);
    }
    free(io.out); free(s.z); free(s.seen);
    return e;
}

static uint32_t rnd = 12345;
static uint32_t R(void) { rnd = rnd * 1103515245u + 12345u; return rnd >> 8; }
static void cart_header(uint8_t *r, uint32_t n)
{
    if (n < 0x150) return;
    memcpy(r + 0x134, "PACKTEST", 8);
    r[0x143] = 0x80; r[0x147] = 0x19; r[0x148] = 0; r[0x149] = 3;
}

int main(int argc, char **argv)
{
    printf("synthetic:\n");
    const uint32_t sizes[] = { 0x150, 0x151, 4095, 4096, 4097, 8191, 8192, 8193, 32768, 65536 + 17, 262144, 1048576 };
    for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
        const uint32_t n = sizes[si];
        uint8_t *r = malloc(n);
        for (int kind = 0; kind < 6; kind++) {
            for (uint32_t i = 0; i < n; i++) {
                switch (kind) {
                case 0: r[i] = 0; break;                                        /* zeros */
                case 1: r[i] = (uint8_t)R(); break;                             /* noise: NOGAIN / RAW */
                case 2: r[i] = (uint8_t)("THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG. "[i % 45]); break;
                case 3: r[i] = (uint8_t)(i * 7 ^ (i >> 5)); break;              /* a pattern */
                case 4: r[i] = (R() % 13 == 0) ? (uint8_t)R() : (uint8_t)(i & 0x3F); break;   /* mostly a ramp, noisy */
                default: r[i] = (uint8_t)(i % 251 < 120 ? i % 7 : R() & 3); break;            /* few symbols */
                }
            }
            cart_header(r, n);
            for (uint8_t sh = 12; sh <= 14; sh++) {
                char nm[64];
                snprintf(nm, sizeof nm, "k%d n%u s%u", kind, n, sh);
                uint32_t zs = 0;
                const int e = one(nm, r, n, sh, &zs, 1);
                if (kind == 1 && n >= 4096) CHECK(e == CF_PACK_E_NOGAIN, "%s: noise should not pack (%d)", nm, e);
                if (kind == 0 && n >= 32768) CHECK(e == 0 && zs < n / 50, "%s: zeros pack to %u", nm, zs);
            }
        }
        free(r);
    }
    /* without chains (their memory not there), lazy off, depth 1: still right */
    {
        const uint32_t n = 200000;
        uint8_t *r = malloc(n);
        for (uint32_t i = 0; i < n; i++) r[i] = (R() % 5 == 0) ? (uint8_t)R() : (uint8_t)("ABCDEFGHABCDXYZ"[i % 15]);
        cart_header(r, n);
        uint32_t a = 0, b = 0, c = 0;
        one("chains", r, n, 12, &a, 1);
        cf_pack_chains = 0; one("no chains", r, n, 12, &b, 1); cf_pack_chains = 1;
        cf_pack_lazy = 0; cf_pack_depth = 1; one("greedy d1", r, n, 12, &c, 1); cf_pack_lazy = 1; cf_pack_depth = 24;
        CHECK(a && b && c && a <= b, "chains %u, none %u, greedy %u", a, b, c);
        printf("  chains %u, no chains %u, greedy depth 1 %u (of %u)\n", a, b, c, n);
        /* a source that fails, a sink that fails: an error, never a container */
        sink_t s;
        memset(&s, 0, sizeof s);
        s.rom = r; s.rn = n; s.cap = n + 4096; s.z = calloc(1, s.cap); s.seen = calloc(1, s.cap);
        s.fail_read_at = 7;
        uint32_t t = 0;
        CHECK(cf_pack(src_rd, sink_wr, &s, n, 12, 0, &t, NULL) == CF_PACK_E_READ, "read failure");
        memset(s.seen, 0, s.cap); s.nreads = 0; s.nwrites = 0; s.fail_read_at = 0; s.fail_write_at = 9;
        CHECK(cf_pack(src_rd, sink_wr, &s, n, 12, 0, &t, NULL) == CF_PACK_E_WRITE, "write failure");
        memset(s.seen, 0, s.cap); s.nwrites = 0; s.fail_write_at = 0;
        CHECK(cf_pack(src_rd, sink_wr, &s, n, 12, 1000, &t, NULL) == CF_PACK_E_BIG, "max");
        CHECK(cf_pack(src_rd, sink_wr, &s, 0x100, 12, 0, &t, NULL) == CF_PACK_E_ARG, "too small");
        CHECK(cf_pack(src_rd, sink_wr, &s, n, 11, 0, &t, NULL) == CF_PACK_E_ARG, "shift 11");
        { seqs_t q = { r, n, malloc(n + 4096), 0, n + 4096, 0, 1 }; CHECK(cf_pack_seq(sq_rd, sq_begin, sq_wr, &q, n, 12, 0, &t, NULL) == CF_PACK_E_WRITE && q.n == 0, "in order: begin says no: nothing written"); free(q.z); }
        free(s.z); free(s.seen); free(r);
    }
    if (argc > 1) printf("games:\n");
    for (int a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        if (!f) { CHECK(0, "%s: cannot open", argv[a]); continue; }
        fseek(f, 0, SEEK_END);
        const long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *r = malloc((size_t)n);
        if (fread(r, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(r); CHECK(0, "%s: read", argv[a]); continue; }
        fclose(f);
        const char *nm = strrchr(argv[a], '/');
        uint32_t zs = 0;
        if (one(nm ? nm + 1 : argv[a], r, (uint32_t)n, 12, &zs, 0) == CF_PACK_E_NOGAIN) printf("  %-18s %8ld: NOGAIN (kept as it is)\n", nm ? nm + 1 : argv[a], n);
        free(r);
    }
    printf("cf_pack_test: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
