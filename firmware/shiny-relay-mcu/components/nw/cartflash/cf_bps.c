/* cf_bps.c: see cf_bps.h (Shiny Relay 1.0.5). */
#include <string.h>

#include "cf_bps.h"
#include "cf_sha256.h"

uint32_t cf_crc32(uint32_t crc, const uint8_t *b, uint32_t n);

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* the patch, read in order through a 256-byte window */
typedef struct {
    const cf_bps_io_t *io;
    uint8_t *win;
    uint32_t base, have, pos, end;   /* window = patch[base, base+have); pos = next byte; end = the actions' end (footer) */
    int err;
} pin_t;
static int pget(pin_t *p)
{
    if (p->pos >= p->end) { p->err = CF_BPS_E_PATCH; return -1; }
    if (p->pos < p->base || p->pos >= p->base + p->have) {
        p->base = p->pos;
        p->have = p->end - p->pos < 256u ? p->end - p->pos : 256u;
        if (p->io->pat_read(p->io->u, p->base, p->win, p->have)) { p->err = CF_BPS_E_IO; p->have = 0; return -1; }
    }
    return p->win[p->pos++ - p->base];
}
static int pnum(pin_t *p, uint64_t *v)
{
    uint64_t data = 0, shift = 1;
    for (int k = 0; k < 10; k++) {
        const int x = pget(p);
        if (x < 0) return -1;
        data += (uint64_t)(x & 0x7f) * shift;
        if (x & 0x80) { *v = data; return 0; }
        shift <<= 7;
        data += shift;
        if (data > 0x10000000u) break;
    }
    p->err = CF_BPS_E_PATCH;
    return -1;
}

static int foot(const cf_bps_io_t *io, uint32_t len, uint8_t *work, uint32_t f[3])
{
    if (len < 4u + 3u + 12u) return CF_BPS_E_PATCH;
    uint8_t *b = work;
    if (io->pat_read(io->u, 0, b, 4)) return CF_BPS_E_IO;
    if (memcmp(b, "BPS1", 4)) return CF_BPS_E_PATCH;
    if (io->pat_read(io->u, len - 12u, b, 12)) return CF_BPS_E_IO;
    f[0] = le32(b); f[1] = le32(b + 4); f[2] = le32(b + 8);
    uint32_t crc = 0;
    for (uint32_t o = 0; o < len - 4u; ) {
        const uint32_t k = len - 4u - o < 256u ? len - 4u - o : 256u;
        if (io->pat_read(io->u, o, b, k)) return CF_BPS_E_IO;
        crc = cf_crc32(crc, b, k);
        o += k;
    }
    return crc == f[2] ? CF_BPS_OK : CF_BPS_E_PATCH;
}

int cf_bps_info(const cf_bps_io_t *io, uint32_t len, uint8_t *work, cf_bps_info_t *inf)
{
    uint32_t f[3];
    const int r = foot(io, len, work, f);
    if (r) return r;
    pin_t p = { io, work + CF_BPS_PAGE, 0, 0, 4, len - 12u, 0 };
    uint64_t s, t;
    if (pnum(&p, &s) || pnum(&p, &t)) return p.err ? p.err : CF_BPS_E_PATCH;
    inf->src_size = (uint32_t)s; inf->tgt_size = (uint32_t)t; inf->src_crc = f[0]; inf->tgt_crc = f[1];
    return CF_BPS_OK;
}

/* the target: one page in RAM, flushed in order; a small cache for TargetCopy from flushed pages */
typedef struct {
    const cf_bps_io_t *io;
    uint8_t *page, *rc;
    uint32_t page_base, o, rc_base, rc_have, tgt_size;
    uint32_t crc;
    cf_sha256_t sha;
} tout_t;
static int tflush(tout_t *t)
{
    const uint32_t n = t->o - t->page_base;
    if (!n) return 0;
    t->crc = cf_crc32(t->crc, t->page, n);
    cf_sha256_update(&t->sha, t->page, n);
    if (t->io->tgt_write(t->io->u, t->page_base, t->page, n)) return -1;
    t->page_base = t->o;
    return 0;
}
static int tput(tout_t *t, uint8_t v)
{
    t->page[t->o - t->page_base] = v;
    t->o++;
    if (t->o - t->page_base == CF_BPS_PAGE) return tflush(t);
    return 0;
}
static int tget(tout_t *t, uint32_t a, uint8_t *v)
{
    if (a >= t->page_base) { *v = t->page[a - t->page_base]; return 0; }
    if (a < t->rc_base || a >= t->rc_base + t->rc_have) {
        t->rc_base = a;
        t->rc_have = t->page_base - a < 256u ? t->page_base - a : 256u;
        if (t->io->tgt_read(t->io->u, a, t->rc, t->rc_have)) { t->rc_have = 0; return -1; }
    }
    *v = t->rc[a - t->rc_base];
    return 0;
}
/* n source bytes from offset s into the target */
static int tsrc(tout_t *t, uint8_t *tmp, uint32_t s, uint32_t n)
{
    while (n) {
        const uint32_t k = n < 256u ? n : 256u;
        if (t->io->src_read(t->io->u, s, tmp, k)) return -1;
        for (uint32_t i = 0; i < k; i++) if (tput(t, tmp[i])) return -1;
        s += k; n -= k;
    }
    return 0;
}

int cf_bps_apply(const cf_bps_io_t *io, uint32_t len, uint32_t src_len, uint32_t max_tgt, uint8_t *work,
                 uint32_t *tgt_len, uint32_t *tgt_crc, uint8_t tgt_sha[32], const char **why)
{
    static const char *const W[] = { "ok", "the patch is damaged", "the patch is for another ROM", "the patched ROM is not the expected one", "read or write failed" };
    uint32_t f[3];
    int r = foot(io, len, work, f);
    if (r) { *why = r == CF_BPS_E_PATCH ? "the patch is damaged (its crc32)" : W[r]; return r; }
    pin_t p = { io, work + CF_BPS_PAGE, 0, 0, 4, len - 12u, 0 };
    uint64_t ssz, tsz, msz;
    if (pnum(&p, &ssz) || pnum(&p, &tsz) || pnum(&p, &msz)) { *why = W[p.err ? p.err : 1]; return p.err ? p.err : CF_BPS_E_PATCH; }
    if (ssz != src_len) { *why = "the patch is for a ROM of another size"; return CF_BPS_E_SOURCE; }
    if (tsz > max_tgt || tsz == 0) { *why = "the patch makes a ROM that does not fit"; return CF_BPS_E_PATCH; }
    if (msz > (uint64_t)(p.end - p.pos)) { *why = W[1]; return CF_BPS_E_PATCH; }
    p.pos += (uint32_t)msz;
    uint8_t *tmp = work + CF_BPS_PAGE + 256u;
    {   /* the source's crc32 (the footer's first word) */
        uint32_t crc = 0;
        for (uint32_t o = 0; o < src_len; ) {
            const uint32_t k = src_len - o < 256u ? src_len - o : 256u;
            if (io->src_read(io->u, o, tmp, k)) { *why = W[4]; return CF_BPS_E_IO; }
            crc = cf_crc32(crc, tmp, k);
            o += k;
        }
        if (crc != f[0]) { *why = "the patch is for another ROM (crc32)"; return CF_BPS_E_SOURCE; }
    }
    tout_t t;
    memset(&t, 0, sizeof t);
    t.io = io; t.page = work; t.rc = work + CF_BPS_PAGE + 512u; t.tgt_size = (uint32_t)tsz;
    cf_sha256_init(&t.sha);
    int64_t src_rel = 0, out_rel = 0;
    while (p.pos < p.end) {
        uint64_t d;
        if (pnum(&p, &d)) { *why = W[p.err ? p.err : 1]; return p.err ? p.err : CF_BPS_E_PATCH; }
        const unsigned act = (unsigned)(d & 3u);
        const uint64_t n = (d >> 2) + 1u;
        if (t.o + n > tsz) { *why = "the patch writes past its target"; return CF_BPS_E_PATCH; }
        if (act == 0) {                                     /* SourceRead */
            if (t.o + n > src_len) { *why = "the patch reads past the source"; return CF_BPS_E_PATCH; }
            if (tsrc(&t, tmp, t.o, (uint32_t)n)) { *why = W[4]; return CF_BPS_E_IO; }
        } else if (act == 1) {                              /* TargetRead */
            for (uint64_t k = 0; k < n; k++) {
                const int x = pget(&p);
                if (x < 0) { *why = W[p.err]; return p.err; }
                if (tput(&t, (uint8_t)x)) { *why = W[4]; return CF_BPS_E_IO; }
            }
        } else {
            uint64_t e;
            if (pnum(&p, &e)) { *why = W[p.err ? p.err : 1]; return p.err ? p.err : CF_BPS_E_PATCH; }
            const int64_t rel = (e & 1u) ? -(int64_t)(e >> 1) : (int64_t)(e >> 1);
            if (act == 2) {                                 /* SourceCopy */
                src_rel += rel;
                if (src_rel < 0 || (uint64_t)src_rel + n > src_len) { *why = "the patch copies from outside the source"; return CF_BPS_E_PATCH; }
                if (tsrc(&t, tmp, (uint32_t)src_rel, (uint32_t)n)) { *why = W[4]; return CF_BPS_E_IO; }
                src_rel += (int64_t)n;
            } else {                                        /* TargetCopy: byte by byte (it may overlap) */
                out_rel += rel;
                if (out_rel < 0 || (uint64_t)out_rel >= t.o) { *why = "the patch copies from outside the target"; return CF_BPS_E_PATCH; }
                for (uint64_t k = 0; k < n; k++) {
                    uint8_t v;
                    if (tget(&t, (uint32_t)out_rel++, &v) || tput(&t, v)) { *why = W[4]; return CF_BPS_E_IO; }
                }
            }
        }
    }
    if (t.o != tsz) { *why = "the patch does not fill its target"; return CF_BPS_E_PATCH; }
    if (tflush(&t)) { *why = W[4]; return CF_BPS_E_IO; }
    cf_sha256_final(&t.sha, tgt_sha);
    if (t.crc != f[1]) { *why = "the patched ROM does not match the patch's crc32"; return CF_BPS_E_RESULT; }
    *tgt_len = t.o; *tgt_crc = t.crc;
    *why = W[0];
    return CF_BPS_OK;
}

int cf_pb_count(const uint8_t h[CF_PB_HDR], uint32_t len)
{
    if (memcmp(h, "SRPB", 4) || h[4] != 1 || h[5] == 0 || h[5] > CF_PB_MAX || h[6] || h[7]) return -1;
    if (len < CF_PB_HDR + (uint32_t)h[5] * CF_PB_ENTRY) return -1;
    return h[5];
}
int cf_pb_entry(const uint8_t e[CF_PB_ENTRY], uint32_t len, cf_pb_entry_t *o)
{
    memcpy(o->src_sha, e, 32); memcpy(o->tgt_sha, e + 32, 32);
    o->src_size = le32(e + 64); o->tgt_size = le32(e + 68); o->off = le32(e + 72); o->len = le32(e + 76);
    if (!o->src_size || !o->tgt_size || o->len < 19u || o->off < CF_PB_HDR || o->off > len || o->len > len - o->off) return -1;
    return 0;
}
