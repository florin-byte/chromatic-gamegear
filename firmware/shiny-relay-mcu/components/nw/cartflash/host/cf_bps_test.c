/* cf_bps_test.c: cf_bps.c (Shiny Relay 1.0.5, the PARTY BATTLE patch applied on the unit) against the REAL
 * patch bundle and the owner's stock Tetris dump (read locally, never copied anywhere), with file-backed
 * source / patch / target as the ESP32's staging area would be. Also the patch built from scratch here.
 *   cf_bps_test BUNDLE.srpb STOCK_DUMP.gbc [V11_FULL.bps]   (no files: the synthetic cases only) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../cf_bps.h"
#include "../cf_sha256.h"

uint32_t cf_crc32(uint32_t crc, const uint8_t *b, uint32_t n)
{
    crc = ~crc;
    for (uint32_t i = 0; i < n; i++) { crc ^= b[i]; for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u))); }
    return ~crc;
}
static int ok_n, bad_n;
static void check(int c, const char *w) { if (c) ok_n++; else { bad_n++; printf("  FAIL %s\n", w); } }

/* the "flash": source, patch, target buffers; the target only ever written in order */
static const uint8_t *S, *P; static uint32_t SN, PN;
static uint8_t T[0x200000]; static uint32_t TW; static int t_order_bad, n_writes;
static int src_read(void *u, uint32_t o, uint8_t *b, uint32_t n) { (void)u; if (o + n > SN) return -1; memcpy(b, S + o, n); return 0; }
static int pat_read(void *u, uint32_t o, uint8_t *b, uint32_t n) { (void)u; if (o + n > PN) return -1; memcpy(b, P + o, n); return 0; }
static int tgt_read(void *u, uint32_t o, uint8_t *b, uint32_t n) { (void)u; if (o + n > TW) return -1; memcpy(b, T + o, n); return 0; }
static int fail_at = -1;
static int tgt_write(void *u, uint32_t o, const uint8_t *b, uint32_t n) { (void)u; if (o != TW) t_order_bad = 1; if (fail_at >= 0 && n_writes == fail_at) return -1; n_writes++; memcpy(T + o, b, n); TW = o + n; return 0; }
static const cf_bps_io_t IO = { NULL, src_read, pat_read, tgt_read, tgt_write };
static uint8_t work[CF_BPS_WORK];

static int apply(const uint8_t *s, uint32_t sn, const uint8_t *p, uint32_t pn, uint8_t sha[32], uint32_t *tn, const char **why)
{
    S = s; SN = sn; P = p; PN = pn; TW = 0; t_order_bad = 0; n_writes = 0;
    uint32_t crc;
    return cf_bps_apply(&IO, pn, sn, 0x1F0000u, work, tn, &crc, sha, why);
}
static uint8_t *load(const char *f, uint32_t *n)
{
    FILE *fp = fopen(f, "rb"); if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); long l = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)l); if (fread(b, 1, (size_t)l, fp) != (size_t)l) { fclose(fp); free(b); return NULL; }
    fclose(fp); *n = (uint32_t)l; return b;
}
static void hex(const uint8_t *d, char *o) { for (int i = 0; i < 32; i++) sprintf(o + 2 * i, "%02x", d[i]); }
static void sha(const uint8_t *b, uint32_t n, uint8_t d[32]) { cf_sha256_t s; cf_sha256_init(&s); cf_sha256_update(&s, b, n); cf_sha256_final(&s, d); }

/* a BPS writer for the synthetic cases: num encoding */
static uint32_t put_num(uint8_t *o, uint64_t v) { uint32_t n = 0; for (;;) { uint8_t x = v & 0x7f; v >>= 7; if (!v) { o[n++] = x | 0x80; break; } o[n++] = x; v--; } return n; }
static void put32(uint8_t *o, uint32_t v) { o[0] = (uint8_t)v; o[1] = (uint8_t)(v >> 8); o[2] = (uint8_t)(v >> 16); o[3] = (uint8_t)(v >> 24); }

int main(int argc, char **argv)
{
    /* ---- synthetic: every action, TargetCopy overlapping and from flushed pages, across page bounds */
    printf("synthetic\n");
    {
        static uint8_t src[20000], want[30000], pat[4096];
        for (unsigned i = 0; i < sizeof src; i++) src[i] = (uint8_t)(i * 7 + (i >> 8));
        uint32_t o = 0, pn = 0;
        memcpy(pat, "BPS1", 4); pn = 4;
        pn += put_num(pat + pn, sizeof src); pn += put_num(pat + pn, sizeof want); pn += put_num(pat + pn, 0);
        /* SourceRead 5000 */
        pn += put_num(pat + pn, ((5000 - 1) << 2) | 0); memcpy(want, src, 5000); o = 5000;
        /* TargetRead 3 bytes */
        pn += put_num(pat + pn, ((3 - 1) << 2) | 1); pat[pn++] = 0xAA; pat[pn++] = 0xBB; pat[pn++] = 0xCC; want[o++] = 0xAA; want[o++] = 0xBB; want[o++] = 0xCC;
        /* SourceCopy 6000 from 12000 */
        pn += put_num(pat + pn, ((6000 - 1) << 2) | 2); pn += put_num(pat + pn, (uint64_t)12000 << 1); memcpy(want + o, src + 12000, 6000); o += 6000;
        /* TargetCopy 9000 from 100 (from flushed pages and the RAM page) */
        pn += put_num(pat + pn, ((9000 - 1) << 2) | 3); pn += put_num(pat + pn, (uint64_t)100 << 1); for (int k = 0; k < 9000; k++) { want[o] = want[100 + k]; o++; }
        /* TargetCopy overlapping: 1 byte back, 997 times (a run) ; out_rel is now 9100 -> back to o-1 */
        { const int64_t rel = (int64_t)(o - 1) - 9100; pn += put_num(pat + pn, ((997 - 1) << 2) | 3); pn += put_num(pat + pn, rel < 0 ? ((uint64_t)(-rel) << 1) | 1 : (uint64_t)rel << 1);
          for (int k = 0; k < 997; k++) { want[o] = want[o - 1]; o++; } }
        /* SourceCopy backwards: -10000 relative (src_rel 18000 -> 8000), the rest */
        { const uint32_t rest = (uint32_t)sizeof want - o; pn += put_num(pat + pn, ((uint64_t)(rest - 1) << 2) | 2); pn += put_num(pat + pn, ((uint64_t)10000 << 1) | 1);
          memcpy(want + o, src + 8000, rest); o += rest; }
        put32(pat + pn, cf_crc32(0, src, sizeof src)); pn += 4; put32(pat + pn, cf_crc32(0, want, sizeof want)); pn += 4; put32(pat + pn, cf_crc32(0, pat, pn)); pn += 4;
        uint8_t d[32], w[32]; uint32_t tn; const char *why;
        int r = apply(src, sizeof src, pat, pn, d, &tn, &why);
        sha(want, sizeof want, w);
        check(r == CF_BPS_OK && tn == sizeof want && !memcmp(T, want, sizeof want) && !memcmp(d, w, 32) && !t_order_bad, "every action: the target, its sha256, written in order");
        printf("    (%s; %d page writes)\n", why, n_writes);
        pat[20] ^= 1;
        r = apply(src, sizeof src, pat, pn, d, &tn, &why);
        check(r == CF_BPS_E_PATCH && TW == 0, "one bit flipped in the patch: refused before anything is written (its crc32)");
        pat[20] ^= 1;
        static uint8_t src2[20000]; memcpy(src2, src, sizeof src2); src2[19999] ^= 0x40;
        r = apply(src2, sizeof src2, pat, pn, d, &tn, &why);
        check(r == CF_BPS_E_SOURCE && TW == 0, "another source of the same size: refused (crc32), nothing written");
        r = apply(src, sizeof src - 1, pat, pn, d, &tn, &why);
        check(r == CF_BPS_E_SOURCE, "a source of another size: refused");
        fail_at = 2; r = apply(src, sizeof src, pat, pn, d, &tn, &why); fail_at = -1;
        check(r == CF_BPS_E_IO, "a target write that fails: an error, not a result");
        cf_bps_info_t inf;
        S = src; SN = sizeof src; P = pat; PN = pn;
        check(cf_bps_info(&IO, pn, work, &inf) == 0 && inf.src_size == sizeof src && inf.tgt_size == sizeof want, "the header read on its own");
    }
    if (argc < 3) { printf("cf_bps_test: %d ok, %d failed (no real files given)\n", ok_n, bad_n); return bad_n ? 1 : 0; }

    /* ---- the real bundle and the owner's stock dump */
    printf("the real PARTY BATTLE bundle\n");
    uint32_t bn, dn;
    uint8_t *bundle = load(argv[1], &bn), *dump = load(argv[2], &dn);
    if (!bundle || !dump) { printf("  cannot read the files\n"); return 2; }
    const int cnt = cf_pb_count(bundle, bn);
    check(cnt == 12, "the bundle: 12 patches (v12 from stock + 11 updates)");
    uint8_t d[32]; char h[65];
    sha(dump, dn, d); hex(d, h);
    check(dn == 0x200000 && !strncmp(h, "1e0e6fd1", 8), "the stock 2 MiB dump is the known one (1e0e6fd1)");
    int tail_ff = 1; for (uint32_t i = 0x40000; i < 0x1F0000; i++) if (dump[i] != 0xFF) { tail_ff = 0; break; }
    check(tail_ff, "the dump past 256 KiB is FF up to the writable end");
    sha(dump, 0x40000, d); hex(d, h);
    check(!strncmp(h, "c62248a7", 8), "its first 256 KiB = stock MR.TETRIS v1.4 (c62248a7)");
    /* which entry is for this source: the unit's own choice (by the source's sha256) */
    int pick = -1; cf_pb_entry_t e[CF_PB_MAX];
    for (int i = 0; i < cnt; i++) { check(cf_pb_entry(bundle + CF_PB_HDR + (uint32_t)i * CF_PB_ENTRY, bn, &e[i]) == 0, "an index entry inside the bundle"); if (!memcmp(e[i].src_sha, d, 32)) pick = i; }
    check(pick == 0, "the stock cart picks the full patch (entry 0)");
    uint32_t tn; const char *why; uint8_t ts[32];
    int r = apply(dump, e[0].src_size, bundle + e[0].off, e[0].len, ts, &tn, &why);
    hex(ts, h);
    check(r == CF_BPS_OK && tn == 0x40000 && !memcmp(ts, e[0].tgt_sha, 32) && !strncmp(h, "f69c8072", 8), "stock -> PARTY BATTLE v12: f69c8072 (the target sha256 the bundle pins)");
    printf("    %s: %u bytes, sha256 %.16s, %d page writes\n", why, (unsigned)tn, h, n_writes);
    static uint8_t v12[0x40000]; memcpy(v12, T, sizeof v12);
    /* v12 on the cart: no entry has it as source; it IS the target (the unit says: already v12) */
    sha(v12, sizeof v12, d);
    int any = 0; for (int i = 0; i < cnt; i++) any |= !memcmp(e[i].src_sha, d, 32);
    check(!any && !memcmp(d, e[0].tgt_sha, 32), "a v12 cart: no patch is for it, it is the target (already installed)");
    /* the full v12 patch applied to v12 (a wrong cart for that patch): refused by the source crc32 */
    r = apply(v12, sizeof v12, bundle + e[0].off, e[0].len, ts, &tn, &why);
    check(r == CF_BPS_E_SOURCE && TW == 0, "the stock patch on a v12 cart: refused, nothing written");
    /* another ROM entirely: refused */
    { static uint8_t other[0x40000]; for (unsigned i = 0; i < sizeof other; i++) other[i] = (uint8_t)(i * 13);
      r = apply(other, sizeof other, bundle + e[0].off, e[0].len, ts, &tn, &why);
      check(r == CF_BPS_E_SOURCE, "another game: refused"); }
    if (argc >= 4) {
        /* an older PARTY BATTLE: v11 from stock (its own full patch), then the bundle's v11 update */
        printf("update from v11\n");
        uint32_t pn11; uint8_t *p11 = load(argv[3], &pn11);
        r = apply(dump, 0x40000, p11, pn11, ts, &tn, &why);
        hex(ts, h);
        check(r == CF_BPS_OK && !strncmp(h, "958dc453", 8), "v11 made from stock (958dc453)");
        static uint8_t v11[0x40000]; memcpy(v11, T, sizeof v11);
        int k = -1; for (int i = 0; i < cnt; i++) if (!memcmp(e[i].src_sha, ts, 32)) k = i;
        check(k > 0, "a v11 cart picks its update entry");
        if (k > 0) {
            r = apply(v11, sizeof v11, bundle + e[k].off, e[k].len, ts, &tn, &why);
            check(r == CF_BPS_OK && !memcmp(ts, e[k].tgt_sha, 32) && !memcmp(T, v12, sizeof v12), "v11 -> v12: byte for byte the v12 the stock patch makes");
        }
    }
    printf("cf_bps_test: %d ok, %d failed\n", ok_n, bad_n);
    return bad_n ? 1 : 0;
}
