/* cf_lib_test.c: the game library (cf_lib.c) on a NOR flash model of the FPGA's game area (0x090000 + 0x2A0000), with
 * power cuts at every flash operation of an add, a delete and a save (the cut op half done: an erase leaves part of
 * its sector, a program part of its bytes), each followed by a "power on" (cf_lib_open from the flash as it is):
 * the library must hold the state before or the state after, every other game and save intact; the operation done
 * again must then work. Then: 5000 saves across games (the journal wraps 2.5 times; a game saved once at the start
 * keeps its save through every carry) with a cut every 37th save; the area filled by one big game; 16 games; delete
 * and reuse of the units. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cf_lib.h"

static int pass, fail;
#define CHECK(c, ...) do { if (c) { pass++; if (getenv("V")) { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } else { fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define BASE 0x090000u
#define SIZE 0x2A0000u
#define FMAX 0x300000u                  /* the model's flash (room for the bigger area cases) */
static uint8_t F[FMAX];
static long ops, cut_at = -1;           /* the op number that is cut (-1: none); every op after it fails */
static int dead;
static unsigned n_erase[FMAX / 0x1000];

static int f_erase(void *u, uint32_t a, uint32_t n)
{
    (void)u;
    if (dead || a < BASE || (a & 0xFFF) || (n & 0xFFF) || a + n > BASE + FMAX) return -1;
    if (ops++ == cut_at) { memset(F + a - BASE, 0xFF, n / 2); dead = 1; return -1; }
    memset(F + a - BASE, 0xFF, n);
    for (uint32_t s = 0; s < n; s += 0x1000) n_erase[(a - BASE + s) >> 12]++;
    return 0;
}
static int f_prog(void *u, uint32_t a, const uint8_t *b, uint32_t n)
{
    (void)u;
    if (dead || a < BASE || a + n > BASE + FMAX) return -1;
    uint32_t k = n;
    if (ops++ == cut_at) { k = n / 2; dead = 1; }
    for (uint32_t i = 0; i < k; i++) F[a - BASE + i] &= b[i];   /* NOR: 1 -> 0 only */
    return dead ? -1 : 0;
}
static int f_read(void *u, uint32_t a, uint8_t *b, uint32_t n)
{
    (void)u;
    if (dead || a < BASE || a + n > BASE + FMAX) return -1;
    memcpy(b, F + a - BASE, n);
    return 0;
}
static const cf_lib_flash_t FL = { NULL, BASE, SIZE, f_erase, f_prog, f_read, NULL };

/* a game's bytes: a pattern from its seed */
typedef struct { uint32_t seed, n; } src_t;
static uint8_t gbyte(uint32_t seed, uint32_t i) { uint32_t x = (i + 1u) * 2654435761u ^ seed * 40503u; x ^= x >> 13; return (uint8_t)(x * 0x9E37u >> 7); }
static int g_get(void *u, uint32_t off, uint8_t *b, uint32_t n) { const src_t *s = u; for (uint32_t i = 0; i < n; i++) b[i] = gbyte(s->seed, off + i); return 0; }
static int g_same(cf_lib_t *L, uint32_t uid, uint32_t seed, uint32_t n)
{
    static uint8_t b[4096];
    for (uint32_t o = 0; o < n; o += sizeof b) {
        const uint32_t k = n - o < sizeof b ? n - o : sizeof b;
        if (cf_lib_read(L, uid, o, b, k)) return 0;
        for (uint32_t i = 0; i < k; i++) if (b[i] != gbyte(seed, o + i)) return 0;
    }
    return 1;
}
/* a save: its bytes from (game, version) */
typedef struct { uint32_t g, v; } sv_src_t;
static int s_get(void *u, uint32_t off, uint8_t *b, uint32_t n) { const sv_src_t *s = u; for (uint32_t i = 0; i < n; i++) b[i] = gbyte(s->g * 7919u + s->v, off + i); return 0; }
/* the version of the game's newest save (-1 none, -2 unreadable / not any version up to max) */
static int s_version(cf_lib_t *L, uint32_t uid, uint32_t g, int maxv)
{
    cf_lib_save_t sv;
    if (cf_lib_save_get(L, uid, &sv)) return -2;
    if (!sv.seq) return -1;
    static uint8_t b[0x20000];
    if (cf_lib_save_read(L, uid, &sv, 0, b, sv.size)) return -2;
    for (int v = maxv; v >= 0; v--) {
        int ok = 1;
        for (uint32_t i = 0; i < sv.size && ok; i++) if (b[i] != gbyte(g * 7919u + (uint32_t)v, i)) ok = 0;
        if (ok) return v;
    }
    return -2;
}

static cf_lib_t L;
static uint32_t add(uint32_t seed, uint32_t n, uint32_t save, const char *t)
{
    cf_lib_ent_t m;
    memset(&m, 0, sizeof m);
    m.stored_size = n; m.rom_size = n; m.save_size = save; m.flags = CF_LIB_F_SGZ;
    snprintf(m.title, sizeof m.title, "%s", t);
    src_t s = { seed, n };
    uint32_t uid = 0;
    const int e = cf_lib_add(&L, &m, g_get, &s, &uid, NULL);
    return e ? 0 : uid;
}
static int put(uint32_t uid, uint32_t g, uint32_t v, uint32_t size) { sv_src_t s = { g, v }; const uint8_t rt[5] = { (uint8_t)v, 1, 2, 3, 0 }; return cf_lib_save_put(&L, uid, size, rt, s_get, &s); }
static void power_on(void) { dead = 0; cut_at = -1; CHECK(cf_lib_open(&L, &FL) == 0, "open after a cut"); }

int main(void)
{
    memset(F, 0x5A, sizeof F);                      /* a flash nobody formatted (the area as fpgacomp1 leaves it may hold anything) */
    CHECK(cf_lib_open(&L, &FL) == 0 && L.ix.n == 0 && L.fresh, "an unknown area opens as an empty library");
    printf("area %u bytes: %u bytes for games + saves\n", SIZE, cf_lib_total_bytes(&L));
    CHECK(cf_lib_total_bytes(&L) == 656u * 4096u, "2.625 MiB: 656 sectors of 4 KiB for games and saves (64 KiB of bookkeeping)");

    /* ---- the base state: A (300 KiB, 32 KiB save), B (1 MiB, 8 KiB save) */
    const uint32_t A = add(1, 300000, 0x8000, "GAME A"), B = add(2, 0x100000, 0x2000, "GAME B");
    CHECK(A && B && L.ix.n == 2, "two games added");
    CHECK(put(A, 1, 1, 0x8000) == 0 && put(B, 2, 1, 0x2000) == 0, "their first saves");
    power_on();
    CHECK(L.ix.n == 2 && g_same(&L, A, 1, 300000) && g_same(&L, B, 2, 0x100000), "after a power on: both games byte for byte");
    CHECK(s_version(&L, A, 1, 5) == 1 && s_version(&L, B, 2, 5) == 1, "after a power on: both saves");
    const uint32_t free0 = cf_lib_free_bytes(&L) / 4096u;
    CHECK(free0 == 656u - 74u - 16u - 256u - 8u, "sectors: A 74 + its save region 16 (2 x 32 KiB), B 256 + 8 (4 x 8 KiB) (free %u)", free0);

    static uint8_t snap[FMAX];
    memcpy(snap, F, FMAX);

    /* ---- cuts in an add (C: 200 KiB, 2 KiB save) */
    long total = 0;
    { ops = 0; cut_at = -1; dead = 0; cf_lib_open(&L, &FL); const uint32_t C = add(3, 200000, 0x800, "GAME C"); total = ops; CHECK(C != 0, "add C (no cut): %ld flash ops", total); }
    int bad = 0;
    for (long c = 0; c < total; c++) {
        memcpy(F, snap, FMAX);
        dead = 0; ops = 0; cut_at = -1; cf_lib_open(&L, &FL);
        ops = 0; cut_at = c;
        (void)add(3, 200000, 0x800, "GAME C");
        power_on();
        const int n = L.ix.n;
        int ok = (n == 2 || n == 3) && g_same(&L, A, 1, 300000) && g_same(&L, B, 2, 0x100000) && s_version(&L, A, 1, 5) == 1 && s_version(&L, B, 2, 5) == 1;
        if (n == 3) ok = ok && g_same(&L, L.ix.e[2].uid, 3, 200000);
        const uint32_t C2 = n == 2 ? add(3, 200000, 0x800, "GAME C") : L.ix.e[2].uid;
        ok = ok && C2 && L.ix.n == 3 && g_same(&L, C2, 3, 200000) && cf_lib_free_bytes(&L) / 4096u == free0 - 49u - 8u;
        if (!ok) { bad++; if (bad < 4) printf("  add: cut at op %ld of %ld: n=%d\n", c, total, n); }
    }
    CHECK(bad == 0, "add: %ld cuts, every one before or after, the others intact, done again after (%d bad)", total, bad);

    /* ---- cuts in a save (A's second) */
    memcpy(F, snap, FMAX);
    { dead = 0; ops = 0; cut_at = -1; cf_lib_open(&L, &FL); ops = 0; CHECK(put(A, 1, 2, 0x8000) == 0, "save (no cut)"); total = ops; }
    bad = 0;
    for (long c = 0; c < total; c++) {
        memcpy(F, snap, FMAX);
        dead = 0; ops = 0; cut_at = -1; cf_lib_open(&L, &FL);
        ops = 0; cut_at = c;
        (void)put(A, 1, 2, 0x8000);
        power_on();
        const int va = s_version(&L, A, 1, 5);
        int ok = (va == 1 || va == 2) && s_version(&L, B, 2, 5) == 1 && g_same(&L, A, 1, 300000) && g_same(&L, B, 2, 0x100000);
        ok = ok && put(A, 1, 3, 0x8000) == 0 && s_version(&L, A, 1, 5) == 3;
        power_on();
        ok = ok && s_version(&L, A, 1, 5) == 3;
        if (!ok) { bad++; if (bad < 4) printf("  save: cut at op %ld of %ld: version %d\n", c, total, va); }
    }
    CHECK(bad == 0, "save: %ld cuts, the save before or after, the other game's intact (%d bad)", total, bad);

    /* ---- cuts in a delete (B) */
    memcpy(F, snap, FMAX);
    { dead = 0; ops = 0; cut_at = -1; cf_lib_open(&L, &FL); ops = 0; CHECK(cf_lib_del(&L, B) == 0 && L.ix.n == 1, "delete B (no cut)"); total = ops; }
    bad = 0;
    for (long c = 0; c < total; c++) {
        memcpy(F, snap, FMAX);
        dead = 0; ops = 0; cut_at = -1; cf_lib_open(&L, &FL);
        ops = 0; cut_at = c;
        (void)cf_lib_del(&L, B);
        power_on();
        int ok = (L.ix.n == 1 || L.ix.n == 2) && g_same(&L, A, 1, 300000) && s_version(&L, A, 1, 5) == 1;
        if (L.ix.n == 2) ok = ok && g_same(&L, B, 2, 0x100000) && s_version(&L, B, 2, 5) == 1 && cf_lib_del(&L, B) == 0;
        ok = ok && L.ix.n == 1 && cf_lib_find(&L, B) < 0 && cf_lib_free_bytes(&L) / 4096u == free0 + 264u;
        if (!ok) bad++;
    }
    CHECK(bad == 0, "delete: %ld cuts, before or after (%d bad)", total, bad);

    /* ---- 5000 saves: the journal wraps; A is saved only at the start (its record is carried) */
    memcpy(F, snap, FMAX);
    power_on();
    const uint32_t C = add(3, 200000, 0x800, "GAME C");
    int vb = 1, vc = -1;
    bad = 0;
    long cuts = 0;
    for (int i = 0; i < 5000; i++) {
        const int on_b = i & 1;
        const uint32_t uid = on_b ? B : C, g = on_b ? 2 : 3, size = on_b ? 0x2000 : 0x800;
        const int v = on_b ? vb + 1 : vc + 1;
        if (i % 37 == 36) {                         /* a cut somewhere in this save */
            ops = 0; cut_at = (long)(i * 7919 % 30);
            (void)put(uid, g, (uint32_t)v, size);
            power_on();
            cuts++;
            const int got = s_version(&L, uid, g, v);
            if (got == v) { if (on_b) vb = v; else vc = v; }
            else if (got != v - 1) bad++;
        } else {
            if (put(uid, g, (uint32_t)v, size)) { bad++; continue; }
            if (on_b) vb = v; else vc = v;
        }
        if (s_version(&L, A, 1, 5) != 1) { bad++; printf("  A's save lost at save %d\n", i); break; }
    }
    power_on();
    CHECK(bad == 0 && s_version(&L, A, 1, 5) == 1 && s_version(&L, B, 2, vb) == vb && s_version(&L, C, 3, vc) == vc,
          "5000 saves, %ld cuts: every newest save there, A's from the start too (%d bad; journal seq %u)", cuts, bad, L.jseq);
    unsigned emax = 0, jmax = 0;
    for (unsigned s = 0; s < FMAX / 0x1000; s++) { if (n_erase[s] > emax) emax = n_erase[s]; if (s >= 16 && s < 32 && n_erase[s] > jmax) jmax = n_erase[s]; }
    printf("wear after it all: the most-erased sector %u times, the journal's most %u times\n", emax, jmax);

    /* ---- full: one game of 2.5 MB after the others are deleted; then a 17th game refused */
    CHECK(cf_lib_del(&L, A) == 0 && cf_lib_del(&L, B) == 0 && cf_lib_del(&L, C) == 0 && L.ix.n == 0 && cf_lib_free_bytes(&L) == 656u * 4096u, "all deleted: all 656 sectors free");
    const uint32_t big = 640u * 4096u - 100u;
    CHECK(cf_lib_need(&L, big + 0x20000u, 0x8000) > 0, "a game too big: the bytes missing are said");
    const uint32_t D = add(4, big, 0x8000, "BIG GAME");
    CHECK(D && cf_lib_free_bytes(&L) == 0 && g_same(&L, D, 4, big), "a %u-byte game + its save fill the area", big);
    CHECK(cf_lib_need(&L, 1000, 0) == 4096u && add(5, 1000, 0, "X") == 0, "full: the next game refused");
    power_on();
    CHECK(L.ix.n == 1 && g_same(&L, D, 4, big), "full library after a power on");
    CHECK(cf_lib_del(&L, D) == 0, "deleted");
    int many = 0;
    for (uint32_t i = 0; i < 17; i++) if (add(10 + i, 40000, 0x200, "SMALL")) many++;
    CHECK(many == 16 && L.ix.n == 16, "16 games at most (%d added)", many);
    power_on();
    int all = L.ix.n == 16;
    for (int i = 0; i < L.ix.n; i++) all = all && g_same(&L, L.ix.e[i].uid, 10u + (uint32_t)i, 40000);
    CHECK(all, "16 games byte for byte after a power on");
    /* uids never come back */
    const uint32_t u0 = L.ix.e[0].uid;
    CHECK(cf_lib_del(&L, u0) == 0, "delete one");
    const uint32_t un = add(99, 40000, 0x200, "NEW");
    CHECK(un && un != u0 && cf_lib_find(&L, u0) < 0, "a new game gets a new uid (%u, not %u)", un, u0);

    /* ---- streaming: a tx kept across "restarts" (the library opened again between parts), a stale tx, fragmentation */
    {
        memset(F, 0xFF, sizeof F); power_on(); cf_lib_format(&L);
        cf_lib_ent_t m; memset(&m, 0, sizeof m);
        m.stored_size = 700000; m.rom_size = 700000; m.save_size = 0x8000; snprintf(m.title, sizeof m.title, "STREAM");
        static cf_lib_tx_t tx, keep;
        CHECK(cf_lib_add_begin(&L, &m, &tx) == 0, "stream: begin");
        static uint8_t b[40000];
        int e = 0;
        for (uint32_t o = 0; !e && o < m.stored_size; o += sizeof b) {
            const uint32_t n = m.stored_size - o < sizeof b ? m.stored_size - o : (uint32_t)sizeof b;
            for (uint32_t i = 0; i < n; i++) b[i] = gbyte(77, o + i);
            e = cf_lib_tx_write(&FL, &tx, b, n);
            keep = tx; power_on(); tx = keep;              /* (the unit restarted between parts: the tx came back from NVS) */
        }
        uint32_t su = 0;
        CHECK(!e && cf_lib_add_end(&L, &tx, &su) == 0 && g_same(&L, su, 77, 700000), "stream: 18 parts with a power on between each, kept byte for byte");
        /* a tx whose room another game took meanwhile: refused at its end, nothing broken */
        cf_lib_ent_t m2 = m; m2.stored_size = 300000; m2.rom_size = 300000;
        CHECK(cf_lib_add_begin(&L, &m2, &tx) == 0, "stale: begin");
        for (uint32_t i = 0; i < sizeof b; i++) b[i] = gbyte(78, i);
        for (uint32_t o = 0; o < 300000; o += 30000) (void)cf_lib_tx_write(&FL, &tx, b, 30000);
        const uint32_t other = add(79, 300000, 0x8000, "OTHER");   /* (the same free sectors: taken first) */
        CHECK(other && cf_lib_add_end(&L, &tx, NULL) == CF_LIB_E_STALE && g_same(&L, other, 79, 300000) && g_same(&L, su, 77, 700000),
              "stale: the tx whose sectors were taken is refused; the other games intact");
        /* fragmentation: 16 small games, every other one deleted: a game that needs more than 8 pieces is refused */
        memset(F, 0xFF, sizeof F); power_on(); cf_lib_format(&L);
        uint32_t u[16];
        for (int i = 0; i < 16; i++) u[i] = add(100 + (uint32_t)i, 160000, 0, "F");
        for (int i = 0; i < 16; i += 2) cf_lib_del(&L, u[i]);
        const uint32_t fr = cf_lib_free_bytes(&L);
        CHECK(add(200, 9u * 160000u, 0, "BIGF") == 0, "fragmented: a game in more than 8 free pieces refused (free %u)", fr);
        CHECK(add(201, 8u * 159744u, 0, "8P") != 0, "fragmented: a game in exactly 8 pieces kept");
    }
    /* ---- the area's top end moves down (an FPGA update with a bigger design): the games below it stay, one over it drops
       out of the view alone; the allocator fills from the bottom, so the top stays blank as long as there is room below */
    {
        static const cf_lib_flash_t Fbig = { NULL, BASE, 0x2A0000u, f_erase, f_prog, f_read, NULL };
        static const cf_lib_flash_t Fsmall = { NULL, BASE, 0x200000u, f_erase, f_prog, f_read, NULL };
        memset(F, 0xFF, sizeof F); dead = 0; cut_at = -1;
        CHECK(cf_lib_open(&L, &Fbig) == 0, "big area");
        const uint32_t lo = add(301, 900000, 0x2000, "LOW");
        const uint32_t hi = add(302, 1100000, 0x2000, "HIGH");   /* (reaches past 0x200000 - 0x090000) */
        CHECK(lo && hi && cf_lib_open(&L, &Fsmall) == 0 && L.ix.n == 1 && g_same(&L, lo, 301, 900000) && cf_lib_find(&L, hi) < 0,
              "area shrunk: the game below stays, the one over the new top drops out alone");
        CHECK(cf_lib_open(&L, &Fbig) == 0 && L.ix.n == 2 && g_same(&L, hi, 302, 1100000), "area back: both (nothing was written meanwhile)");
    }
    /* ---- the Emerald Color case: a 4 MiB game packed to 2,769,777 bytes (zopfli, 16 KiB blocks) with its 32 KiB save in an
       area of 0x2C5000 (2,904,064 bytes) fits exactly; 4 KiB more of game does not */
    {
        static const cf_lib_flash_t F2 = { NULL, BASE, 0x2C5000u, f_erase, f_prog, f_read, NULL };
        memset(F, 0xFF, sizeof F);
        dead = 0; cut_at = -1;
        CHECK(cf_lib_open(&L, &F2) == 0, "area 0x2C5000");
        cf_lib_ent_t m; memset(&m, 0, sizeof m);
        m.stored_size = 2769777; m.rom_size = 4194304; m.save_size = 0x8000; m.flags = CF_LIB_F_SGZ; snprintf(m.title, sizeof m.title, "EMERALD COLOR");
        printf("emerald: needs %u bytes, the area has %u for games\n", cf_lib_room_for(m.stored_size, m.save_size), cf_lib_total_bytes(&L));
        src_t sr = { 4242, m.stored_size };
        uint32_t ue = 0;
        const int ee = cf_lib_add(&L, &m, g_get, &sr, &ue, NULL);
        printf("emerald: add e=%d, free after %u\n", ee, cf_lib_free_bytes(&L));
        CHECK(ee == 0 && g_same(&L, ue, 4242, m.stored_size) && cf_lib_free_bytes(&L) < 4096u,
              "a 2,769,777-byte container + a 32 KiB save in 0x2C5000 (2,904,064 bytes): kept, the area full");
        memset(F, 0xFF, sizeof F); cf_lib_open(&L, &F2);
        m.stored_size += 4096u;
        CHECK(cf_lib_add(&L, &m, g_get, &sr, &ue, NULL) == CF_LIB_E_FULL, "4 KiB more: refused (full)");
    }

    /* ---- the save a cart header asks for (as the play keeps it), and Emerald Color's real one (MBC5 + RAM + battery, 64 KiB)
       in the merged area (2,977,792 bytes for games + saves): its 4 KiB-block container does not fit, its 8 KiB one does */
    CHECK(cf_lib_cart_save(0x1B, 0x05) == 0x10000u && cf_lib_cart_save(0x1B, 0x03) == 0x8000u && cf_lib_cart_save(0x10, 0x03) == 0x8000u,
          "battery saves: MBC5 64 KiB / 32 KiB, MBC3+RTC 32 KiB");
    CHECK(cf_lib_cart_save(0x06, 0) == 0x200u && cf_lib_cart_save(0x03, 0x01) == 0x800u && cf_lib_cart_save(0x0F, 0) == 0,
          "MBC2 512 bytes, a 2 KiB RAM, RTC without RAM: no save bytes");
    CHECK(cf_lib_cart_save(0x1A, 0x05) == 0 && cf_lib_cart_save(0x00, 0) == 0 && cf_lib_cart_save(0x1B, 0x07) == 0, "no battery: nothing kept; a bad RAM code: nothing");
    CHECK(cf_lib_room_for(2883317u, 0x10000u) == 3014656u && cf_lib_room_for(2819385u, 0x10000u) == 2953216u,
          "Emerald Color + 64 KiB save: 4 KiB blocks 3,014,656 (> 2,977,792), 8 KiB blocks 2,953,216 (fits)");

    printf("cf_lib: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
