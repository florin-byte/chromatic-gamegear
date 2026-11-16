/* cf_pack.c: SGZ1 packing on the unit (lane ui110dev; cf_pack.h says what and why). Plain C: the same code in the Mac
 * host tests (host/cf_pack_test.c: every container unpacked by cf_sgz.c and by zlib, block for block). */
#include <stdlib.h>
#include <string.h>

#include "cf_pack.h"
#include "cf_sgz.h"

#ifdef ESP_PLATFORM
#include "mbedtls/sha256.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
typedef mbedtls_sha256_context pk_sha_t;
static void sha_init(pk_sha_t *s) { mbedtls_sha256_init(s); (void)mbedtls_sha256_starts(s, 0); }
static void sha_upd(pk_sha_t *s, const uint8_t *b, uint32_t n) { (void)mbedtls_sha256_update(s, b, n); }
static void sha_fin(pk_sha_t *s, uint8_t d[32]) { (void)mbedtls_sha256_finish(s, d); mbedtls_sha256_free(s); }
static uint32_t crc_z(uint32_t crc, const uint8_t *b, uint32_t n) { return esp_rom_crc32_le(crc, b, n); }
static int64_t now_us(void) { return esp_timer_get_time(); }
#else
#include <time.h>
#include "cf_sha256.h"
typedef cf_sha256_t pk_sha_t;
static void sha_init(pk_sha_t *s) { cf_sha256_init(s); }
static void sha_upd(pk_sha_t *s, const uint8_t *b, uint32_t n) { cf_sha256_update(s, b, n); }
static void sha_fin(pk_sha_t *s, uint8_t d[32]) { cf_sha256_final(s, d); }
uint32_t cf_crc32(uint32_t crc, const uint8_t *b, uint32_t n);   /* cf_esp.c: zlib's crc32 (table) */
static uint32_t crc_z(uint32_t crc, const uint8_t *b, uint32_t n) { return cf_crc32(crc, b, n); }
static int64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }
#endif
uint32_t crc_combine_slow(uint32_t c1, uint32_t c2, uint32_t len2);   /* cf_sgz.c: zlib's crc32_combine */

cf_pack_prof_t cf_pack_prof;
int cf_pack_depth = 24, cf_pack_lazy = 1, cf_pack_chains = 1;

enum { HBITS = 10, HSIZE = 1 << HBITS, MINM = 3, MAXM = 258, NICE = 96, NLIT = 288, NDIST = 32, NCL = 19, OBUF = 256 };

static const uint16_t kLenBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t kLenExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t kDistBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                        6145, 8193, 12289, 16385, 24577 };
static const uint8_t kDistExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
static const uint8_t kClOrder[NCL] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

typedef struct {
    const uint8_t *b;                   /* the block */
    uint32_t n;
    uint16_t *head, *prev;              /* (prev may be NULL: one candidate a position) */
    uint32_t ins;                       /* positions inserted: [0, ins) */
    int pass;                           /* 0 count, 1 write */
    uint32_t extra;                     /* pass 0: the extra bits of the lengths and distances */
    uint16_t lf[NLIT], df[NDIST], clf[NCL];
    uint8_t ll[NLIT], dl[NDIST], cll[NCL];
    uint16_t lc[NLIT], dc[NDIST], clc[NCL];
    uint8_t rle[NLIT + NDIST], rlx[NLIT + NDIST], seq[NLIT + NDIST];
    int nrle, hlit, hdist, hclen;
    /* scratch (heap with the rest: the card task's stack is 4 KiB) */
    uint16_t sym[NLIT];
    uint32_t A[NLIT];
    uint16_t df0[NDIST], lc_d[NLIT], dc_d[NDIST];
    uint8_t ll_d[NLIT], dl_d[NDIST];
    /* the writer */
    int (*wr)(void *u, uint32_t off, const uint8_t *b, uint32_t n);
    void *u;
    uint32_t at, bits, nbits, on;
    int werr;
    uint8_t ob[OBUF];
} pk_t;

/* ---- the matcher (deterministic: both passes see the same matches) */
static uint32_t hash3(const uint8_t *p) { return (((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]) * 2654435761u) >> (32 - HBITS); }
static void ins_to(pk_t *k, uint32_t p)
{
    while (k->ins < p && k->ins + MINM <= k->n) {
        const uint32_t q = k->ins++, h = hash3(k->b + q);
        if (k->prev) k->prev[q] = k->head[h];
        k->head[h] = (uint16_t)(q + 1);
    }
    if (k->ins < p) k->ins = p;
}
static void find(const pk_t *k, uint32_t p, int *len, int *dist)
{
    *len = 0; *dist = 0;
    if (p + MINM > k->n) return;
    const uint32_t lim = k->n - p < MAXM ? k->n - p : MAXM;
    uint32_t c = k->head[hash3(k->b + p)];
    int steps = k->prev ? cf_pack_depth : 1, best = MINM - 1;
    while (c && steps-- > 0) {
        const uint32_t q = c - 1;
        if (k->b[q + (uint32_t)best] == k->b[p + (uint32_t)best] && k->b[q] == k->b[p]) {
            uint32_t m = 0;
            while (m < lim && k->b[q + m] == k->b[p + m]) m++;
            if ((int)m > best) { best = (int)m; *len = best; *dist = (int)(p - q); if (m >= lim || best >= NICE) break; }
        }
        c = k->prev ? k->prev[q] : 0;
    }
}

/* ---- the writer: LSB first, into a small buffer, out to the sink at the container's offset */
static void flush_ob(pk_t *k)
{
    if (k->on && !k->werr && k->wr(k->u, k->at, k->ob, k->on)) k->werr = 1;
    k->at += k->on;
    k->on = 0;
}
static void putbits(pk_t *k, uint32_t v, uint32_t n)
{
    k->bits |= v << k->nbits;
    k->nbits += n;
    while (k->nbits >= 8) {
        k->ob[k->on++] = (uint8_t)k->bits;
        k->bits >>= 8; k->nbits -= 8;
        if (k->on == OBUF) flush_ob(k);
    }
}
static void putbytes(pk_t *k, const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) { k->ob[k->on++] = b[i]; if (k->on == OBUF) flush_ob(k); }
}

/* ---- the symbols */
static int len_sym(int len) { int i = 28; while (kLenBase[i] > len) i--; return i; }
static int dist_sym(int d) { int i = 29; while (kDistBase[i] > d) i--; return i; }
static void lit(pk_t *k, int c)
{
    if (k->pass == 0) k->lf[c]++;
    else putbits(k, k->lc[c], k->ll[c]);
}
static void match(pk_t *k, int len, int dist)
{
    const int ls = len_sym(len), ds = dist_sym(dist);
    if (k->pass == 0) { k->lf[257 + ls]++; k->df[ds]++; k->extra += kLenExtra[ls] + kDistExtra[ds]; return; }
    putbits(k, k->lc[257 + ls], k->ll[257 + ls]);
    if (kLenExtra[ls]) putbits(k, (uint32_t)(len - kLenBase[ls]), kLenExtra[ls]);
    putbits(k, k->dc[ds], k->dl[ds]);
    if (kDistExtra[ds]) putbits(k, (uint32_t)(dist - kDistBase[ds]), kDistExtra[ds]);
}
static void lz(pk_t *k)
{
    memset(k->head, 0, HSIZE * sizeof k->head[0]);
    k->ins = 0;
    uint32_t i = 0;
    while (i < k->n) {
        int L = 0, D = 0;
        ins_to(k, i);
        find(k, i, &L, &D);
        if (L >= MINM && cf_pack_lazy && L < NICE && i + 1 + MINM <= k->n) {
            int L2 = 0, D2 = 0;
            ins_to(k, i + 1);
            find(k, i + 1, &L2, &D2);
            if (L2 > L) { lit(k, k->b[i]); i++; L = L2; D = D2; }
        }
        if (L >= MINM) { match(k, L, D); i += (uint32_t)L; }
        else { lit(k, k->b[i]); i++; }
    }
    lit(k, 256);                                       /* end of block */
}

/* ---- Huffman codes: lengths from the counts (minimum redundancy, at most maxlen bits), then the canonical codes */
static void min_redundancy(uint32_t *A, int n)
{
    /* A: the counts in ascending order -> each one's code length (in place: Moffat & Katajainen, as miniz does) */
    if (n == 0) return;
    if (n == 1) { A[0] = 1; return; }
    int root = 0, leaf = 2, next;
    A[0] += A[1];
    for (next = 1; next < n - 1; next++) {
        if (leaf >= n || A[root] < A[leaf]) { A[next] = A[root]; A[root++] = (uint32_t)next; } else A[next] = A[leaf++];
        if (leaf >= n || (root < next && A[root] < A[leaf])) { A[next] += A[root]; A[root++] = (uint32_t)next; } else A[next] += A[leaf++];
    }
    A[n - 2] = 0;
    for (next = n - 3; next >= 0; next--) A[next] = A[A[next]] + 1;
    int avbl = 1, used = 0, dpth = 0;
    root = n - 2; next = n - 1;
    while (avbl > 0) {
        while (root >= 0 && (int)A[root] == dpth) { used++; root--; }
        while (avbl > used) { A[next--] = (uint32_t)dpth; avbl--; }
        avbl = 2 * used; dpth++; used = 0;
    }
}
static void build(pk_t *k, const uint16_t *freq, int nsym, int maxlen, uint8_t *len, uint16_t *code)
{
    uint16_t *const sym = k->sym;
    uint32_t *const A = k->A;
    int n = 0;
    memset(len, 0, (size_t)nsym);
    for (int s = 0; s < nsym; s++) {
        if (!freq[s]) continue;
        int j = n++;                                   /* insertion sort by count, then by symbol */
        while (j > 0 && freq[sym[j - 1]] > freq[s]) { sym[j] = sym[j - 1]; j--; }
        sym[j] = (uint16_t)s;
    }
    if (n == 1) { len[sym[0]] = 1; len[sym[0] ? 0 : 1] = 1; }   /* (one symbol: a complete code of two) */
    else if (n > 1) {
        for (int i = 0; i < n; i++) A[i] = freq[sym[i]];
        min_redundancy(A, n);
        int num[33] = { 0 };
        for (int i = 0; i < n; i++) num[A[i] > 32 ? 32 : A[i]]++;
        /* at most maxlen bits (miniz's rule: move codes up until the Kraft sum is exact) */
        for (int i = maxlen + 1; i <= 32; i++) { num[maxlen] += num[i]; num[i] = 0; }
        uint32_t total = 0;
        for (int i = maxlen; i > 0; i--) total += (uint32_t)num[i] << (maxlen - i);
        while (total != (1u << maxlen)) {
            num[maxlen]--;
            for (int i = maxlen - 1; i > 0; i--) if (num[i]) { num[i]--; num[i + 1] += 2; break; }
            total--;
        }
        int j = n;
        for (int i = 1; i <= maxlen; i++) for (int l = num[i]; l > 0; l--) len[sym[--j]] = (uint8_t)i;   /* (the most used: the shortest) */
    }
    /* canonical codes, bit-reversed (DEFLATE sends them from their top bit, the writer from the bottom) */
    uint16_t cnt[16] = { 0 }, next[16] = { 0 };
    for (int s = 0; s < nsym; s++) cnt[len[s]]++;
    cnt[0] = 0;
    for (int b = 1, c = 0; b < 16; b++) { c = (c + cnt[b - 1]) << 1; next[b] = (uint16_t)c; }
    for (int s = 0; s < nsym; s++) {
        if (!len[s]) { code[s] = 0; continue; }
        uint32_t c = next[len[s]]++, r = 0;
        for (int b = 0; b < len[s]; b++) { r = r << 1 | (c & 1u); c >>= 1; }
        code[s] = (uint16_t)r;
    }
}
static void fixed_codes(pk_t *k)
{
    for (int s = 0; s < NLIT; s++) k->ll[s] = s < 144 ? 8 : s < 256 ? 9 : s < 280 ? 7 : 8;
    for (int s = 0; s < NDIST; s++) k->dl[s] = 5;
    /* the canonical codes of those lengths */
    uint16_t cnt[16] = { 0 }, next[16] = { 0 };
    for (int s = 0; s < NLIT; s++) cnt[k->ll[s]]++;
    for (int b = 1, c = 0; b < 16; b++) { c = (c + cnt[b - 1]) << 1; next[b] = (uint16_t)c; }
    for (int s = 0; s < NLIT; s++) { uint32_t c = next[k->ll[s]]++, r = 0; for (int b = 0; b < k->ll[s]; b++) { r = r << 1 | (c & 1u); c >>= 1; } k->lc[s] = (uint16_t)r; }
    for (int s = 0; s < NDIST; s++) { uint32_t c = (uint32_t)s, r = 0; for (int b = 0; b < 5; b++) { r = r << 1 | (c & 1u); c >>= 1; } k->dc[s] = (uint16_t)r; }
}
/* the dynamic block's code length sequence, run-length coded (16 / 17 / 18), its code, and the header's bits */
static uint32_t dyn_header(pk_t *k)
{
    k->hlit = 286; while (k->hlit > 257 && !k->ll[k->hlit - 1]) k->hlit--;
    k->hdist = 30; while (k->hdist > 1 && !k->dl[k->hdist - 1]) k->hdist--;
    uint8_t *const seq = k->seq;
    const int ns = k->hlit + k->hdist;
    memcpy(seq, k->ll, (size_t)k->hlit);
    memcpy(seq + k->hlit, k->dl, (size_t)k->hdist);
    k->nrle = 0;
    memset(k->clf, 0, sizeof k->clf);
    for (int i = 0; i < ns;) {
        const uint8_t v = seq[i];
        int r = 1;
        while (i + r < ns && seq[i + r] == v) r++;
        i += r;
        if (v == 0) {
            while (r >= 11) { const int t = r > 138 ? 138 : r; k->rle[k->nrle] = 18; k->rlx[k->nrle++] = (uint8_t)(t - 11); k->clf[18]++; r -= t; }
            if (r >= 3) { k->rle[k->nrle] = 17; k->rlx[k->nrle++] = (uint8_t)(r - 3); k->clf[17]++; r = 0; }
        } else {
            k->rle[k->nrle] = v; k->rlx[k->nrle++] = 0; k->clf[v]++; r--;
            while (r >= 3) { const int t = r > 6 ? 6 : r; k->rle[k->nrle] = 16; k->rlx[k->nrle++] = (uint8_t)(t - 3); k->clf[16]++; r -= t; }
        }
        while (r-- > 0) { k->rle[k->nrle] = v; k->rlx[k->nrle++] = 0; k->clf[v]++; }
    }
    build(k, k->clf, NCL, 7, k->cll, k->clc);
    k->hclen = NCL; while (k->hclen > 4 && !k->cll[kClOrder[k->hclen - 1]]) k->hclen--;
    uint32_t bits = 5 + 5 + 4 + 3u * (uint32_t)k->hclen;
    for (int i = 0; i < k->nrle; i++) bits += k->cll[k->rle[i]] + (k->rle[i] == 16 ? 2u : k->rle[i] == 17 ? 3u : k->rle[i] == 18 ? 7u : 0u);
    return bits;
}
static void put_dyn_header(pk_t *k)
{
    putbits(k, (uint32_t)(k->hlit - 257), 5);
    putbits(k, (uint32_t)(k->hdist - 1), 5);
    putbits(k, (uint32_t)(k->hclen - 4), 4);
    for (int i = 0; i < k->hclen; i++) putbits(k, k->cll[kClOrder[i]], 3);
    for (int i = 0; i < k->nrle; i++) {
        const uint8_t s = k->rle[i];
        putbits(k, k->clc[s], k->cll[s]);
        if (s == 16) putbits(k, k->rlx[i], 2); else if (s == 17) putbits(k, k->rlx[i], 3); else if (s == 18) putbits(k, k->rlx[i], 7);
    }
}
static uint32_t body_bits(const pk_t *k)
{
    uint32_t b = k->extra;
    for (int s = 0; s < NLIT; s++) b += (uint32_t)k->lf[s] * k->ll[s];
    for (int s = 0; s < NDIST; s++) b += (uint32_t)k->df[s] * k->dl[s];
    return b;
}

static void le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* one block (k->b, k->n) out at k->at: its stored length, and whether it is RAW */
static uint32_t pack_block(pk_t *k, int *raw)
{
    const int64_t t0 = now_us();
    memset(k->lf, 0, sizeof k->lf); memset(k->df, 0, sizeof k->df);
    k->extra = 0; k->pass = 0;
    lz(k);
    const uint32_t start = k->at;
    /* the costs: dynamic, fixed, as it is */
    memcpy(k->df0, k->df, sizeof k->df0);
    int ndist = 0;
    for (int s = 0; s < NDIST; s++) ndist += k->df0[s] != 0;
    if (!ndist) { k->df[0] = 1; k->df[1] = 1; }      /* (no matches: a complete distance code anyway) */
    build(k, k->lf, NLIT, 15, k->ll, k->lc);
    build(k, k->df, NDIST, 15, k->dl, k->dc);
    memcpy(k->df, k->df0, sizeof k->df0);
    const uint32_t dyn = 3 + dyn_header(k) + body_bits(k);
    memcpy(k->ll_d, k->ll, sizeof k->ll_d); memcpy(k->dl_d, k->dl, sizeof k->dl_d); memcpy(k->lc_d, k->lc, sizeof k->lc_d); memcpy(k->dc_d, k->dc, sizeof k->dc_d);
    fixed_codes(k);
    const uint32_t fix = 3 + body_bits(k);
    const uint32_t bytes_dyn = (dyn + 7) / 8, bytes_fix = (fix + 7) / 8;
    const int64_t t1 = now_us();
    cf_pack_prof.match_us += t1 - t0;
    if (bytes_dyn >= k->n && bytes_fix >= k->n) {
        *raw = 1;
        putbytes(k, k->b, k->n);
        cf_pack_prof.raw++;
    } else {
        *raw = 0;
        const int use_fix = bytes_fix <= bytes_dyn;
        if (!use_fix) {
            memcpy(k->ll, k->ll_d, sizeof k->ll_d); memcpy(k->dl, k->dl_d, sizeof k->dl_d); memcpy(k->lc, k->lc_d, sizeof k->lc_d); memcpy(k->dc, k->dc_d, sizeof k->dc_d);
            putbits(k, 1 | 2u << 1, 3);                /* BFINAL, BTYPE 10 */
            put_dyn_header(k);
            cf_pack_prof.dyn++;
        } else {
            putbits(k, 1 | 1u << 1, 3);                /* BFINAL, BTYPE 01 */
            cf_pack_prof.fix++;
        }
        k->pass = 1;
        lz(k);
        if (k->nbits) putbits(k, 0, 8 - k->nbits);     /* (to the byte) */
    }
    flush_ob(k);
    cf_pack_prof.write_us += now_us() - t1;
    return k->at - start;
}

int cf_pack(int (*rd)(void *u, uint32_t off, uint8_t *b, uint32_t n), int (*wr)(void *u, uint32_t off, const uint8_t *b, uint32_t n), void *u,
            uint32_t rom_size, uint8_t shift, uint32_t max, uint32_t *total, void (*pct)(void *u, int p))
{
    memset(&cf_pack_prof, 0, sizeof cf_pack_prof);
    if (shift < 12 || shift > 14 || rom_size < CF_SGZ_ROM_MIN || rom_size > CF_SGZ_ROM_MAX) return CF_PACK_E_ARG;
    const uint32_t bs = 1u << shift, nb = (rom_size + bs - 1u) >> shift, data_start = CF_SGZ_HDR + 8u * nb;
    pk_t *k = calloc(1, sizeof *k);
    uint8_t *blk = malloc(bs);
    uint16_t *head = malloc(HSIZE * sizeof(uint16_t));
    pk_sha_t *sha = malloc(sizeof *sha);
    if (!k || !blk || !head || !sha) { free(k); free(blk); free(head); free(sha); return CF_PACK_E_MEM; }
    uint16_t *prev = cf_pack_chains ? malloc(bs * sizeof(uint16_t)) : NULL;    /* (none: one candidate a position) */
    cf_pack_prof.chains = prev != NULL;
    k->b = blk; k->head = head; k->prev = prev; k->wr = wr; k->u = u; k->at = data_start;
    sha_init(sha);
    uint32_t crc = 0, tcrc = 0, data = 0;
    uint8_t hdr[CF_SGZ_HDR];
    int e = 0;
    for (uint32_t i = 0; i < nb && !e; i++) {
        const uint32_t off = i << shift, n = rom_size - off < bs ? rom_size - off : bs;
        const int64_t t0 = now_us();
        if (rd(u, off, blk, n)) { e = CF_PACK_E_READ; break; }
        cf_pack_prof.read_us += now_us() - t0;
        if (i == 0) { memset(hdr, 0, sizeof hdr); if (n >= 0x150u) memcpy(hdr + 48, blk + 0x134, 28); }
        else if (off <= 0x134u && off + n >= 0x150u) memcpy(hdr + 48, blk + 0x134 - off, 28);
        crc = crc_z(crc, blk, n);
        sha_upd(sha, blk, n);
        k->n = n;
        int raw = 0;
        const uint32_t L = pack_block(k, &raw);
        if (k->werr) { e = CF_PACK_E_WRITE; break; }
        data += L;
        uint8_t ent[8];
        le32(ent, L | (raw ? 0x80000000u : 0u));
        le32(ent + 4, crc_z(0, blk, n));
        if (wr(u, CF_SGZ_HDR + 8u * i, ent, 8)) { e = CF_PACK_E_WRITE; break; }
        tcrc = crc_z(tcrc, ent, 8);
        if (data_start + data >= rom_size) e = CF_PACK_E_NOGAIN;
        else if (max && data_start + data > max) e = CF_PACK_E_BIG;
        if (pct) pct(u, (int)((i + 1u) * 100u / nb));
    }
    uint8_t d[32];
    sha_fin(sha, d);
    if (!e) {
        memcpy(hdr, "SGZ1", 4);
        hdr[4] = 1; hdr[5] = shift; hdr[6] = (uint8_t)nb; hdr[7] = (uint8_t)(nb >> 8);
        le32(hdr + 8, rom_size); le32(hdr + 12, crc);
        memcpy(hdr + 16, d, 32);
        le32(hdr + 76, data);
        le32(hdr + 92, crc_combine_slow(crc_z(0, hdr, 92), tcrc, 8u * nb));
        if (wr(u, 0, hdr, CF_SGZ_HDR)) e = CF_PACK_E_WRITE;
    }
    if (!e && total) *total = data_start + data;
    free(prev); free(head); free(blk); free(k); free(sha);
    return e;
}

/* ---- the same container IN ORDER, for a sink that only appends (the library's stream, cf_lib tx): pass A packs into nothing
   and keeps the header and the table (8 bytes a block), begin(total) is told the length, then the header and the table go out
   and pass B packs again, its blocks appended (the matcher is deterministic: the same bytes; checked against pass A). */
typedef struct {
    int (*rd)(void *u, uint32_t off, uint8_t *b, uint32_t n);
    int (*wr_seq)(void *u, const uint8_t *b, uint32_t n);
    void *u;
    uint8_t *ht;                        /* header + table */
    uint32_t ds, next;                  /* data start; the next byte the sink takes */
    int pass, bad;
    void (*pct)(void *u, int p);
} seq_t;
static int seq_rd(void *u, uint32_t off, uint8_t *b, uint32_t n) { seq_t *s = u; return s->rd(s->u, off, b, n); }
static int seq_wr(void *u, uint32_t off, const uint8_t *b, uint32_t n)
{
    seq_t *s = u;
    if (off < s->ds) {
        if (off + n > s->ds) return -1;
        if (s->pass == 0) memcpy(s->ht + off, b, n);
        else if (memcmp(s->ht + off, b, n)) s->bad = 1;      /* (pass B made other bytes: never) */
        return 0;
    }
    if (s->pass == 0) return 0;
    if (off != s->next) return -1;                           /* (blocks come in order, back to back) */
    s->next += n;
    return s->wr_seq(s->u, b, n);
}
static void seq_pct(void *u, int p) { seq_t *s = u; if (s->pct) s->pct(s->u, s->pass == 0 ? p / 3 : 33 + p * 2 / 3); }
int cf_pack_seq(int (*rd)(void *u, uint32_t off, uint8_t *b, uint32_t n), int (*begin)(void *u, uint32_t total),
                int (*wr_seq)(void *u, const uint8_t *b, uint32_t n), void *u, uint32_t rom_size, uint8_t shift, uint32_t max,
                uint32_t *total, void (*pct)(void *u, int p))
{
    if (shift < 12 || shift > 14 || rom_size < CF_SGZ_ROM_MIN || rom_size > CF_SGZ_ROM_MAX) return CF_PACK_E_ARG;
    const uint32_t nb = (rom_size + (1u << shift) - 1u) >> shift;
    seq_t s = { rd, wr_seq, u, NULL, CF_SGZ_HDR + 8u * nb, CF_SGZ_HDR + 8u * nb, 0, 0, pct };
    if (!(s.ht = malloc(s.ds))) return CF_PACK_E_MEM;
    uint32_t t = 0, t2 = 0;
    int e = cf_pack(seq_rd, seq_wr, &s, rom_size, shift, max, &t, seq_pct);
    if (!e && begin && begin(u, t)) e = CF_PACK_E_WRITE;
    if (!e && wr_seq(u, s.ht, s.ds)) e = CF_PACK_E_WRITE;
    if (!e) {
        s.pass = 1;
        e = cf_pack(seq_rd, seq_wr, &s, rom_size, shift, max, &t2, seq_pct);
        if (e == CF_PACK_E_MEM || e == CF_PACK_E_NOGAIN || e == CF_PACK_E_BIG) e = CF_PACK_E_WRITE;   /* (bytes went out already: never "try again" from here) */
        if (!e && (s.bad || t2 != t || s.next != t)) e = CF_PACK_E_WRITE;
    }
    free(s.ht);
    if (!e && total) *total = t;
    return e;
}
