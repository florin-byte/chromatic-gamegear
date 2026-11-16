/* sd_core.c: lane sdcard1: see sd_core.h. Portable C (host tests: host/sd_core_test.c). */
#include "sd_core.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* crc32 (zlib's), a nibble at a time: a 64-byte table (const: flash, not DRAM), ~4x the bit loop */
static const uint32_t k_nib[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};
uint32_t sdc_crc32(uint32_t c, const void *bv, size_t n)
{
    const uint8_t *b = bv;
    c = ~c;
    for (size_t i = 0; i < n; i++) {
        c ^= b[i];
        c = (c >> 4) ^ k_nib[c & 15u];
        c = (c >> 4) ^ k_nib[c & 15u];
    }
    return ~c;
}

/* the same rules as the RAM cart's own (cf_rc_dev.c header_parse) */
int sdc_rom_fit(uint8_t romsz, uint32_t size, uint32_t max, uint16_t *rom_banks, const char **why)
{
    *rom_banks = 0;
    if (romsz > 7) { *why = "TOO  BIG"; return -1; }        /* 32 KiB << 7 = 4 MiB (the RAM cart's room) */
    if (size < 0x150u) { *why = "SIZE"; return -1; }
    if (size > max) { *why = "TOO  BIG"; return -1; }
    *rom_banks = (uint16_t)(2u << romsz);                    /* (a shorter file: the rest read as FF, the player fills it) */
    return 0;
}

int sdc_hdr_parse(const uint8_t *h, uint32_t size, sdc_hdr_t *o)
{
    memset(o, 0, sizeof *o);
    o->why = "";
    /* the title: 0x134.. up to 16 bytes (11 on a CGB header with a maker code, but the rest is then not ASCII text) */
    int n = 0;
    const int max = (h[0x143] & 0x80) ? 15 : 16;
    for (int i = 0; i < max; i++) {
        const uint8_t c = h[0x134 + i];
        if (c == 0) break;
        if (c < 0x20 || c > 0x7E) break;
        o->title[n++] = (char)toupper(c);
    }
    while (n && o->title[n - 1] == ' ') n--;
    o->title[n] = 0;
    o->cgb = h[0x143]; o->type = h[0x147]; o->romsz = h[0x148]; o->ramsz = h[0x149];
    switch (o->type) {
    case 0x00: case 0x08: o->mbc = 0; break;
    case 0x09: o->mbc = 0; o->battery = 1; break;
    case 0x01: case 0x02: o->mbc = 1; break;
    case 0x03: o->mbc = 1; o->battery = 1; break;
    case 0x05: o->mbc = 2; break;
    case 0x06: o->mbc = 2; o->battery = 1; break;
    case 0x0F: case 0x10: o->mbc = 3; o->battery = 1; o->rtc = 1; break;
    case 0x11: case 0x12: o->mbc = 3; break;
    case 0x13: o->mbc = 3; o->battery = 1; break;
    case 0x19: case 0x1A: case 0x1C: case 0x1D: o->mbc = 5; break;
    case 0x1B: case 0x1E: o->mbc = 5; o->battery = 1; break;
    default: o->mbc = -1; o->why = "MAPPER"; return -1;
    }
    if (sdc_rom_fit(o->romsz, size, SDC_ROM_MAX, &o->rom_banks, &o->why)) return -1;
    static const uint8_t banks[6] = { 0, 1, 1, 4, 16, 8 };
    const uint8_t rb = o->ramsz < 6 ? banks[o->ramsz] : 0;
    o->sav_size = (uint32_t)rb * 0x2000u;
    if (o->ramsz == 1) o->sav_size = 0x800;
    if (o->mbc == 2) o->sav_size = 0x200;
    if (o->mbc == 3 && o->rtc && o->sav_size == 0) o->sav_size = 0;   /* (a clock with no RAM: the footer alone) */
    return 0;
}

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

int sdc_is_game_name(const char *name)
{
    if (!name || !name[0] || name[0] == '.') return 0;
    const char *d = strrchr(name, '.');
    if (!d || d == name) return 0;
    return ieq(d, ".gb") || ieq(d, ".gbc");
}

int sdc_skip_dir(const char *name)
{
    if (!name || !name[0] || name[0] == '.' || name[0] == '$') return 1;
    static const char *const skip[] = { "System Volume Information", "SHINY", "SAVES", "LOST.DIR", "Android", "DCIM", NULL };
    for (int i = 0; skip[i]; i++) if (ieq(name, skip[i])) return 1;
    return 0;
}

int sdc_save_path(const char *rom_path, char *out, size_t n)
{
    const char *b = strrchr(rom_path, '/');
    b = b ? b + 1 : rom_path;
    const char *d = strrchr(b, '.');
    const size_t bl = d && d != b ? (size_t)(d - b) : strlen(b);
    const int w = snprintf(out, n, SDC_SAVES_DIR "/%.*s.sav", (int)bl, b);
    return (w < 0 || (size_t)w >= n || bl == 0) ? -1 : 0;
}

static size_t name_up(const char *src, size_t len, char *out, size_t n)
{
    size_t k = 0;
    for (size_t i = 0; i < len && k + 1 < n; i++) {
        const unsigned char c = (unsigned char)src[i];
        if (c >= 0x80) { while (i + 1 < len && (src[i + 1] & 0xC0) == 0x80) i++; out[k++] = '?'; continue; }   /* (one ? for one UTF-8 character) */
        char o = (c == '_') ? ' ' : (c >= 0x20 && c < 0x7F) ? (char)toupper(c) : '?';
        if (o == ' ' && (k == 0 || out[k - 1] == ' ')) continue;          /* (no leading or double spaces) */
        out[k++] = o;
    }
    while (k && out[k - 1] == ' ') k--;
    out[k] = 0;
    return k;
}
static int alnums(const char *s, size_t n) { int a = 0; for (size_t i = 0; i < n; i++) if (isalnum((unsigned char)s[i])) a++; return a; }
void sdc_display_name(const char *title, const char *path, char *out, size_t n)
{
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    const char *d = strrchr(b, '.');
    size_t len = d && d != b ? (size_t)(d - b) : strlen(b);
    /* trailing tags: "Zelda DX (USA, Europe) (Rev 2) [!]" -> "Zelda DX" */
    for (;;) {
        size_t e = len;
        while (e && b[e - 1] == ' ') e--;
        if (!e || (b[e - 1] != ')' && b[e - 1] != ']')) break;
        const char open = b[e - 1] == ')' ? '(' : '[';
        size_t s0 = e - 1;
        while (s0 && b[s0] != open) s0--;
        if (b[s0] != open || alnums(b, s0) < 3) break;
        len = s0;
    }
    if (alnums(b, len) >= 3 && name_up(b, len, out, n)) return;
    if (alnums(title, strlen(title)) >= 2 && name_up(title, strlen(title), out, n)) return;
    if (name_up(b, d && d != b ? (size_t)(d - b) : strlen(b), out, n)) return;
    snprintf(out, n, "A  GAME");
}

int sdc_clean_file(const char *in, const char *want_ext, char *out, size_t n)
{
    if (!in || n < 8) return -1;
    const char *b = strrchr(in, '/');
    b = b ? b + 1 : in;
    { const char *b2 = strrchr(b, '\\'); if (b2) b = b2 + 1; }
    char t[64];
    size_t k = 0;
    for (const char *p = b; *p && k < sizeof t - 1; p++) {
        const unsigned char c = (unsigned char)*p;
        if (isalnum(c) || strchr(" ._-()'&!,+", c)) t[k++] = (char)c;
        else if (c >= 0x80) { while ((p[1] & 0xC0) == 0x80) p++; t[k++] = '_'; }
        else t[k++] = '_';
    }
    t[k] = 0;
    char *s = t;
    while (*s == '.' || *s == ' ') s++;                 /* no hidden or blank-led names */
    k = strlen(s);
    while (k && (s[k - 1] == ' ' || s[k - 1] == '.')) s[--k] = 0;
    if (!k) return -1;
    /* the extension: want_ext ("" = keep .gb / .gbc / .sav as is) */
    const char *d = strrchr(s, '.');
    const char *ext = want_ext && want_ext[0] ? want_ext : (d ? d : "");
    size_t bl = d ? (size_t)(d - s) : k;
    if (want_ext && want_ext[0] && d && ieq(d, want_ext)) bl = (size_t)(d - s);
    if (bl == 0) return -1;
    const size_t el = strlen(ext);
    if (bl + el > 60) bl = 60 - el;
    if (bl + el + 1 > n) return -1;
    memcpy(out, s, bl);
    memcpy(out + bl, ext, el + 1);
    for (char *q = out + bl; *q; q++) *q = (char)tolower((unsigned char)*q);
    return 0;
}

void sdc_rec_make(sdc_rec_t *r, const char *path, uint32_t size, uint32_t stamp, const sdc_hdr_t *h, int hdr_ok)
{
    memset(r, 0, sizeof *r);
    sdc_display_name(hdr_ok >= 0 ? h->title : "", path, r->title, sizeof r->title);
    r->cgb = h->cgb; r->type = h->type; r->romsz = h->romsz; r->ramsz = h->ramsz;
    r->flags = (hdr_ok == 0 ? SDC_F_PLAYABLE : 0) | (h->battery ? SDC_F_BATTERY : 0) | (h->rtc ? SDC_F_RTC : 0);
    r->size = size; r->stamp = stamp;
    snprintf(r->path, sizeof r->path, "%s", path);
}

void sdc_key_folder(char t[17], uint32_t id, const char *name)
{
    /* '0' + 6 bits: '0'..'o', in ASCII order, so strncmp orders the ids as numbers (up to 262,143) */
    t[0] = (char)('0' + ((id >> 12) & 63)); t[1] = (char)('0' + ((id >> 6) & 63)); t[2] = (char)('0' + (id & 63));
    size_t k = 3;
    for (const char *p = name ? name : ""; *p && k < 16; p++) t[k++] = (char)toupper((unsigned char)*p);
    while (k < 16) t[k++] = 0;
    t[16] = 0;
}

uint32_t sdc_fp_start(void) { return 2166136261u; }
static uint32_t fnv(uint32_t h, const void *bv, size_t n)
{
    const uint8_t *b = bv;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}
uint32_t sdc_fp_add(uint32_t fp, const char *path, uint32_t size, uint32_t stamp)
{
    fp = fnv(fp, path, strlen(path) + 1);
    const uint8_t v[8] = { (uint8_t)size, (uint8_t)(size >> 8), (uint8_t)(size >> 16), (uint8_t)(size >> 24),
                           (uint8_t)stamp, (uint8_t)(stamp >> 8), (uint8_t)(stamp >> 16), (uint8_t)(stamp >> 24) };
    return fnv(fp, v, sizeof v);
}

/* (title, index) order */
static int kcmp(const char *ta, uint32_t ia, const char *tb, uint32_t ib)
{
    const int c = strncmp(ta, tb, 16);
    if (c) return c;
    return ia < ib ? -1 : ia > ib ? 1 : 0;
}
/* the work array as a max-heap of the cap smallest keys seen */
static void sift_down(sdc_key_t *h, uint32_t n, uint32_t i)
{
    for (;;) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < n && kcmp(h[l].t, h[l].i, h[m].t, h[m].i) > 0) m = l;
        if (r < n && kcmp(h[r].t, h[r].i, h[m].t, h[m].i) > 0) m = r;
        if (m == i) return;
        const sdc_key_t x = h[i]; h[i] = h[m]; h[m] = x;
        i = m;
    }
}
static void sift_up(sdc_key_t *h, uint32_t i)
{
    while (i) {
        const uint32_t p = (i - 1) / 2;
        if (kcmp(h[i].t, h[i].i, h[p].t, h[p].i) <= 0) return;
        const sdc_key_t x = h[i]; h[i] = h[p]; h[p] = x;
        i = p;
    }
}
int sdc_sort(uint32_t n, int (*get)(void *u, uint32_t i, char t[17]), int (*emit)(void *u, uint32_t i), void *u,
             sdc_key_t *work, uint32_t cap, uint32_t *passes)
{
    if (cap == 0) return -1;
    char last_t[16] = { 0 };
    uint32_t last_i = 0, done = 0, np = 0;
    int have_last = 0;
    while (done < n) {
        uint32_t m = 0;
        np++;
        for (uint32_t i = 0; i < n; i++) {
            char t[17];
            if (get(u, i, t)) return -2;
            char k[16];
            memset(k, 0, sizeof k);
            memcpy(k, t, strnlen(t, 16));
            if (have_last && kcmp(k, i, last_t, last_i) <= 0) continue;
            if (m < cap) { memcpy(work[m].t, k, 16); work[m].i = i; sift_up(work, m); m++; }
            else if (kcmp(k, i, work[0].t, work[0].i) < 0) { memcpy(work[0].t, k, 16); work[0].i = i; sift_down(work, m, 0); }
        }
        if (m == 0) break;
        /* heap sort in place: ascending */
        for (uint32_t e = m; e > 1; e--) { const sdc_key_t x = work[0]; work[0] = work[e - 1]; work[e - 1] = x; sift_down(work, e - 1, 0); }
        for (uint32_t j = 0; j < m; j++) if (emit(u, work[j].i)) return -3;
        memcpy(last_t, work[m - 1].t, 16); last_i = work[m - 1].i; have_last = 1;
        done += m;
    }
    if (passes) *passes = np;
    return 0;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
void sdc_rtc_put(uint8_t f[SDC_RTC_FOOTER], const uint8_t rtc[5], uint64_t t)
{
    for (int i = 0; i < 5; i++) { put32(f + 4 * i, rtc[i]); put32(f + 20 + 4 * i, rtc[i]); }
    put32(f + 40, (uint32_t)t); put32(f + 44, (uint32_t)(t >> 32));
}
int sdc_rtc_get(const uint8_t *f, uint32_t len, uint8_t rtc[5], uint64_t *t)
{
    if (len != 48 && len != 44) return -1;
    for (int i = 0; i < 5; i++) rtc[i] = (uint8_t)get32(f + 4 * i);
    rtc[0] &= 0x3F; rtc[1] &= 0x3F; rtc[2] &= 0x1F; rtc[4] &= 0xC1;
    if (t) *t = len == 48 ? (uint64_t)get32(f + 40) | (uint64_t)get32(f + 44) << 32 : get32(f + 40);
    return 0;
}
void sdc_rtc_advance(uint8_t r[5], uint64_t s)
{
    if (r[4] & 0x40) return;                          /* halted */
    uint64_t sec = r[0] + s;
    r[0] = (uint8_t)(sec % 60);
    uint64_t min = r[1] + sec / 60;
    r[1] = (uint8_t)(min % 60);
    uint64_t hr = r[2] + min / 60;
    r[2] = (uint8_t)(hr % 24);
    uint64_t day = ((uint64_t)(r[4] & 1) << 8 | r[3]) + hr / 24;
    if (day > 511) { r[4] |= 0x80; day %= 512; }
    r[3] = (uint8_t)day;
    r[4] = (uint8_t)((r[4] & 0xFE) | (day >> 8));
}

int sdc_clk_load(sdc_clk_t *c, uint8_t rtc[5], uint64_t t_saved, uint64_t now, int64_t up)
{
    int adv = 0;
    memset(c, 0, sizeof *c);
    c->up0 = up;
    if (now && t_saved) {
        if (now > t_saved) { sdc_rtc_advance(rtc, now - t_saved); adv = 1; }
        c->t_ref = now;
    } else if (now) {
        c->t_ref = now;                                   /* nothing to catch up (no time was kept with it) */
    } else if (t_saved) {
        c->t_ref = t_saved;                               /* the clock as it was left: the gap is caught up later */
        c->pending = 1;
    }
    return adv;
}
uint64_t sdc_clk_save(const sdc_clk_t *c, uint8_t rtc[5], uint64_t now, int64_t up)
{
    const int64_t ran = up > c->up0 ? up - c->up0 : 0;      /* the seconds the game has run since the load */
    if (now) {
        if (c->pending && now > (uint64_t)ran) {
            const uint64_t at_load = now - (uint64_t)ran;    /* the real time at the load */
            if (at_load > c->t_ref) sdc_rtc_advance(rtc, at_load - c->t_ref);
        }
        return now;
    }
    if (c->t_ref) return c->t_ref + (uint64_t)ran;           /* no real time: the last one known plus the time played */
    return 0;
}
uint32_t sdc_tsec_pack(uint64_t t)
{
    if (t <= SDC_T_EPOCH) return 0;
    const uint32_t v = (uint32_t)((t - SDC_T_EPOCH) & SDC_TSEC_MASK);
    return v ? v : 1u;                                    /* (0 is "not known": the one second in 194 days that is 0 is read as 1) */
}
uint64_t sdc_tsec_unpack(uint32_t v, uint64_t anchor)
{
    if (!v || v > SDC_TSEC_MASK || anchor <= SDC_T_EPOCH) return 0;
    const uint32_t a = (uint32_t)((anchor - SDC_T_EPOCH) & SDC_TSEC_MASK);
    const uint32_t d = (a - v) & SDC_TSEC_MASK;           /* how long before the anchor, modulo 2^24 s */
    if (d > SDC_TSEC_MASK + 1u - SDC_TSEC_AHEAD) return anchor + (SDC_TSEC_MASK + 1u - d);   /* a little ahead of it: a clock that ran fast */
    return anchor > d ? anchor - d : 0;
}
