/* cf_lib.c: the game library on the FPGA's boot flash (lane gamecomp1; the layout and the rules: cf_lib.h). */
#include <stddef.h>
#include <string.h>

#include "cf_lib.h"

uint32_t cf_crc32(uint32_t crc, const uint8_t *b, uint32_t n);   /* cf_esp.c (zlib's) */

#define IX_MAGIC   0x324C4753u            /* "SGL2" (4 KiB sectors, extents) */
#define IX_VERSION 2u
#define IX_SECTS   8u
#define J_MAGIC    0x4A53u                /* "SJ" */
#define J_REC      32u
#define J_PER      (CF_LIB_SECT / J_REC)  /* 128 */
#define J_SECTS    8u
#define J_RESERVE  (CF_LIB_MAX_GAMES + 1u) /* records kept free in a sector for the carries before the next is erased */
#define TX_MAGIC   0x58544C53u            /* "SLTX" */

typedef struct {                          /* a journal record, 32 bytes */
    uint16_t magic, slot;
    uint32_t uid, seq, size, crc;
    uint8_t  rtc[5], rsv[3];
    uint32_t rcrc;                        /* of the 28 bytes before it */
} jrec_t;
_Static_assert(sizeof(jrec_t) == J_REC, "journal record");
_Static_assert(sizeof(cf_lib_index_t) <= CF_LIB_BUF, "index in the scratch");

static uint32_t sec_addr(const cf_lib_flash_t *f, uint32_t s) { return f->base + s * CF_LIB_SECT; }
static uint32_t ix_addr(const cf_lib_t *L, uint32_t s) { return sec_addr(L->f, s); }
static uint32_t j_addr(const cf_lib_t *L, uint32_t s) { return sec_addr(L->f, IX_SECTS + s); }
static uint32_t ix_crc(const cf_lib_index_t *x) { return cf_crc32(0, (const uint8_t *)x, (uint32_t)offsetof(cf_lib_index_t, crc)); }
static uint32_t rec_crc(const jrec_t *r) { return cf_crc32(0, (const uint8_t *)r, (uint32_t)offsetof(jrec_t, rcrc)); }
static uint32_t sects(uint32_t bytes) { return (bytes + CF_LIB_SECT - 1u) / CF_LIB_SECT; }

static int rd(cf_lib_t *L, uint32_t a, void *b, uint32_t n) { return L->f->read(L->f->u, a, b, n) ? CF_LIB_E_FLASH : 0; }
static int er(cf_lib_t *L, uint32_t a, uint32_t n) { return L->f->erase(L->f->u, a, n) ? CF_LIB_E_FLASH : 0; }
static int pg(cf_lib_t *L, uint32_t a, const void *b, uint32_t n) { return L->f->prog(L->f->u, a, b, n) ? CF_LIB_E_FLASH : 0; }
static int crc_of(const cf_lib_flash_t *f, uint8_t *buf, uint32_t a, uint32_t n, uint32_t *c)
{
    if (f->crc) return f->crc(f->u, a, n, c) ? CF_LIB_E_FLASH : 0;
    uint32_t x = 0;
    for (uint32_t o = 0; o < n; o += CF_LIB_BUF) {
        const uint32_t k = n - o < CF_LIB_BUF ? n - o : CF_LIB_BUF;
        if (f->read(f->u, a + o, buf, k)) return CF_LIB_E_FLASH;
        x = cf_crc32(x, buf, k);
    }
    *c = x;
    return 0;
}

/* ---------------------------------------------------------------- room */
typedef struct { uint32_t w[CF_LIB_MAX_SECT / 32]; } smap_t;
static void mark(smap_t *m, cf_lib_ext_t x) { for (uint32_t s = x.start; s < (uint32_t)x.start + x.count && s < CF_LIB_MAX_SECT; s++) m->w[s >> 5] |= 1u << (s & 31); }
static int used(const smap_t *m, uint32_t s) { return (m->w[s >> 5] >> (s & 31)) & 1u; }
static void used_map(const cf_lib_t *L, smap_t *m)
{
    memset(m, 0, sizeof *m);
    for (uint32_t s = 0; s < CF_LIB_META; s++) m->w[s >> 5] |= 1u << (s & 31);
    for (uint32_t s = L->nsect; s < CF_LIB_MAX_SECT; s++) m->w[s >> 5] |= 1u << (s & 31);
    for (int i = 0; i < L->ix.n; i++) {
        for (int k = 0; k < L->ix.e[i].next && k < CF_LIB_EXT; k++) mark(m, L->ix.e[i].ext[k]);
        mark(m, L->ix.e[i].sext);
    }
}
static uint32_t free_sects(const cf_lib_t *L)
{
    smap_t m;
    used_map(L, &m);
    uint32_t n = 0;
    for (uint32_t s = 0; s < L->nsect; s++) n += !used(&m, s);
    return n;
}
uint32_t cf_lib_total_bytes(const cf_lib_t *L) { return L->ok && L->nsect > CF_LIB_META ? (L->nsect - CF_LIB_META) * CF_LIB_SECT : 0; }
uint32_t cf_lib_free_bytes(const cf_lib_t *L) { return L->ok ? free_sects(L) * CF_LIB_SECT : 0; }
uint32_t cf_lib_save_region(uint32_t save_size, uint8_t *kslots)
{
    if (!save_size) { if (kslots) *kslots = 0; return 0; }
    const uint32_t slot = sects(save_size) * CF_LIB_SECT;
    uint32_t k = 0x8000u / slot;
    if (k < 2) k = 2;
    if (k > 16) k = 16;
    if (kslots) *kslots = (uint8_t)k;
    return slot * k;
}
/* the save a cart header asks for (0x147 the type, 0x149 the RAM size), as the play keeps it: battery-backed RAM only */
uint32_t cf_lib_cart_save(uint8_t type, uint8_t ram)
{
    switch (type) {
    case 0x06: return 0x200u;                              /* MBC2: its own 512 x 4 bits */
    case 0x03: case 0x09: case 0x0F: case 0x10: case 0x13: case 0x1B: case 0x1E: break;
    default: return 0;
    }
    static const uint8_t banks[6] = { 0, 1, 1, 4, 16, 8 };
    if (ram == 1) return 0x800u;
    return ram < 6 ? (uint32_t)banks[ram] * 0x2000u : 0;
}
uint32_t cf_lib_room_for(uint32_t stored_size, uint32_t save_size) { return sects(stored_size) * CF_LIB_SECT + cf_lib_save_region(save_size, NULL); }
uint32_t cf_lib_need(const cf_lib_t *L, uint32_t stored_size, uint32_t save_size)
{
    const uint32_t want = cf_lib_room_for(stored_size, save_size), have = cf_lib_free_bytes(L);
    return want <= have ? 0 : want - have;
}
/* the free runs of m: the save region first (one run, the lowest that fits), then the game's sectors (one run when one
   fits, the lowest such; else the longest runs first, at most CF_LIB_EXT of them). 0 = chosen */
static int choose(smap_t *m, uint32_t nsect, uint32_t gs, uint32_t ss, cf_lib_ent_t *e)
{
    if (ss) {
        uint32_t run = 0;
        for (uint32_t s = 0; s < nsect; s++) {
            run = used(m, s) ? 0 : run + 1;
            if (run == ss) { e->sext.start = (uint16_t)(s + 1 - ss); e->sext.count = (uint16_t)ss; mark(m, e->sext); break; }
        }
        if (!e->sext.count) return CF_LIB_E_FULL;
    }
    e->next = 0;
    while (gs) {
        uint32_t best = 0, bstart = 0, fit = 0, fstart = 0, run = 0;
        for (uint32_t s = 0; s <= nsect; s++) {
            if (s < nsect && !used(m, s)) { run++; continue; }
            if (run) {
                const uint32_t st = s - run;
                if (run >= gs && !fit) { fit = run; fstart = st; }
                if (run > best) { best = run; bstart = st; }
            }
            run = 0;
        }
        if (!best || e->next == CF_LIB_EXT) return CF_LIB_E_FULL;
        cf_lib_ext_t x;
        if (fit) { x.start = (uint16_t)fstart; x.count = (uint16_t)gs; }
        else { x.start = (uint16_t)bstart; x.count = (uint16_t)best; }
        e->ext[e->next++] = x;
        mark(m, x);
        gs -= x.count;
    }
    return 0;
}

int cf_lib_find(const cf_lib_t *L, uint32_t uid)
{
    for (int i = 0; L->ok && i < L->ix.n; i++) if (L->ix.e[i].uid == uid) return i;
    return -1;
}
int cf_lib_find_rom(const cf_lib_t *L, uint32_t rom_crc32, uint32_t rom_size)
{
    for (int i = 0; L->ok && i < L->ix.n; i++) if (L->ix.e[i].rom_crc32 == rom_crc32 && L->ix.e[i].rom_size == rom_size) return i;
    return -1;
}

/* ---------------------------------------------------------------- the index ring */
static int ix_valid(const cf_lib_index_t *x)
{
    return x->magic == IX_MAGIC && x->version == IX_VERSION && x->n <= CF_LIB_MAX_GAMES && ix_crc(x) == x->crc;
}
/* an entry whose sectors are not all inside this area (an FPGA update moved the area's top end down over it, or junk):
   left out of the view (the index on the flash keeps it until the next index write) */
static int ent_inside(const cf_lib_t *L, const cf_lib_ent_t *e)
{
    if (e->next > CF_LIB_EXT) return 0;
    for (int k = 0; k < e->next; k++) if ((uint32_t)e->ext[k].start + e->ext[k].count > L->nsect || e->ext[k].start < CF_LIB_META) return 0;
    return !e->sext.count || ((uint32_t)e->sext.start + e->sext.count <= L->nsect && e->sext.start >= CF_LIB_META);
}
static int ix_write(cf_lib_t *L, cf_lib_index_t *x)
{
    x->magic = IX_MAGIC; x->version = IX_VERSION;
    x->seq = L->ix.seq + 1u;
    x->crc = ix_crc(x);
    const uint32_t s = (L->isect + 1u) % IX_SECTS;
    if (er(L, ix_addr(L, s), CF_LIB_SECT) || pg(L, ix_addr(L, s), x, sizeof *x)) return CF_LIB_E_FLASH;
    cf_lib_index_t *chk = (cf_lib_index_t *)L->buf;           /* read back: only an index that is there counts */
    if (rd(L, ix_addr(L, s), chk, sizeof *chk) || memcmp(chk, x, sizeof *x)) return CF_LIB_E_VERIFY;
    /* the journal's per-entry view follows the entries (by uid) */
    cf_lib_save_t sv[CF_LIB_MAX_GAMES];
    uint8_t ss[CF_LIB_MAX_GAMES];
    for (int i = 0; i < x->n; i++) {
        memset(&sv[i], 0, sizeof sv[i]); sv[i].uid = x->e[i].uid; ss[i] = 0xFF;
        for (int k = 0; k < L->ix.n; k++) if (L->ix.e[k].uid == x->e[i].uid) { sv[i] = L->sv[k]; ss[i] = L->svsect[k]; }
    }
    L->ix = *x;
    if (L->jok) { memcpy(L->sv, sv, sizeof sv); memcpy(L->svsect, ss, sizeof ss); }
    L->isect = (uint8_t)s;
    return 0;
}

/* ---------------------------------------------------------------- the journal */
static int blank(const uint8_t *p, uint32_t n) { for (uint32_t i = 0; i < n; i++) if (p[i] != 0xFF) return 0; return 1; }
static int j_scan(cf_lib_t *L)
{
    for (int i = 0; i < L->ix.n; i++) { memset(&L->sv[i], 0, sizeof L->sv[i]); L->sv[i].uid = L->ix.e[i].uid; L->svsect[i] = 0xFF; }
    uint32_t best = 0, bs = 0, bnext = 0;
    for (uint32_t s = 0; s < J_SECTS; s++) {
        uint32_t top = 0, usedr = 0;                       /* the sector's newest seq; records up to its last written one */
        for (uint32_t r = 0; r < J_PER; r++) {
            const uint32_t per = CF_LIB_BUF / J_REC, q = r % per;
            if (q == 0 && rd(L, j_addr(L, s) + r * J_REC, L->buf, CF_LIB_BUF)) return CF_LIB_E_FLASH;
            const jrec_t *j = (const jrec_t *)(L->buf + q * J_REC);
            if (blank(L->buf + q * J_REC, J_REC)) continue;
            usedr = r + 1u;                                /* (a torn record still takes its place) */
            if (j->magic != J_MAGIC || rec_crc(j) != j->rcrc) continue;
            if (j->seq > top) top = j->seq;
            for (int i = 0; i < L->ix.n; i++) {
                if (L->ix.e[i].uid != j->uid || j->seq <= L->sv[i].seq || j->slot >= L->ix.e[i].kslots || j->size != L->ix.e[i].save_size) continue;
                L->sv[i].seq = j->seq; L->sv[i].slot = j->slot; L->sv[i].size = j->size; L->sv[i].crc = j->crc;
                memcpy(L->sv[i].rtc, j->rtc, 5);
                L->svsect[i] = (uint8_t)s;
            }
        }
        if (top > best) { best = top; bs = s; bnext = usedr; }
    }
    L->jseq = best; L->jsect = (uint8_t)bs; L->jnext = (uint16_t)bnext;
    L->jok = 1;
    return 0;
}
static int j_append(cf_lib_t *L, jrec_t *r)
{
    if (L->jnext >= J_PER) return CF_LIB_E_FULL;
    r->magic = J_MAGIC; r->seq = ++L->jseq;
    memset(r->rsv, 0, sizeof r->rsv);
    r->rcrc = rec_crc(r);
    const uint32_t a = j_addr(L, L->jsect) + (uint32_t)L->jnext * J_REC;
    L->jnext++;                                            /* (a failed or torn record is skipped either way) */
    if (pg(L, a, r, J_REC)) return CF_LIB_E_FLASH;
    jrec_t chk;
    if (rd(L, a, &chk, J_REC) || memcmp(&chk, r, J_REC)) return CF_LIB_E_VERIFY;
    return 0;
}
/* room for one record: when the sector is down to its reserve, the newest records living only in the next sector
   (the oldest) are carried into this one, then that one is erased and becomes the one written */
static int j_room(cf_lib_t *L)
{
    if (L->jnext + J_RESERVE < J_PER) return 0;
    const uint32_t nx = (L->jsect + 1u) % J_SECTS;
    for (int i = 0; i < L->ix.n; i++) {
        if (L->svsect[i] != nx || !L->sv[i].seq) continue;
        jrec_t r = { .slot = L->sv[i].slot, .uid = L->sv[i].uid, .size = L->sv[i].size, .crc = L->sv[i].crc };
        memcpy(r.rtc, L->sv[i].rtc, 5);
        const int e = j_append(L, &r);
        if (e) return e;
        L->sv[i].seq = r.seq; L->svsect[i] = L->jsect;
    }
    if (er(L, j_addr(L, nx), CF_LIB_SECT)) return CF_LIB_E_FLASH;
    L->jsect = (uint8_t)nx; L->jnext = 0;
    return 0;
}

/* ---------------------------------------------------------------- open / format */
int cf_lib_open(cf_lib_t *L, const cf_lib_flash_t *f)
{
    memset(L, 0, sizeof *L);
    L->f = f;
    if (!f || (f->base & (CF_LIB_SECT - 1u)) || (f->size & (CF_LIB_SECT - 1u)) || f->size < (CF_LIB_META + 16u) * CF_LIB_SECT) return CF_LIB_E_NOAREA;
    L->nsect = f->size / CF_LIB_SECT;
    if (L->nsect > CF_LIB_MAX_SECT) L->nsect = CF_LIB_MAX_SECT;
    int best = -1;
    uint32_t bseq = 0;
    for (uint32_t s = 0; s < IX_SECTS; s++) {
        cf_lib_index_t *x = (cf_lib_index_t *)L->buf;
        if (rd(L, ix_addr(L, s), x, sizeof *x)) return CF_LIB_E_FLASH;
        if (ix_valid(x) && (best < 0 || x->seq > bseq)) { best = (int)s; bseq = x->seq; L->ix = *x; }
    }
    for (int i = 0; i < L->ix.n;) {
        if (ent_inside(L, &L->ix.e[i])) { i++; continue; }
        memmove(&L->ix.e[i], &L->ix.e[i + 1], (size_t)(L->ix.n - i - 1) * sizeof L->ix.e[0]);
        L->ix.n--;
    }
    if (best < 0) {                                        /* an empty library: its journal is erased before its first add */
        memset(&L->ix, 0, sizeof L->ix);
        L->ix.next_uid = 1;
        L->isect = IX_SECTS - 1u;
        L->fresh = 1;
    } else L->isect = (uint8_t)best;
    L->ok = 1;
    return 0;                                              /* (the journal is scanned at the first save asked for) */
}
int cf_lib_format(cf_lib_t *L)
{
    if (!L->f) return CF_LIB_E_NOAREA;
    const cf_lib_flash_t *f = L->f;
    if (er(L, f->base, CF_LIB_META * CF_LIB_SECT)) return CF_LIB_E_FLASH;
    return cf_lib_open(L, f);
}

/* ---------------------------------------------------------------- adding a game */
/* the free sectors as a bitmap (bit set = used), for a caller that plans a stream without the library open */
void cf_lib_free_map(const cf_lib_t *L, uint32_t map[CF_LIB_MAX_SECT / 32], uint32_t *nsect, uint32_t *seq)
{
    smap_t m;
    used_map(L, &m);
    memcpy(map, m.w, sizeof m.w);
    if (nsect) *nsect = L->nsect;
    if (seq) *seq = L->ix.seq;
}
int cf_lib_plan(const uint32_t map[CF_LIB_MAX_SECT / 32], uint32_t nsect, uint32_t seq, int games, const cf_lib_ent_t *meta, cf_lib_tx_t *tx)
{
    if (!meta || !meta->stored_size || meta->save_size > CF_LIB_SAVE_MAX || nsect > CF_LIB_MAX_SECT) return CF_LIB_E_ARG;
    if (games >= CF_LIB_MAX_GAMES) return CF_LIB_E_MANY;
    smap_t m;
    memcpy(m.w, map, sizeof m.w);
    uint32_t fr = 0;
    for (uint32_t s = 0; s < nsect; s++) fr += !used(&m, s);
    if (cf_lib_room_for(meta->stored_size, meta->save_size) > fr * CF_LIB_SECT) return CF_LIB_E_FULL;
    memset(tx, 0, sizeof *tx);
    tx->e = *meta;
    memset(tx->e.ext, 0, sizeof tx->e.ext);
    tx->e.sext.start = tx->e.sext.count = 0;
    const uint32_t sr = cf_lib_save_region(meta->save_size, &tx->e.kslots);
    const int e = choose(&m, nsect, sects(meta->stored_size), sects(sr), &tx->e);
    if (e) return e;                                       /* (free room in more than 8 pieces) */
    tx->seq0 = seq;
    tx->magic = TX_MAGIC;
    return 0;
}
int cf_lib_add_begin(cf_lib_t *L, const cf_lib_ent_t *meta, cf_lib_tx_t *tx)
{
    if (!L->ok) return CF_LIB_E_NOAREA;
    uint32_t map[CF_LIB_MAX_SECT / 32];
    cf_lib_free_map(L, map, NULL, NULL);
    return cf_lib_plan(map, L->nsect, L->ix.seq, L->ix.n, meta, tx);
}
/* the game's real save size, known only once its bytes are there (its header): the save region planned at begin shrinks
   to it, or a bigger one is found now. 0 = set */
int cf_lib_tx_set_save(cf_lib_t *L, cf_lib_tx_t *tx, uint32_t save_size)
{
    if (tx->magic != TX_MAGIC || save_size > CF_LIB_SAVE_MAX) return CF_LIB_E_ARG;
    uint8_t k = 0;
    const uint32_t need = sects(cf_lib_save_region(save_size, &k));
    tx->e.save_size = save_size; tx->e.kslots = k;
    if (need <= tx->e.sext.count) { tx->e.sext.count = (uint16_t)need; if (!need) tx->e.sext.start = 0; return 0; }
    smap_t m;
    used_map(L, &m);
    for (int i = 0; i < tx->e.next; i++) mark(&m, tx->e.ext[i]);
    cf_lib_ent_t t;
    memset(&t, 0, sizeof t);
    if (choose(&m, L->nsect, 0, need, &t)) return CF_LIB_E_FULL;
    tx->e.sext = t.sext;
    return 0;
}
/* where logical byte off of the stored bytes lives (e's extents in order): its address and the bytes to its extent's end */
static int ent_at(const cf_lib_flash_t *f, const cf_lib_ent_t *e, uint32_t off, int *k, uint32_t *a, uint32_t *room)
{
    uint32_t base = 0;
    for (int i = 0; i < e->next; i++) {
        const uint32_t len = (uint32_t)e->ext[i].count * CF_LIB_SECT;
        if (off < base + len) { *k = i; *a = sec_addr(f, e->ext[i].start) + (off - base); *room = base + len - off; return 0; }
        base += len;
    }
    return -1;
}
void cf_lib_tx_rewind(cf_lib_tx_t *tx) { tx->have = tx->crc = tx->erased = 0; memset(tx->ecrc, 0, sizeof tx->ecrc); }
int cf_lib_tx_read(const cf_lib_flash_t *f, const cf_lib_tx_t *tx, uint32_t off, uint8_t *b, uint32_t n)
{
    if ((uint64_t)off + n > tx->have) return CF_LIB_E_ARG;
    while (n) {
        int k;
        uint32_t a, room;
        if (ent_at(f, &tx->e, off, &k, &a, &room)) return CF_LIB_E_ARG;
        const uint32_t q = n < room ? n : room;
        if (f->read(f->u, a, b, q)) return CF_LIB_E_FLASH;
        b += q; off += q; n -= q;
    }
    return 0;
}
int cf_lib_tx_write(const cf_lib_flash_t *f, cf_lib_tx_t *tx, const uint8_t *b, uint32_t n)
{
    if (tx->magic != TX_MAGIC || (uint64_t)tx->have + n > tx->e.stored_size) return CF_LIB_E_ARG;
    while (n) {
        int k;
        uint32_t a, room;
        /* the sectors under these bytes erased first: 64 KiB at a time where a block of the extent lines up */
        while (tx->erased < tx->have + n && tx->erased < tx->e.stored_size) {
            uint32_t ea, eroom;
            if (ent_at(f, &tx->e, tx->erased, &k, &ea, &eroom)) return CF_LIB_E_ARG;
            const uint32_t sz = (!(ea & 0xFFFFu) && eroom >= 0x10000u) ? 0x10000u : CF_LIB_SECT;   /* (the chip's 64 KiB blocks: absolute) */
            if (f->erase(f->u, ea, sz)) return CF_LIB_E_FLASH;
            tx->erased += sz;
        }
        if (ent_at(f, &tx->e, tx->have, &k, &a, &room)) return CF_LIB_E_ARG;
        const uint32_t q = n < room ? n : room;
        if (f->prog(f->u, a, b, q)) return CF_LIB_E_FLASH;
        tx->crc = cf_crc32(tx->crc, b, q);
        tx->ecrc[k] = cf_crc32(tx->ecrc[k], b, q);
        tx->have += q; b += q; n -= q;
    }
    return 0;
}
int cf_lib_add_end(cf_lib_t *L, cf_lib_tx_t *tx, uint32_t *uid)
{
    if (!L->ok) return CF_LIB_E_NOAREA;
    if (tx->magic != TX_MAGIC || tx->have != tx->e.stored_size) return CF_LIB_E_ARG;
    if (tx->e.stored_crc && tx->crc != tx->e.stored_crc) return CF_LIB_E_VERIFY;   /* (the bytes are not the ones meant) */
    if (L->ix.n >= CF_LIB_MAX_GAMES) return CF_LIB_E_MANY;
    if (L->ix.seq != tx->seq0) {                           /* the library changed since begin: its sectors must still be free */
        smap_t m;
        used_map(L, &m);
        for (int i = 0; i <= tx->e.next; i++) {
            const cf_lib_ext_t x = i < tx->e.next ? tx->e.ext[i] : tx->e.sext;
            for (uint32_t s = x.start; s < (uint32_t)x.start + x.count; s++) if (used(&m, s)) return CF_LIB_E_STALE;
        }
    }
    uint32_t off = 0;                                      /* each extent read back by crc32 */
    for (int i = 0; i < tx->e.next; i++) {
        uint32_t len = (uint32_t)tx->e.ext[i].count * CF_LIB_SECT, c = 0;
        if (len > tx->e.stored_size - off) len = tx->e.stored_size - off;
        if (crc_of(L->f, L->buf, sec_addr(L->f, tx->e.ext[i].start), len, &c)) return CF_LIB_E_FLASH;
        if (c != tx->ecrc[i]) return CF_LIB_E_VERIFY;
        off += len;
    }
    if (L->fresh) {                                        /* the first game of a new library: a clean journal */
        if (er(L, j_addr(L, 0), J_SECTS * CF_LIB_SECT)) return CF_LIB_E_FLASH;
        L->fresh = 0; L->jsect = 0; L->jnext = 0; L->jseq = 0;
    }
    if (tx->e.sext.count && er(L, sec_addr(L->f, tx->e.sext.start), (uint32_t)tx->e.sext.count * CF_LIB_SECT)) return CF_LIB_E_FLASH;
    cf_lib_index_t *const x = &L->nx;
    *x = L->ix;
    cf_lib_ent_t *e = &x->e[x->n++];
    *e = tx->e;
    e->uid = x->next_uid ? x->next_uid : 1u;
    x->next_uid = e->uid + 1u;
    e->stored_crc = tx->crc;
    e->added = L->ix.seq + 1u;
    e->title[CF_LIB_TITLE - 1] = 0;
    const uint32_t nu = e->uid;
    const int r = ix_write(L, x);
    if (r) return r;
    tx->magic = 0;
    if (uid) *uid = nu;
    return 0;
}
int cf_lib_add(cf_lib_t *L, const cf_lib_ent_t *meta, int (*get)(void *u, uint32_t off, uint8_t *b, uint32_t n), void *gu,
               uint32_t *uid, void (*pct)(void *u, int p))
{
    cf_lib_tx_t tx;
    int e = cf_lib_add_begin(L, meta, &tx);
    for (uint32_t o = 0; !e && o < meta->stored_size; o += CF_LIB_BUF) {
        const uint32_t n = meta->stored_size - o < CF_LIB_BUF ? meta->stored_size - o : CF_LIB_BUF;
        if (get(gu, o, L->buf, n)) return CF_LIB_E_ARG;
        e = cf_lib_tx_write(L->f, &tx, L->buf, n);
        if (pct && !(o & 0xFFFFu)) pct(gu, (int)((uint64_t)o * 95u / meta->stored_size));
    }
    if (!e) e = cf_lib_add_end(L, &tx, uid);
    if (!e && pct) pct(gu, 100);
    return e;
}

int cf_lib_del(cf_lib_t *L, uint32_t uid)
{
    const int i = cf_lib_find(L, uid);
    if (i < 0) return CF_LIB_E_NONE;
    cf_lib_index_t *const x = &L->nx;
    *x = L->ix;
    memmove(&x->e[i], &x->e[i + 1], (size_t)(x->n - i - 1) * sizeof x->e[0]);
    x->n--;
    memset(&x->e[x->n], 0, sizeof x->e[0]);
    return ix_write(L, x);
}

int cf_lib_read(cf_lib_t *L, uint32_t uid, uint32_t off, uint8_t *b, uint32_t n)
{
    const int i = cf_lib_find(L, uid);
    if (i < 0) return CF_LIB_E_NONE;
    const cf_lib_ent_t *e = &L->ix.e[i];
    if ((uint64_t)off + n > e->stored_size) return CF_LIB_E_ARG;
    while (n) {
        int k;
        uint32_t a, room;
        if (ent_at(L->f, e, off, &k, &a, &room)) return CF_LIB_E_ARG;
        const uint32_t q = n < room ? n : room;
        if (rd(L, a, b, q)) return CF_LIB_E_FLASH;
        b += q; off += q; n -= q;
    }
    return 0;
}

/* ---------------------------------------------------------------- saves */
static uint32_t slot_bytes(const cf_lib_ent_t *e) { return sects(e->save_size) * CF_LIB_SECT; }
static uint32_t slot_addr(const cf_lib_t *L, const cf_lib_ent_t *e, uint32_t slot, uint32_t off) { return sec_addr(L->f, e->sext.start) + slot * slot_bytes(e) + off; }
int cf_lib_save_get(cf_lib_t *L, uint32_t uid, cf_lib_save_t *sv)
{
    const int i = cf_lib_find(L, uid);
    if (i < 0) return CF_LIB_E_NONE;
    if (!L->jok && j_scan(L)) return CF_LIB_E_FLASH;
    *sv = L->sv[i];
    return 0;
}
int cf_lib_save_read(cf_lib_t *L, uint32_t uid, const cf_lib_save_t *sv, uint32_t off, uint8_t *b, uint32_t n)
{
    const int i = cf_lib_find(L, uid);
    if (i < 0) return CF_LIB_E_NONE;
    const cf_lib_ent_t *e = &L->ix.e[i];
    if (!sv->seq || (uint64_t)off + n > e->save_size) return CF_LIB_E_ARG;
    return rd(L, slot_addr(L, e, sv->slot, off), b, n);
}
int cf_lib_save_put(cf_lib_t *L, uint32_t uid, uint32_t size, const uint8_t rtc[5],
                    int (*get)(void *u, uint32_t off, uint8_t *b, uint32_t n), void *gu)
{
    const int i = cf_lib_find(L, uid);
    if (i < 0) return CF_LIB_E_NONE;
    const cf_lib_ent_t *e = &L->ix.e[i];
    if (!e->save_size || size != e->save_size || e->kslots < 2 || !e->sext.count) return CF_LIB_E_ARG;
    if (!L->jok && j_scan(L)) return CF_LIB_E_FLASH;
    int r = j_room(L);
    if (r) return r;
    const uint16_t slot = L->sv[i].seq ? (uint16_t)((L->sv[i].slot + 1u) % e->kslots) : 0u;
    if (er(L, slot_addr(L, e, slot, 0), slot_bytes(e))) return CF_LIB_E_FLASH;
    uint32_t crc = 0;
    for (uint32_t o = 0; o < size; o += CF_LIB_BUF) {
        const uint32_t n = size - o < CF_LIB_BUF ? size - o : CF_LIB_BUF;
        if (get(gu, o, L->buf, n)) return CF_LIB_E_ARG;
        crc = cf_crc32(crc, L->buf, n);
        if (pg(L, slot_addr(L, e, slot, o), L->buf, n)) return CF_LIB_E_FLASH;
    }
    uint32_t c = 0;                                        /* read back */
    if (crc_of(L->f, L->buf, slot_addr(L, e, slot, 0), size, &c)) return CF_LIB_E_FLASH;
    if (c != crc) return CF_LIB_E_VERIFY;
    jrec_t j = { .slot = slot, .uid = uid, .size = size, .crc = crc };
    if (rtc) memcpy(j.rtc, rtc, 5);
    r = j_append(L, &j);
    if (r) return r;
    L->sv[i].seq = j.seq; L->sv[i].slot = slot; L->sv[i].size = size; L->sv[i].crc = crc;
    memcpy(L->sv[i].rtc, j.rtc, 5);
    L->svsect[i] = L->jsect;
    return 0;
}
