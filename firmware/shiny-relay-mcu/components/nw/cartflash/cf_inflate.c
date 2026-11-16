/* cf_inflate.c: a small raw DEFLATE (RFC 1951) decoder for SGZ1 blocks (lane gamecomp1). Why not the ESP32 ROM's tinfl:
 * its state is one 10,992-byte struct, and at a play's start (right after a Wi-Fi send) the heap's largest free block was
 * 12,288 with 17,220 free in all (unit 2, 2026-10-22): the state plus a block of output did not fit. This one keeps
 * ~1.1 KB of state (canonical Huffman tables as counts + symbols, decoded bit by bit), reads its input 1 KiB at a time
 * through the caller's reader, and writes its output into SEGMENTS of 1 << seg_shift bytes, so a 16 KiB block's window
 * can be two 8 KiB allocations. Back references reach only into this block's own output (SGZ1 blocks stand alone). */
#include <string.h>

#include "cf_inflate.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define HOT IRAM_ATTR                   /* the decode loop in IRAM: no flash cache misses (unit 2: 4 MiB inflated in 6.2 s from flash) */
#else
#define HOT
#endif

static HOT int bits_fill(cf_infl_t *s)
{
    if (s->in_pos == s->in_n) {
        if (s->src_done >= s->src_len) return -1;                 /* past the stream's bytes */
        uint32_t k = s->src_len - s->src_done;
        if (k > CF_INFL_IN) k = CF_INFL_IN;
        if (s->rd(s->u, s->src_off + s->src_done, s->in, k)) return -2;
        s->src_done += k; s->in_n = k; s->in_pos = 0;
    }
    s->bitbuf |= (uint32_t)s->in[s->in_pos++] << s->bitcnt;
    s->bitcnt += 8;
    return 0;
}
static HOT int getbits(cf_infl_t *s, int n, uint32_t *v)
{
    while (s->bitcnt < (uint32_t)n) { const int e = bits_fill(s); if (e) return e; }
    *v = n ? s->bitbuf & ((1u << n) - 1u) : 0;
    s->bitbuf >>= n; s->bitcnt -= (uint32_t)n;
    return 0;
}
/* the symbol of the next code in table (count, sym) */
static HOT int decode(cf_infl_t *s, const uint16_t *count, const uint16_t *sym, int *out)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        uint32_t b;
        const int e = getbits(s, 1, &b);
        if (e) return e;
        code |= (int)b;
        const int c = count[len];
        if (code - c < first) { *out = sym[index + (code - first)]; return 0; }
        index += c; first += c; first <<= 1; code <<= 1;
    }
    return -3;                                                    /* no such code */
}
/* canonical table from code lengths; 0 = complete (or one code), -4 = over-subscribed */
static int build(uint16_t *count, uint16_t *sym, const uint8_t *len, int n)
{
    uint16_t offs[16];
    for (int i = 0; i < 16; i++) count[i] = 0;
    for (int i = 0; i < n; i++) count[len[i]]++;
    count[0] = 0;
    int left = 1;
    for (int i = 1; i < 16; i++) { left <<= 1; left -= count[i]; if (left < 0) return -4; }
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = (uint16_t)(offs[i] + count[i]);
    for (int i = 0; i < n; i++) if (len[i]) sym[offs[len[i]]++] = (uint16_t)i;
    return 0;
}

/* the lookup table for codes of up to fb bits: every fb-bit pattern that starts with a code gives its symbol and length */
static void build_fast(uint16_t *fast, int fb, const uint8_t *len, int n)
{
    uint16_t cnt[16] = { 0 }, next[16];
    memset(fast, 0, sizeof(uint16_t) << fb);
    for (int i = 0; i < n; i++) cnt[len[i]]++;
    cnt[0] = 0;
    int code = 0;
    for (int b = 1; b < 16; b++) { code = (code + cnt[b - 1]) << 1; next[b] = (uint16_t)code; }
    for (int i = 0; i < n; i++) {
        const int l = len[i];
        if (!l) continue;
        const int c = next[l]++;
        if (l > fb) continue;
        int r = 0;
        for (int k = 0; k < l; k++) r |= ((c >> k) & 1) << (l - 1 - k);   /* (deflate sends a code's bits from its top) */
        for (int f = r; f < (1 << fb); f += 1 << l) fast[f] = (uint16_t)(l << 12 | i);
    }
}
/* a symbol through the lookup table, the bit-by-bit way for longer codes (and at the stream's very end) */
static HOT int decode_fast(cf_infl_t *s, const uint16_t *fast, int fb, const uint16_t *count, const uint16_t *sym, int *out)
{
    while (s->bitcnt <= 24u && s->in_pos < s->in_n) { s->bitbuf |= (uint32_t)s->in[s->in_pos++] << s->bitcnt; s->bitcnt += 8; }   /* (up to 3 bytes at once) */
    while (s->bitcnt < (uint32_t)fb && (s->in_pos < s->in_n || s->src_done < s->src_len)) { const int e = bits_fill(s); if (e) return e; }
    const uint16_t v = fast[s->bitbuf & ((1u << fb) - 1u)];
    if (v && (uint32_t)(v >> 12) <= s->bitcnt) {
        s->bitbuf >>= v >> 12; s->bitcnt -= v >> 12;
        *out = v & 0xFFF;
        return 0;
    }
    return decode(s, count, sym, out);
}

static HOT uint8_t *at(cf_infl_t *s, uint32_t p) { return s->seg[p >> s->seg_shift] + (p & ((1u << s->seg_shift) - 1u)); }
static HOT int put(cf_infl_t *s, uint8_t c)
{
    if (s->out_pos >= s->out_len) return -5;                      /* more than the block's bytes */
    *at(s, s->out_pos++) = c;
    return 0;
}

static const uint16_t kLenBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t  kLenExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t kDistBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                                        4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t  kDistExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static HOT int codes(cf_infl_t *s)
{
    for (;;) {
        int sym;
        int e = decode_fast(s, s->lfast, CF_INFL_LFAST, s->lcount, s->lsym, &sym);
        if (e) return e;
        if (sym < 256) {                                             /* a literal: straight into its piece */
            if (s->out_pos >= s->out_len) return -5;
            s->seg[s->out_pos >> s->seg_shift][s->out_pos & ((1u << s->seg_shift) - 1u)] = (uint8_t)sym;
            s->out_pos++;
            continue;
        }
        if (sym == 256) return 0;
        sym -= 257;
        if (sym >= 29) return -6;
        uint32_t x;
        if ((e = getbits(s, kLenExtra[sym], &x))) return e;
        const uint32_t len = kLenBase[sym] + x;
        int ds;
        if ((e = decode_fast(s, s->dfast, CF_INFL_DFAST, s->dcount, s->dsym, &ds))) return e;
        if (ds >= 30) return -6;
        if ((e = getbits(s, kDistExtra[ds], &x))) return e;
        const uint32_t dist = kDistBase[ds] + x;
        if (dist > s->out_pos) return -7;                         /* before the block's first byte */
        if (s->out_pos + len > s->out_len) return -5;
        /* the copy in runs that stay inside one piece on both sides (forward byte by byte: an overlap repeats, as it must) */
        const uint32_t mask = (1u << s->seg_shift) - 1u;
        uint32_t left = len, dpos = s->out_pos, spos = s->out_pos - dist;
        while (left) {
            uint32_t run = left;
            const uint32_t droom = mask + 1u - (dpos & mask), sroom = mask + 1u - (spos & mask);
            if (run > droom) run = droom;
            if (run > sroom) run = sroom;
            uint8_t *d = s->seg[dpos >> s->seg_shift] + (dpos & mask);
            const uint8_t *sp = s->seg[spos >> s->seg_shift] + (spos & mask);
            for (uint32_t i = 0; i < run; i++) d[i] = sp[i];
            dpos += run; spos += run; left -= run;
        }
        s->out_pos += len;
    }
}
static int fixed(cf_infl_t *s)
{
    uint8_t l[288 + 30];
    int i = 0;
    for (; i < 144; i++) l[i] = 8;
    for (; i < 256; i++) l[i] = 9;
    for (; i < 280; i++) l[i] = 7;
    for (; i < 288; i++) l[i] = 8;
    (void)build(s->lcount, s->lsym, l, 288);
    build_fast(s->lfast, CF_INFL_LFAST, l, 288);
    for (i = 0; i < 30; i++) l[i] = 5;
    (void)build(s->dcount, s->dsym, l, 30);
    build_fast(s->dfast, CF_INFL_DFAST, l, 30);
    return codes(s);
}
static int dynamic(cf_infl_t *s)
{
    static const uint8_t ord[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    uint8_t l[288 + 32];
    uint32_t nlen, ndist, ncode, v;
    int e;
    if ((e = getbits(s, 5, &nlen)) || (e = getbits(s, 5, &ndist)) || (e = getbits(s, 4, &ncode))) return e;
    nlen += 257; ndist += 1; ncode += 4;
    if (nlen > 286 || ndist > 30) return -8;
    memset(l, 0, sizeof l);
    for (uint32_t i = 0; i < ncode; i++) { if ((e = getbits(s, 3, &v))) return e; l[ord[i]] = (uint8_t)v; }
    if (build(s->lcount, s->lsym, l, 19)) return -8;
    for (uint32_t i = 0; i < nlen + ndist;) {
        int sym;
        if ((e = decode(s, s->lcount, s->lsym, &sym))) return e;
        if (sym < 16) { l[i++] = (uint8_t)sym; continue; }
        uint8_t rep = 0;
        uint32_t n;
        if (sym == 16) { if (!i) return -8; rep = l[i - 1]; if ((e = getbits(s, 2, &n))) return e; n += 3; }
        else if (sym == 17) { if ((e = getbits(s, 3, &n))) return e; n += 3; }
        else { if ((e = getbits(s, 7, &n))) return e; n += 11; }
        if (i + n > nlen + ndist) return -8;
        while (n--) l[i++] = rep;
    }
    if (!l[256]) return -8;                                       /* no end-of-block code */
    /* (an incomplete table is allowed: a code it lacks is refused when it is met; an over-subscribed one is not) */
    if (build(s->lcount, s->lsym, l, (int)nlen) || build(s->dcount, s->dsym, l + nlen, (int)ndist)) return -8;
    build_fast(s->lfast, CF_INFL_LFAST, l, (int)nlen);
    build_fast(s->dfast, CF_INFL_DFAST, l + nlen, (int)ndist);
    return codes(s);
}
static int stored(cf_infl_t *s)
{
    s->bitbuf >>= (s->bitcnt & 7u); s->bitcnt -= s->bitcnt & 7u;  /* to the next byte (whole bytes already read stay) */
    uint32_t len = 0, nlen = 0, b;
    for (int k = 0; k < 2; k++) { int e = getbits(s, 8, &b); if (e) return e; len |= b << (8 * k); }
    for (int k = 0; k < 2; k++) { int e = getbits(s, 8, &b); if (e) return e; nlen |= b << (8 * k); }
    if (len != (~nlen & 0xFFFFu)) return -9;
    while (len--) { int e = getbits(s, 8, &b); if (e) return e; if ((e = put(s, (uint8_t)b))) return e; }
    return 0;
}

int cf_inflate(cf_infl_t *s, int (*rd)(void *u, uint32_t off, uint8_t *b, uint32_t n), void *u, uint32_t src_off, uint32_t src_len,
               uint8_t *const *seg, uint8_t seg_shift, uint32_t out_len)
{
    s->rd = rd; s->u = u; s->src_off = src_off; s->src_len = src_len; s->src_done = 0;
    s->in_n = s->in_pos = 0; s->bitbuf = 0; s->bitcnt = 0;
    s->seg = seg; s->seg_shift = seg_shift; s->out_len = out_len; s->out_pos = 0;
    uint32_t last = 0, type = 0;
    do {
        int e;
        if ((e = getbits(s, 1, &last)) || (e = getbits(s, 2, &type))) return e;
        if (type == 0) e = stored(s);
        else if (type == 1) e = fixed(s);
        else if (type == 2) e = dynamic(s);
        else return -10;
        if (e) return e;
    } while (!last);
    /* every byte of the stream used (only the last byte's padding bits left), exactly the block's length out */
    if (s->in_pos != s->in_n || s->src_done != s->src_len || s->bitcnt >= 8) return -11;
    return s->out_pos == s->out_len ? 0 : -12;
}
