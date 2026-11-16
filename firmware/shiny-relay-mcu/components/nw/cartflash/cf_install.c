/* cf_install.c: see cf_install.h (lane cartflash1; receive mode + code: lane wifirecv1). */
#include <ctype.h>
#include <strings.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>

#include "cf_install.h"
#include "cf_proto.h"

static void logf_(cf_inst_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void logf_(cf_inst_t *c, const char *fmt, ...)
{
    if (!c->h.log) return;
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    c->h.log(c->h.user, b);
}

/* ---------------------------------------------------------------- a flat JSON object reader
   (the answer is one small object; no nesting is needed) */
static const char *jfind(const char *j, const char *key)
{
    char k[40];
    const int kn = snprintf(k, sizeof k, "\"%s\"", key);
    for (const char *p = strstr(j, k); p; p = strstr(p + 1, k)) {
        const char *q = p + kn;
        while (*q == ' ') q++;
        if (*q != ':') continue;
        q++;
        while (*q == ' ') q++;
        return q;
    }
    return NULL;
}
static int jstr(const char *j, const char *key, char *out, size_t cap)
{
    const char *q = jfind(j, key);
    if (!q || *q != '"') return -1;
    q++;
    size_t n = 0;
    while (*q && *q != '"') {
        if (*q == '\\') return -1;               /* no escapes in any field we take */
        if (n + 1 >= cap) return -1;
        out[n++] = *q++;
    }
    if (*q != '"') return -1;
    out[n] = 0;
    return (int)n;
}
static int jnum(const char *j, const char *key, uint32_t *v)
{
    const char *q = jfind(j, key);
    if (!q || !isdigit((unsigned char)*q)) return -1;
    char *e;
    const unsigned long x = strtoul(q, &e, 10);
    if (e == q || x > 0xFFFFFFFFul) return -1;
    *v = (uint32_t)x;
    return 0;
}
static int jtrue(const char *j, const char *key)
{
    const char *q = jfind(j, key);
    return q && !strncmp(q, "true", 4);
}
static int all_hex(const char *s, size_t n)
{
    if (strlen(s) != n) return 0;
    for (size_t i = 0; i < n; i++) if (!isxdigit((unsigned char)s[i]) || isupper((unsigned char)s[i])) return 0;
    return 1;
}
static int hexval(int ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1; }
/* n bytes from 2n hex digits, either case; 0 ok */
static int unhex(const char *s, uint8_t *out, size_t n)
{
    if (strlen(s) != 2 * n) return -1;
    for (size_t i = 0; i < n; i++) {
        const int a = hexval((unsigned char)s[2 * i]), b = hexval((unsigned char)s[2 * i + 1]);
        if (a < 0 || b < 0) return -1;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return 0;
}
/* The blob URL: absolute http:// (or https://, which costs a TLS session), a plain host[:port],
   and the path /gbnet/v1/install/<this id>/blob/<hex token>, nothing after it. The token in the
   path is the only credential on that connection: nothing else is ever sent there. */
static int blob_url_ok(const char *u, const char *id)
{
    const char *h = !strncmp(u, "http://", 7) ? u + 7 : !strncmp(u, "https://", 8) ? u + 8 : NULL;
    if (!h || strlen(u) > CF_BLOB_URL_MAX) return 0;
    const char *slash = strchr(h, '/');
    if (!slash || slash == h) return 0;
    for (const char *p = h; p < slash; p++) if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '-' || *p == ':')) return 0;
    char want[80];
    snprintf(want, sizeof want, "/gbnet/v1/install/%s/blob/", id);
    if (strncmp(slash, want, strlen(want))) return 0;
    const char *tok = slash + strlen(want);
    if (strlen(tok) < 16) return 0;
    for (const char *p = tok; *p; p++) if (!isxdigit((unsigned char)*p)) return 0;
    return 1;
}

int cf_ctr_xor(int (*ecb)(void *u, const uint8_t in[16], uint8_t out[16]), void *u, const uint8_t iv[16], uint32_t off, uint8_t *b, uint32_t n)
{
    uint8_t ctr[16], ks[16];
    memcpy(ctr, iv, 16);
    uint32_t add = off / 16u;
    for (int i = 15; i >= 0 && add; i--) {          /* ctr += off / 16 */
        const uint32_t v = (uint32_t)ctr[i] + (add & 0xFFu);
        ctr[i] = (uint8_t)v;
        add = (add >> 8) + (v >> 8);
    }
    uint32_t skip = off % 16u, i = 0;
    int r = 0;
    while (i < n) {
        if (ecb(u, ctr, ks)) { r = -1; break; }
        for (int k = 15; k >= 0; k--) if (++ctr[k]) break;   /* the next block (wraps mod 2^128) */
        for (; skip < 16 && i < n; skip++, i++) b[i] ^= ks[skip];
        skip = 0;
    }
    volatile uint8_t *z = ks;
    for (int k = 0; k < 16; k++) z[k] = 0;
    return r;
}

void cf_offer_wipe(cf_offer_t *o)
{
    volatile uint8_t *p = (volatile uint8_t *)o;
    for (size_t i = 0; i < sizeof *o; i++) p[i] = 0;
}

int cf_install_parse(const char *json, const char *base, cf_offer_t *o, const char **why)
{
    (void)base;
    *why = "";
    cf_offer_wipe(o);
    if (!jtrue(json, "pending")) return 0;
    /* lane mygames1: an answer that filled the buffer was cut: its last keys (serde sorts them: "z" last) are lost, and a
       packed game would be taken as a plain one. Not taken */
    if (strlen(json) >= CF_INST_BUF - 1) { *why = "answer too long"; return -1; }
    char crc[12];
    /* the id: 32 hex, or a uuid with dashes (36): kept as its 32 hex digits */
    char idraw[40];
    const int il = jstr(json, "id", idraw, sizeof idraw);
    if (il == 36) {
        int n = 0;
        for (int i = 0; i < 36; i++) if (idraw[i] != '-') { if (n >= 32) { n = 99; break; } o->id[n++] = idraw[i]; }
        o->id[n < 33 ? n : 0] = 0;
    } else if (il == 32) memcpy(o->id, idraw, 33);
    if (!all_hex(o->id, 32)) { *why = "id"; return -1; }
    /* the encrypted image (wire format section 1): all four, or the offer is not taken */
    {
        char alg[16], k[72], v[40];
        if (jstr(json, "enc_alg", alg, sizeof alg) < 1 || strcmp(alg, "AES-256-CTR")) { *why = "no AES-256-CTR image"; cf_offer_wipe(o); return -1; }
        if (jstr(json, "enc_key", k, sizeof k) != 64 || unhex(k, o->key, 32)) { memset(k, 0, sizeof k); *why = "enc_key"; cf_offer_wipe(o); return -1; }
        memset(k, 0, sizeof k);
        if (jstr(json, "enc_iv", v, sizeof v) != 32 || unhex(v, o->iv, 16)) { *why = "enc_iv"; cf_offer_wipe(o); return -1; }
        if (jstr(json, "blob_url", o->blob_url, sizeof o->blob_url) < 1 || !blob_url_ok(o->blob_url, o->id)) { *why = "blob_url"; cf_offer_wipe(o); return -1; }
    }
    /* the size rule the device can check without the image: 32 KiB .. the chip (2 MiB). Whether a
       file longer than the writable flash ends in FF is checked on the cart (CF_JOB_STREAM). */
    {   /* 1.0.5: "kind": "patch" = a patch bundle (the unit applies it to its own cart); anything else is not taken */
        char kd[12];
        const int kn = jstr(json, "kind", kd, sizeof kd);
        if (kn > 0 && !strcmp(kd, "patch")) o->kind = CF_KIND_PATCH;
        else if (kn > 0 && !strcmp(kd, "backup")) o->kind = CF_KIND_BACKUP;
        else if (kn > 0 && !strcmp(kd, "save")) o->kind = CF_KIND_SAVE;
        else if (kn > 0 && !strcmp(kd, "romsave")) o->kind = CF_KIND_ROMSAVE;
        else if (kn > 0 && !strcmp(kd, "play")) o->kind = CF_KIND_PLAY;
        else if (kn > 0 && strcmp(kd, "rom")) { *why = "kind"; cf_offer_wipe(o); return -1; }
    }
    {   /* lane sdcard1: "sd": "keep" | "play" and "file": a game (kind play) or a save (kind save) for the unit's card */
        char sv[8];
        const int sn = jstr(json, "sd", sv, sizeof sv);
        if (sn > 0) {
            if (!strcmp(sv, "keep")) o->sd = CF_SD_KEEP;
            else if (!strcmp(sv, "play") && o->kind == CF_KIND_PLAY) o->sd = CF_SD_PLAY;
            else { *why = "sd"; cf_offer_wipe(o); return -1; }
            if (o->kind != CF_KIND_PLAY && o->kind != CF_KIND_SAVE) { *why = "sd"; cf_offer_wipe(o); return -1; }
            const int fl = jstr(json, "file", o->file, sizeof o->file);
            int ok = fl > 0 && o->file[0] != '.' && o->file[0] != ' ';
            for (const char *q = o->file; ok && *q; q++) if (*q < 0x20 || *q > 0x7E || strchr("/\\:*?\"<>|", *q)) ok = 0;
            const char *dot = ok ? strrchr(o->file, '.') : NULL;
            if (ok) ok = dot && dot != o->file && (o->kind == CF_KIND_SAVE ? !strcasecmp(dot, ".sav") : (!strcasecmp(dot, ".gb") || !strcasecmp(dot, ".gbc")));
            if (!ok) { *why = "file"; cf_offer_wipe(o); return -1; }
        }
    }
    {   /* lane ui110dev: "lib": "keep" | "none" on a play send with no card */
        char lv[8];
        if (jstr(json, "lib", lv, sizeof lv) > 0) {
            if (o->kind != CF_KIND_PLAY || o->sd) { *why = "lib"; cf_offer_wipe(o); return -1; }
            if (!strcmp(lv, "keep")) o->lib = CF_LIB_KEEP;
            else if (!strcmp(lv, "none")) o->lib = CF_LIB_NONE;
            else { *why = "lib"; cf_offer_wipe(o); return -1; }
        }
    }
    if (o->kind == CF_KIND_BACKUP) {
        /* 1.0.6: nothing comes down; the blob URL takes the cart's bytes up (no size or hashes to check) */
        snprintf(o->title, sizeof o->title, "YOUR  CART");
        return 1;
    }
    if (o->kind == CF_KIND_PATCH) {
        if (jnum(json, "size", &o->size) || o->size < CF_PATCH_MIN || o->size > CF_PATCH_MAX) { *why = "size"; cf_offer_wipe(o); return -1; }
    } else if (o->kind == CF_KIND_SAVE) {
        if (jnum(json, "size", &o->size) || (o->sd ? (o->size < 1u || o->size > CF_SD_SAVE_MAX) : o->size != 0x8000u)) { *why = "size"; cf_offer_wipe(o); return -1; }
    } else if (o->kind == CF_KIND_ROMSAVE) {
        /* the game's part: at least 32 KiB, at most the cart's writable 1.94 MB, with its own hashes; then exactly 32 KiB of save */
        char rc[12];
        if (jnum(json, "size", &o->size) || jnum(json, "rom_size", &o->rom_size) || o->rom_size < 0x8000u || o->rom_size > CF_INST_WRITABLE ||
            o->size != o->rom_size + 0x8000u) { *why = "size"; cf_offer_wipe(o); return -1; }
        if (jstr(json, "rom_crc32", rc, sizeof rc) != 8 || !all_hex(rc, 8)) { *why = "rom_crc32"; cf_offer_wipe(o); return -1; }
        o->rom_crc32 = (uint32_t)strtoul(rc, NULL, 16);
        if (jstr(json, "rom_sha256", o->rom_sha256, sizeof o->rom_sha256) != 64 || !all_hex(o->rom_sha256, 64)) { *why = "rom_sha256"; cf_offer_wipe(o); return -1; }
    } else if (o->kind == CF_KIND_PLAY) {
        char zf[8];
        if (jstr(json, "z", zf, sizeof zf) > 0) {
            /* lane gamecomp1: an SGZ1 container (size = its own, up to the staging area) for a game of game_size bytes */
            char rc[12];
            if (strcmp(zf, "sgz1")) { *why = "z"; cf_offer_wipe(o); return -1; }
            o->z = 1;
            if (jnum(json, "size", &o->size) || o->size < 104u || o->size > CF_PLAY_MAX + CF_SD_SAVE_MAX || jnum(json, "game_size", &o->rom_size) ||
                o->rom_size < 0x8000u || o->rom_size > CF_PLAY_MAX) { *why = "size"; cf_offer_wipe(o); return -1; }
            if (jstr(json, "game_crc32", rc, sizeof rc) != 8 || !all_hex(rc, 8)) { *why = "game_crc32"; cf_offer_wipe(o); return -1; }
            o->rom_crc32 = (uint32_t)strtoul(rc, NULL, 16);
            if (jstr(json, "game_sha256", o->rom_sha256, sizeof o->rom_sha256) != 64 || !all_hex(o->rom_sha256, 64)) { *why = "game_sha256"; cf_offer_wipe(o); return -1; }
        } else if (jnum(json, "size", &o->size) || o->size < 0x8000u || o->size > (o->sd == CF_SD_KEEP ? CF_SD_GAME_MAX : CF_PLAY_MAX) + CF_SD_SAVE_MAX) { *why = "size"; cf_offer_wipe(o); return -1; }
        /* lane mygames1: the game's save after its object (the whole download is size); none: the object is all of it */
        if (jnum(json, "save_size", &o->save_size) == 0 && o->save_size) {
            char oc[12], sc[12];
            if (o->save_size > CF_SD_SAVE_MAX || jnum(json, "obj_size", &o->obj_size) || o->obj_size + o->save_size != o->size ||
                jstr(json, "obj_crc32", oc, sizeof oc) != 8 || !all_hex(oc, 8) || jstr(json, "save_crc32", sc, sizeof sc) != 8 || !all_hex(sc, 8)) { *why = "save_size"; cf_offer_wipe(o); return -1; }
            o->obj_crc32 = (uint32_t)strtoul(oc, NULL, 16);
            o->save_crc32 = (uint32_t)strtoul(sc, NULL, 16);
        } else o->save_size = 0;
        if (CF_OBJ_SIZE(o) > (o->z ? CF_PLAY_MAX : o->sd == CF_SD_KEEP ? CF_SD_GAME_MAX : CF_PLAY_MAX) || (!o->z && CF_OBJ_SIZE(o) < 0x8000u)) { *why = "size"; cf_offer_wipe(o); return -1; }
    } else if (jnum(json, "size", &o->size) || o->size < 0x8000u || o->size > 0x200000u) { *why = "size"; cf_offer_wipe(o); return -1; }
    if (jstr(json, "crc32", crc, sizeof crc) != 8 || !all_hex(crc, 8)) { *why = "crc32"; cf_offer_wipe(o); return -1; }
    o->crc32 = (uint32_t)strtoul(crc, NULL, 16);
    if (jstr(json, "sha256", o->sha256, sizeof o->sha256) != 64 || !all_hex(o->sha256, 64)) { *why = "sha256"; cf_offer_wipe(o); return -1; }
    char t[64];
    if (jstr(json, "title", t, sizeof t) < 0 && jstr(json, "game", t, sizeof t) < 0) snprintf(t, sizeof t, "A GAME");
    size_t n = 0;
    for (const char *p = t; *p && n < sizeof o->title - 1; p++) o->title[n++] = (*p >= 0x20 && *p < 0x7F) ? (char)toupper((unsigned char)*p) : '?';
    o->title[n] = 0;
    if (!n) snprintf(o->title, sizeof o->title, "A GAME");
    return 1;
}

/* ---------------------------------------------------------------- 1.0.7: an HTTP answer, as it arrives */
void cf_http_ans_init(cf_http_ans_t *a) { memset(a, 0, sizeof *a); }
static void ans_body(cf_http_ans_t *a, char ch)
{
    if (a->bn + 1u < CF_HTTP_BODY) { a->body[a->bn++] = ch; a->body[a->bn] = 0; }
}
/* a whole header line (CRLF cut off) */
static int ans_line(cf_http_ans_t *a)
{
    char *l = a->line;
    if (a->st == 0) {
        if (strncmp(l, "HTTP/1.", 7) || sscanf(l + 7, "%*d %d", &a->code) != 1 || a->code < 100 || a->code > 599) return -1;
        a->st = 1;
        return 0;
    }
    if (!l[0]) {                                  /* the blank line: the body starts */
        if (a->chunked) { a->st = 3; return 0; }
        if (!a->have_len) a->len = 0;             /* no length: our answers always have one, or are empty */
        if (a->len > CF_HTTP_BODY_MAX) return -1;
        a->left = a->len;
        a->st = a->left ? 2 : 9;
        return a->st == 9 ? 1 : 0;
    }
    if (!strncasecmp(l, "content-length:", 15)) { a->len = (uint32_t)strtoul(l + 15, NULL, 10); a->have_len = 1; }
    else if (!strncasecmp(l, "transfer-encoding:", 18) && strstr(l + 18, "chunked")) a->chunked = 1;
    else if (!strncasecmp(l, "connection:", 11)) { const char *v = l + 11; while (*v == ' ') v++; a->closing = !strncasecmp(v, "close", 5); }
    return 0;
}
int cf_http_ans_feed(cf_http_ans_t *a, const char *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        const char ch = b[i];
        if (a->st < 0) return -1;
        if (a->st == 9) return 1;
        if (a->st == 2 || a->st == 4) {           /* body bytes */
            ans_body(a, ch);
            if (++a->total > CF_HTTP_BODY_MAX) { a->st = -1; return -1; }
            if (--a->left == 0) { if (a->st == 2) { a->st = 9; return 1; } a->st = 5; a->ln = 0; }
            continue;
        }
        /* a line (status, header, chunk size, the CRLF after a chunk, the trailer) */
        if (ch == '\n') {
            if (a->ln && a->line[a->ln - 1] == '\r') a->ln--;
            a->line[a->ln < CF_HTTP_LINE ? a->ln : CF_HTTP_LINE - 1] = 0;
            a->ln = 0;
            int r = 0;
            if (a->st <= 1) r = ans_line(a);
            else if (a->st == 3) {
                char *e;
                const unsigned long k = strtoul(a->line, &e, 16);
                if (e == a->line || k > CF_HTTP_BODY_MAX) r = -1;
                else if (k == 0) a->st = 6;
                else { a->left = (uint32_t)k; a->st = 4; }
            } else if (a->st == 5) a->st = 3;     /* the CRLF after a chunk's data */
            else if (a->st == 6) { if (!a->line[0]) { a->st = 9; r = 1; } }
            if (r < 0) { a->st = -1; return -1; }
            if (r > 0) return 1;
            continue;
        }
        if (a->ln < CF_HTTP_LINE - 1) a->line[a->ln] = ch;
        if (a->ln < 0xFFFF) a->ln++;
        if (a->ln > 8192) { a->st = -1; return -1; }   /* not a header line */
    }
    return a->st == 9 ? 1 : 0;
}

/* 1.0.4 (owner 2026-10-19): on the charger, or the battery icon is not red (above the kind's "low"
   mark, roughly 15 %). 1.0.3 asked for the "mid" mark + 0.05 V, which refused a green icon. */
int cf_power_policy(int valid, float level_v, float low_v, int charging)
{
    if (charging) return 1;
    return valid && level_v > low_v;
}


/* ---------------------------------------------------------------- the machine */
const char *cf_install_state(const cf_inst_t *c)
{
    static const char *const k[CI_NSTATES] = {
        "idle", "receive: freeing memory", "receive: registering", "receive: getting a code", "receive: waiting for a game",
        "receive: asking the server", "receive: resuming", "receive: paused", "receive: code expired", "receive: failed",
        "receive: cancelling", "reading the header", "waiting for battery", "telling the server", "writing", "reporting", "reporting: waiting for memory",
        "receive: get games call" };
    return c->st >= 0 && c->st < CI_NSTATES ? k[c->st] : "?";
}

static int due(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

/* 1.0.5: INSTALL ANYWAY on (A on BATTERY LOW) / off (the install ended) */
static void anyway_set(cf_inst_t *c, int on)
{
    if (!on && !c->anyway) return;
    c->anyway = on;
    if (c->h.power_anyway) c->h.power_anyway(c->h.user, on);
}

int cf_rx_code_ok(const char *s)
{
    if (!s || strlen(s) != CF_RX_CODE_LEN) return 0;
    for (const char *p = s; *p; p++) if (!strchr(CF_RX_ALPHABET, *p)) return 0;
    return 1;
}

void cf_install_init(cf_inst_t *c, const cf_inst_host_t *h, uint32_t now)
{
    memset(c, 0, sizeof *c);
    c->h = *h;
    c->now = now;
    c->btn = -1;
    /* power was lost (or the unit restarted) in the middle of an install: it waits for A (no call is made by itself) */
    if (c->h.pending && c->h.pending(c->h.user, c->unf_id, c->unf_title)) {
        c->unfinished = 1;
        logf_(c, "cf: install: \"%s\" was not finished: SYSTEM > DOWNLOAD, A resumes it", c->unf_title);
    }
}

void cf_install_button(cf_inst_t *c, int a) { c->btn = a ? 1 : 0; }
static int take_btn(cf_inst_t *c) { const int b = c->btn; c->btn = -1; return b; }

/* the states that make a device API call (the answer buffer is in use) */
static int in_call(int st) { return st == CI_RX_REG || st == CI_RX_START || st == CI_RX_POLL || st == CI_RX_RESUME || st == CI_RX_DRAIN || st == CI_ACKDL || st == CI_REPORT || st == CI_RX_SIDE; }
/* the states receive mode's hold stays on for */
static int keeps_hold(int st)
{
    return st == CI_RX_HOLD || st == CI_RX_REG || st == CI_RX_START || st == CI_RX_WAIT || st == CI_RX_POLL || st == CI_RX_RESUME ||
           st == CI_RX_PAUSED || st == CI_PEEK || st == CI_WAIT_POWER || st == CI_ACKDL || st == CI_ARMED || st == CI_REPORT ||
           st == CI_RX_DRAIN || st == CI_REPORT_WAIT || st == CI_RX_SIDE;   /* 1.0.3: the code goes back over receive mode's kept connection, then the hold ends */
}

/* gamecomp1: only a report waiting on its minute timer is left (its quick tries over, receive mode ended): a game that waits for
   the report may start now (the report goes on being tried; unit 1 2026-10-22: a staged install's report held ~0.3 KB short of
   the TLS gate kept its play waiting 6 minutes) */
int cf_install_report_only_waits(const cf_inst_t *c)
{
    return !c->rx && c->rep_left > 0 && c->rep_quick >= CF_INST_REPORT_QUICK && c->st == CI_IDLE;
}
int cf_install_busy(const cf_inst_t *c)
{
    return c->rx || c->rep_left > 0 || !(c->st == CI_IDLE || c->st == CI_RX_EXPIRED || c->st == CI_RX_FAIL);
}

/* the answer buffer: taken for a call, given back when no call is under way and none can still write it */
static char *io(cf_inst_t *c)
{
    if (!c->buf) c->buf = (char *)malloc(CF_INST_BUF);
    if (c->buf) c->buf[0] = 0;
    return c->buf;
}
static void io_release(cf_inst_t *c)
{
    if (c->buf && !in_call(c->st) && !(c->h.call_busy && c->h.call_busy(c->h.user))) { memset(c->buf, 0, CF_INST_BUF); free(c->buf); c->buf = NULL; }
}
/* one device API call: 1 = started (t0 = now) */
static int api(cf_inst_t *c, const char *path, const char *body)
{
    if (!io(c) || !c->h.api_start(c->h.user, path, body, c->buf, CF_INST_BUF)) return 0;
    c->t0 = c->now;
    return 1;
}
/* 0 = still running; else 1 and *code = the HTTP code (-1 no connection or no answer in time) */
static int api_done(cf_inst_t *c, int *code)
{
    int r = c->h.api_result(c->h.user);
    if (r == 0 && !due(c->now, c->t0 + CF_INST_API_MS)) return 0;
    *code = r == 0 ? -1 : r;
    return 1;
}

/* ---- receive mode's hold: the link gives its heap to the TLS calls (on), takes it back (off) */
static void rx_on(cf_inst_t *c)
{
    if (c->rx) return;
    c->rx = 1;
    if (c->h.hold) c->h.hold(c->h.user, 1);
}
static void rx_off(cf_inst_t *c)
{
    if (!c->rx) return;
    c->rx = 0;
    if (c->h.hold) c->h.hold(c->h.user, 0);
}
static void rx_fail(cf_inst_t *c, const char *rows)
{
    c->st = CI_RX_FAIL;
    c->fail_rows = rows;
    {   /* the console says why in plain words ("NO  WI-FI|..." -> "NO WI-FI, ...") */
        char w[44]; size_t n = 0;
        for (const char *p = rows; *p && n < sizeof w - 3; p++) {
            if (*p == '|') { w[n++] = ','; w[n++] = ' '; continue; }
            if (*p == ' ' && p[1] == ' ') continue;
            w[n++] = (char)tolower((unsigned char)*p);
        }
        w[n] = 0;
        logf_(c, "cf: receive: %s", w);
    }
}
static void rx_expired(cf_inst_t *c)
{
    if (c->cat && c->rx && c->cat_renew < CF_CAT_RENEW_MAX) {
        /* lane ui110dev: GET GAMES has no code on the screen: a new one, the page goes on */
        c->cat_renew++;
        memset(c->code, 0, sizeof c->code);
        c->st = CI_RX_HOLD;
        c->t0 = c->now;
        logf_(c, "cf: get games: the code ended, a new one (%d)", c->cat_renew);
        return;
    }
    memset(c->code, 0, sizeof c->code);
    c->chain_wait = 0;
    c->n_expired++;
    c->st = CI_RX_EXPIRED;
    logf_(c, "cf: receive: the code expired (A: a new one)");
}
static void busy_clear(cf_inst_t *c);
static void rx_begin(cf_inst_t *c, int resume)
{
    rx_on(c);
    c->rx_resume = resume;
    c->rx_reg_tries = c->rx_fails = c->rx_peeks = c->rx_cancel_sent = c->rx_inflight = c->rx_sent = 0;
    busy_clear(c);
    c->rx_conn_tries = 0; c->rx_wait_until = 0;
    memset(c->code, 0, sizeof c->code);
    c->st = CI_RX_HOLD;
    c->t0 = c->now;
    logf_(c, resume ? "cf: receive: resuming %s (the link stops meanwhile)" : "cf: receive: started%s (the link stops meanwhile)", resume ? c->unf_title : "");
}

void cf_install_receive(cf_inst_t *c)
{
    if (!(c->st == CI_IDLE || c->st == CI_RX_EXPIRED || c->st == CI_RX_FAIL)) return;   /* already receiving or installing */
    if (c->unfinished) return;                         /* the page shows INSTALL NOT FINISHED: A resumes, B cancels */
    c->res_shown = 0;
    c->cat = 0; c->cat_renew = 0; c->side_st = 0; c->cat_pending = 0;
    if (c->h.fpga_ok && !c->h.fpga_ok(c->h.user)) { rx_fail(c, "UPDATE  THIS|CHROMATIC  FIRST"); return; }
    rx_begin(c, 0);
}

/* lane ui110dev: GET GAMES */
void cf_install_catalog(cf_inst_t *c)
{
    if (c->st == CI_RX_DRAIN || c->st == CI_REPORT || c->st == CI_REPORT_WAIT) { c->cat_pending = 1; return; }   /* (the last one still ending: right after) */
    if (!(c->st == CI_IDLE || c->st == CI_RX_EXPIRED || c->st == CI_RX_FAIL)) { if (c->rx) c->cat = 1; return; }   /* (receiving already: the page uses it) */
    cf_install_receive(c);
    c->cat = 1;
    if (c->st == CI_RX_HOLD) logf_(c, "cf: get games: receive mode for the catalog (no code on the screen)");
}
int cf_install_has_code(const cf_inst_t *c) { return c->code[0] && (c->st == CI_RX_WAIT || c->st == CI_RX_POLL || c->st == CI_RX_SIDE); }
int cf_install_side(cf_inst_t *c, const char *path, const char *body, char *out, int cap)
{
    if (!cf_install_has_code(c) || c->side_st == 1 || c->side_st == 2 || !out || cap < 2) return -1;
    snprintf(c->side_path, sizeof c->side_path, "%s", path);
    snprintf(c->side_body, sizeof c->side_body, body, c->code);   /* (the format's one %s: the code) */
    c->side_out = out; c->side_cap = cap; out[0] = 0;
    c->side_code = 0;
    c->side_st = 1;
    return 0;
}
int cf_install_side_done(cf_inst_t *c, int *code)
{
    if (c->side_st == 0) return -1;
    if (c->side_st != 3) {
        /* receive mode went (B, an install, a failure) before the call: no answer will come */
        if (!c->rx || !(c->st == CI_RX_WAIT || c->st == CI_RX_POLL || c->st == CI_RX_SIDE || c->st == CI_RX_HOLD || c->st == CI_RX_START || c->st == CI_RX_REG)) {
            c->side_st = 0; *code = -1; return -1;
        }
        return 0;
    }
    c->side_st = 0;
    *code = c->side_code;
    return 1;
}

/* after the hold (and after a registration): register, or ask for a code, or for the unfinished install */
static void rx_next(cf_inst_t *c)
{
    int ok;
    if (c->h.registered && !c->h.registered(c->h.user)) { ok = api(c, "/link_device/register", "{}"); if (ok) c->st = CI_RX_REG; }
    else if (c->rx_resume) {
        char body[64];
        snprintf(body, sizeof body, "{\"id\":\"%s\"}", c->unf_id);
        ok = api(c, "/link_device/recv/resume", body);
        if (ok) c->st = CI_RX_RESUME;
    } else {
        char sb[160];
        if (c->h.start_body) c->h.start_body(c->h.user, sb, (int)sizeof sb); else snprintf(sb, sizeof sb, "%s", CF_RX_START_BODY);
        ok = api(c, "/link_device/recv/start", sb);
        if (ok) c->st = CI_RX_START;
    }
    if (!ok) {   /* the link task is busy: the hold state tries again (it gives up after the API time) */
        c->st = CI_RX_HOLD;
        if (due(c->now, c->t0 + CF_INST_API_MS)) rx_fail(c, "NO  CONNECTION|TRY  AGAIN");
    }
}

/* how a failed call reads on the screen */
static const char *call_rows(int code)
{
    if (code < 0) return "NO  CONNECTION|TRY  AGAIN";
    if (code == 401) return "NOT  REGISTERED|TRY  AGAIN  LATER";
    if (code == 429) return "TOO  MANY  TRIES|WAIT  A  MINUTE";
    return "SERVER  BUSY|TRY  AGAIN  LATER";
}

/* 1.0.3: a 429 (the server's rate limits) is not a failure of receive mode. The wait: the body's retry_after_s
   (the limiter middleware sends it with Retry-After), at least the poll interval, plus 0-1 s of jitter; without
   one, 4 s doubling to 60 s (plus jitter). One answer that is not 429 resets it. The code and its timer stay; the
   page says BUSY, RETRYING. (Beta 2026-10-16: the receive routes also counted against a 200-per-hour-per-IP
   default, so a unit polling for ~17 min was refused for the rest of the hour; after 5 refused polls the unit
   showed NO CONNECTION / TOO MANY TRIES.) A 429 without retry_after_s is the handler's own limit (too many
   codes asked for, too many tries of a code): that one keeps its words. */

static uint32_t busy_wait(cf_inst_t *c)
{
    uint32_t ra = 0, w;
    c->rx_jit = c->rx_jit * 1103515245u + 12345u + c->now;
    const uint32_t jit = (c->rx_jit >> 8) % 1001u;
    if (c->buf && !jnum(c->buf, "retry_after_s", &ra) && ra > 0u && ra < 3600u) {
        w = ra * 1000u;
        if (w < c->poll_ms) w = c->poll_ms;
        c->rx_busy_wait = 0;
    } else {
        c->rx_busy_wait = !c->rx_busy_wait ? CF_RX_BUSY_FIRST_MS : (c->rx_busy_wait * 2u > CF_RX_BUSY_MAX_MS ? CF_RX_BUSY_MAX_MS : c->rx_busy_wait * 2u);
        w = c->rx_busy_wait;
    }
    if (!c->rx_busy) c->rx_busy_since = c->now;
    c->rx_busy = 1;
    logf_(c, "cf: receive: the server is busy (429): again in %lu ms", (unsigned long)(w + jit));
    return w + jit;
}
static int has_retry_after(const cf_inst_t *c)
{
    uint32_t ra = 0;
    return c->buf && !jnum(c->buf, "retry_after_s", &ra);
}
static void busy_clear(cf_inst_t *c) { c->rx_busy = 0; c->rx_busy_wait = 0; }

/* cartspeed2: a device call that got no connection at all (-1) while receive mode starts: asked again
   twice, 2 s apart, before NO CONNECTION. Unit 1, 10 receives each right after an install: 3 got -1 from
   recv/start (the TLS handshake ran out of heap: largest free block 8 KiB a few seconds after the last
   install, the link session's buffers still on their way back); the next tries find the heap whole. */
static int rx_again(cf_inst_t *c, int code)
{
    if (code >= 0 || c->rx_conn_tries >= 2 || due(c->now, c->t0 + 20000u)) return 0;   /* (a call that hung: no more) */
    c->rx_conn_tries++;
    c->st = CI_RX_HOLD;
    c->t0 = c->now;
    c->rx_wait_until = c->now + 2000u;
    if (!c->rx_wait_until) c->rx_wait_until = 1;
    logf_(c, "cf: receive: no connection (try %d of 3): again in 2 s", c->rx_conn_tries);
    return 1;
}

/* B before the write starts: a call in flight is waited for; the code is given back, then the hold goes.
   1.0.3: the hold (and with it receive mode's kept TLS connection) ends only after the cancel call, so the
   cancel goes over the kept connection (no new handshake: unit 2, beta, the site said "expired" 9.0 s after B
   when the cancel needed a connection of its own; the hold's end closed the kept one first) */
static void rx_abort(cf_inst_t *c)
{
    const int inflight = c->st == CI_RX_REG || c->st == CI_RX_START || c->st == CI_RX_POLL || c->st == CI_RX_RESUME || c->st == CI_RX_SIDE;
    logf_(c, "cf: receive: cancelled on the unit");
    c->chain_wait = 0;
    c->rx_inflight = inflight ? c->st : 0;
    c->rx_cancel_sent = 0;
    c->st = CI_RX_DRAIN;
}

/* queue a report; sent now if the link task is free, else again in a while */
static void report(cf_inst_t *c, const char *id, const char *state, const char *detail)
{
    snprintf(c->rep_id, sizeof c->rep_id, "%s", id);
    snprintf(c->rep_state, sizeof c->rep_state, "%s", state);
    snprintf(c->rep_detail, sizeof c->rep_detail, "%s", detail);
    c->rep_left = CF_INST_REPORT_TRIES;
    c->rep_at = c->now;
    c->rep_quick = 0;
}
static int report_try(cf_inst_t *c)
{
    if (c->rep_left <= 0) return 0;
    char body[200];
    snprintf(body, sizeof body, "{\"id\":\"%s\",\"state\":\"%s\",\"detail\":\"%s\"}", c->rep_id, c->rep_state, c->rep_detail);
    if (!api(c, "/link_device/cart_install/ack", body)) return 0;
    c->rep_left--;
    c->st = CI_REPORT;
    return 1;
}

/* what the unit's screen says about an install that ended (two rows of at most 18 characters,
   "|" between them; the OSD font has no lower case). Not for a B: a NO is not answered with a screen. */
static void show_result(cf_inst_t *c, int ok, const char *rows)
{
    /* lane m110u: a cartridge job says where the game is now (the cart game restarts with it) */
    c->res_note = ok ? (c->offer.kind == CF_KIND_SAVE ? "THE  SAVE  IS  ON|THE  CARTRIDGE" : "ON  THE  CARTRIDGE|") : rows;
    c->res_title = ok ? "INSTALLED" : "NOT  INSTALLED";
    if (c->offer.kind == CF_KIND_BACKUP) {     /* 1.0.6 */
        c->res_note = ok ? "SAVE  IT  ON|THE  WEBSITE" : rows;
        c->res_title = ok ? "BACKED  UP" : "BACKUP  STOPPED";
    }
    if (c->offer.kind == CF_KIND_PLAY) {       /* 1.1.0 */
        c->res_note = ok ? "" : rows;
        c->res_title = ok ? "PLAYING" : "NOT  PLAYING";
    }
    if (c->offer.kind == CF_KIND_PLAY && c->offer.lib == CF_LIB_KEEP) {   /* lane ui110dev */
        c->res_note = ok ? "PLAY  IT  FROM|SYSTEM  >  GAMES" : rows;   /* lane m110u: what to do next, as SHARE says it */
        c->res_title = ok ? "ON  CHROMATIC" : "NOT  KEPT";
    }
    if (c->offer.sd && c->offer.kind == CF_KIND_PLAY) {   /* lane sdcard1 */
        c->res_note = ok ? (c->offer.sd == CF_SD_PLAY ? "FROM  THE  CARD|" : "PLAY  IT  FROM|SYSTEM  >  GAMES") : rows;
        c->res_title = ok ? (c->offer.sd == CF_SD_PLAY ? "PLAYING" : "ON  THE  CARD") : "NOT  ON  THE  CARD";
    }
    /* lane mygames1: the save that came with it, said on the result's second row */
    if (ok && c->offer.kind == CF_KIND_PLAY && c->offer.save_size) c->res_note = c->offer.sd == CF_SD_PLAY || (!c->offer.sd && c->offer.lib != CF_LIB_KEEP) ? "WITH  ITS  SAVE|" : "WITH  ITS  SAVE.|PLAY  IT  FROM  GAMES";
    if (ok && c->offer.kind == CF_KIND_PLAY && !c->offer.sd && c->offer.lib == CF_LIB_NONE) {
        c->res_title = "PLAYING  NOW";
        if (!c->offer.save_size) c->res_note = "NOT  KEPT|";              /* lane m110u: as SHARE's PLAY NOW says it */
    }
    if (ok && c->offer.kind == CF_KIND_ROMSAVE) c->res_note = "ON  THE  CARTRIDGE|WITH  ITS  SAVE";
    if (c->offer.sd && c->offer.kind == CF_KIND_SAVE) {
        c->res_note = ok ? "IN  SAVES  ON|THE  CARD" : rows;
        c->res_title = ok ? "SAVE  ON  THE  CARD" : "NOT  ON  THE  CARD";
    }
    c->res_shown = 1;
    c->res_until = c->now + CF_INST_RESULT_MS;
    c->res_reopen_at = c->now + CF_INST_RESULT_REOPEN_MS;
    if (c->h.open) c->h.open(c->h.user);
}

static void finish(cf_inst_t *c, int ok, const char *state, const char *detail)
{
    snprintf(c->done_id, sizeof c->done_id, "%s", c->offer.id);
    if (ok) c->n_installs++; else if (!strcmp(state, "declined")) c->n_declined++; else c->n_failed++;
    logf_(c, "cf: install of %s: %s%s%s", c->offer.title, state, detail[0] ? ", " : "", detail);
    c->unfinished = 0;
    c->arming = 0;
    anyway_set(c, 0);
    report(c, c->offer.id, state, detail);
    {   /* the key goes with the install; the report names only the id */
        char id[33], title[17];
        const uint8_t kind = c->offer.kind, sd = c->offer.sd, lib = c->offer.lib;
        const uint32_t sv = c->offer.save_size;
        memcpy(id, c->offer.id, sizeof id); memcpy(title, c->offer.title, sizeof title);
        cf_offer_wipe(&c->offer);
        memcpy(c->offer.id, id, sizeof id); memcpy(c->offer.title, title, sizeof title);
        c->offer.kind = kind; c->offer.sd = sd; c->offer.lib = lib;   /* (the result screen's words) */
        c->offer.save_size = sv;                                        /* (lane mygames1: WITH ITS SAVE) */
    }
    memset(c->code, 0, sizeof c->code);
    c->st = CI_IDLE;
    /* a report not sent now (the install's connection still closing) goes again in a few seconds */
    if (!report_try(c)) c->rep_at = c->now + CF_INST_REPORT_SOON_MS;
}

/* the job did not finish but the cart can be finished later: in receive mode it resumes by itself a few times */
static void unfinished(cf_inst_t *c, const char *why)
{
    c->unfinished = 1; c->unf_opened = 1;
    c->arming = 0;
    if (!(c->rx && c->auto_tries < CF_INST_MAX_AUTO)) anyway_set(c, 0);   /* an automatic resume keeps the player's choice */
    snprintf(c->unf_id, sizeof c->unf_id, "%s", c->offer.id);
    snprintf(c->unf_title, sizeof c->unf_title, "%s", c->offer.title);
    cf_offer_wipe(&c->offer);                    /* the resume brings the key again */
    memset(c->code, 0, sizeof c->code);
    if (c->rx && c->auto_tries < CF_INST_MAX_AUTO) {
        c->st = CI_RX_PAUSED;
        c->resume_at = c->now + CF_INST_RETRY_MS;
        logf_(c, "cf: install of %s is NOT FINISHED (%s): the cart will not start until it is; it resumes in %u s", c->unf_title, why, (unsigned)(CF_INST_RETRY_MS / 1000u));
    } else {
        c->st = CI_IDLE;
        logf_(c, "cf: install of %s is NOT FINISHED (%s): SYSTEM > DOWNLOAD, A resumes it", c->unf_title, why);
        if (c->h.open) c->h.open(c->h.user);
    }
}

/* B on INSTALL NOT FINISHED: the journal forgets it, the server is told */
static void give_up(cf_inst_t *c)
{
    c->h.cancel(c->h.user);
    snprintf(c->offer.id, sizeof c->offer.id, "%s", c->unf_id);
    snprintf(c->offer.title, sizeof c->offer.title, "%s", c->unf_title);
    finish(c, 0, "failed", "cancelled on the unit");
}

/* a game came (or the unfinished one came back): take it */
static void got_offer(cf_inst_t *c, const cf_offer_t *op)
{
    c->chain_wait = 0;
    c->offer = *op;
    c->note[0] = 0;
    c->n_offers++;
    c->auto_tries = 0;
    c->rx_sent = 1;
    if (op->kind == CF_KIND_BACKUP) {
        /* 1.0.6: a backup: no header to read, no battery rule (the cart is only read); "reading" goes first */
        logf_(c, "cf: receive: a backup of this cart was asked for");
        char body[120];
        snprintf(body, sizeof body, "{\"id\":\"%s\",\"state\":\"reading\",\"detail\":\"\"}", c->offer.id);
        c->resuming = 0;
        c->st = CI_ACKDL;
        if (!api(c, "/link_device/cart_install/ack", body)) c->t0 = c->now - CF_INST_API_MS;   /* busy: skip it */
        return;
    }
    logf_(c, "cf: receive: %s arrived (%u bytes, crc32 %08x)", op->title, (unsigned)op->size, (unsigned)op->crc32);
    if (c->h.peek_start && c->h.peek_start(c->h.user, &c->offer)) { c->t0 = c->now; c->st = CI_PEEK; return; }
    c->power_until = c->now + CF_RX_POWER_MS;
    c->st = CI_WAIT_POWER;
    if (c->h.power_ok && !c->h.power_ok(c->h.user)) logf_(c, "cf: install: %s waits for enough battery (or the charger)", c->offer.title);   /* (1.0.5: a patch has no header read) */
}

/* the answer to a poll / a resume in c->buf: 1 = an offer (in *o), 0 = none, -1 = one the device does not take */
static int take_offer(cf_inst_t *c, cf_offer_t *o)
{
    const char *why;
    const int r = cf_install_parse(c->buf, c->h.gateway_base ? c->h.gateway_base(c->h.user) : NULL, o, &why);
    if (r < 0) logf_(c, "cf: receive: an offer the device does not take (%s)", why);
    return r;
}

static void start_resume(cf_inst_t *c, cf_offer_t *op)
{
    snprintf(op->title, sizeof op->title, "%s", c->unf_title);
    c->offer = *op;
    c->auto_tries++;
    c->n_resumes++;
    logf_(c, "cf: install of %s resumes (try %d)", c->offer.title, c->auto_tries);
    /* "resuming" first (the server keeps the image 2 h longer for every such report), then the write */
    char body[120];
    snprintf(body, sizeof body, "{\"id\":\"%s\",\"state\":\"resuming\",\"detail\":\"\"}", c->offer.id);
    c->resuming = 1;
    c->st = CI_ACKDL;
    if (!api(c, "/link_device/cart_install/ack", body)) c->t0 = c->now - CF_INST_API_MS;   /* busy: skip it */
}

void cf_install_tick(cf_inst_t *c, uint32_t now)
{
    c->now = now;
    io_release(c);
    if (c->rx && !keeps_hold(c->st)) rx_off(c);
    const int b = take_btn(c);
    int code;
    switch (c->st) {
    case CI_IDLE:
        if (c->cat_pending) { c->cat_pending = 0; cf_install_catalog(c); return; }   /* lane ui110dev: GET GAMES asked while the last receive mode ended */
        /* how the last install ended stays up until A / B (or a while) */
        if (c->res_shown && c->res_reopen_at && due(now, c->res_reopen_at)) {
            c->res_reopen_at = 0;
            if (c->h.open) c->h.open(c->h.user);           /* SYSTEM > DOWNLOAD over the restarted game */
        }
        if (c->rx_after) { c->rx_after = 0; c->res_shown = 0; cf_install_receive(c); return; }
        if (c->res_shown) {
            /* cartspeed2: A on the result is NEXT GAME (receive mode at once: a second send right after
               the first used to need A twice, the first only closed the result), B / time closes it */
            if (b == 1) { c->res_shown = 0; cf_install_receive(c); return; }
            if (b == 0 || due(now, c->res_until)) c->res_shown = 0;
        } else if (c->unfinished) {
            if (!c->unf_opened) { c->unf_opened = 1; if (c->h.open) c->h.open(c->h.user); }
            if (b == 1) { c->auto_tries = 0; rx_begin(c, 1); return; }
            if (b == 0) { give_up(c); return; }
        } else if (b == 1) { cf_install_receive(c); return; }
        /* a report that did not get through: again on a timer (not a poll: nothing is asked for) */
        if (c->rep_left > 0 && due(now, c->rep_at) && !report_try(c)) c->rep_at = now + CF_INST_RETRY_MS;
        return;
    case CI_RX_EXPIRED:
    case CI_RX_FAIL:
        if (b == 1) { if (c->st == CI_RX_FAIL && c->rx_resume && c->unfinished) { c->auto_tries = 0; rx_begin(c, 1); } else { c->st = CI_IDLE; cf_install_receive(c); } }
        else if (b == 0) c->st = CI_IDLE;
        return;
    case CI_RX_HOLD: {
        if (b == 0) { rx_abort(c); return; }
        if (c->rx_wait_until) { if (!due(now, c->rx_wait_until)) return; c->rx_wait_until = 0; }
        const int held = !c->h.held || c->h.held(c->h.user);
        if (!held && !due(now, c->t0 + CF_RX_HOLD_MS)) return;
        const int w = c->h.wifi ? c->h.wifi(c->h.user) : 2;
        if (w == 0) { rx_fail(c, "NO  WI-FI|SET  UP  WI-FI  FIRST"); return; }
        if (w == 1) { if (due(now, c->t0 + CF_RX_WIFI_MS)) rx_fail(c, "NO  WI-FI|CHECK  THE  NETWORK"); return; }
        rx_next(c);
        return;
    }
    case CI_RX_REG:
        if (b == 0) { rx_abort(c); return; }
        if (!api_done(c, &code)) return;
        if (code == 200 && (!c->h.registered || c->h.registered(c->h.user))) { busy_clear(c); logf_(c, "cf: receive: this unit is registered"); c->t0 = now; rx_next(c); return; }
        if (code == 429 && has_retry_after(c) && !(c->rx_busy && due(now, c->rx_busy_since + CF_RX_BUSY_GIVEUP_MS))) {
            const uint32_t w = busy_wait(c);
            if (c->buf) memset(c->buf, 0, CF_INST_BUF);
            c->st = CI_RX_HOLD; c->t0 = now; c->rx_wait_until = now + w; if (!c->rx_wait_until) c->rx_wait_until = 1;
            return;
        }
        if (rx_again(c, code)) return;
        rx_fail(c, call_rows(code == 200 ? 502 : code));
        return;
    case CI_RX_START: {
        if (b == 0) { rx_abort(c); return; }
        if (!api_done(c, &code)) return;
        if (code == 401 && c->rx_reg_tries == 0 && c->h.reg_forget) {
            /* the server does not know this registration (any more): one new one */
            c->rx_reg_tries = 1;
            c->h.reg_forget(c->h.user);
            c->t0 = now;
            rx_next(c);
            return;
        }
        if (code == 429 && has_retry_after(c) && !(c->rx_busy && due(now, c->rx_busy_since + CF_RX_BUSY_GIVEUP_MS))) {
            /* the limiter's 429 (it says how long): asked again after the wait, still GETTING READY */
            const uint32_t w = busy_wait(c);
            if (c->buf) memset(c->buf, 0, CF_INST_BUF);
            c->st = CI_RX_HOLD;
            c->t0 = now;
            c->rx_wait_until = now + w;
            if (!c->rx_wait_until) c->rx_wait_until = 1;
            return;
        }
        if (code != 200) { busy_clear(c); if (rx_again(c, code)) return; rx_fail(c, call_rows(code)); return; }
        busy_clear(c);
        uint32_t ex = 0, pm = 0;
        char k[CF_RX_CODE_LEN + 2];
        if (jstr(c->buf, "code", k, sizeof k) != CF_RX_CODE_LEN || !cf_rx_code_ok(k)) { rx_fail(c, "SERVER  ERROR|TRY  AGAIN  LATER"); return; }
        memcpy(c->code, k, sizeof c->code);
        if (jnum(c->buf, "expires_in", &ex) || ex < 60u || ex > 900u) ex = 600u;
        if (jnum(c->buf, "poll_ms", &pm) || pm < 2000u || pm > 10000u) pm = CF_RX_POLL_MS;
        memset(c->buf, 0, CF_INST_BUF);
        c->code_until = now + ex * 1000u;
        c->poll_ms = pm;
        c->next_poll = now + pm;
        c->n_codes++;
        c->st = CI_RX_WAIT;
        logf_(c, "cf: receive: a code is on the screen (valid %u s), waiting for a game", (unsigned)ex);   /* never the code itself */
        return;
    }
    case CI_RX_WAIT: {
        if (b == 0) { rx_abort(c); return; }
        if (!c->rx_sent && due(now, c->code_until)) { rx_expired(c); return; }
        /* 1.0.8: the backup's report that did not go yet goes first (over receive mode's connection) */
        if (c->chain_wait && c->rep_left > 0 && due(now, c->rep_at)) { if (report_try(c)) return; c->rep_at = now + 2000u; }
        if (c->side_st == 1) {
            /* lane ui110dev: GET GAMES's call first (a page, an order) */
            if (api(c, c->side_path, c->side_body)) { c->side_st = 2; c->st = CI_RX_SIDE; }
            return;
        }
        if (!due(now, c->next_poll)) return;
        char body[40];
        snprintf(body, sizeof body, "{\"code\":\"%s\"}", c->code);
        if (!api(c, "/link_device/recv/poll", body)) { c->next_poll = now + 1000u; return; }
        c->n_polls++;
        c->st = CI_RX_POLL;
        return;
    }
    case CI_RX_POLL: {
        if (b == 0) { rx_abort(c); return; }
        if (!api_done(c, &code)) return;
        c->st = CI_RX_WAIT;
        c->next_poll = now + c->poll_ms;
        if (code == 429) { c->next_poll = now + busy_wait(c); if (c->buf) memset(c->buf, 0, CF_INST_BUF); return; }
        busy_clear(c);
        if (code != 200) {
            if (code == 401) { rx_fail(c, call_rows(401)); return; }
            if (++c->rx_fails >= CF_RX_POLL_FAILS) rx_fail(c, call_rows(code));
            return;
        }
        c->rx_fails = 0;
        char ses[12] = "";
        (void)jstr(c->buf, "session", ses, sizeof ses);
        if (!strcmp(ses, "expired")) { rx_expired(c); return; }
        if (strcmp(ses, "sent")) return;                    /* waiting */
        cf_offer_t *o = (cf_offer_t *)malloc(sizeof *o);   /* heap, not stack, not static: wiped and freed below */
        if (!o) return;                                     /* the next poll brings it again */
        const int r = take_offer(c, o);
        memset(c->buf, 0, CF_INST_BUF);
        if (r == 1 && !strcmp(o->id, c->done_id)) { cf_offer_wipe(o); free(o); rx_expired(c); return; }   /* this boot's last install, over */
        if (r == 1) got_offer(c, o);
        else if (r < 0) rx_fail(c, "SERVER  ERROR|TRY  AGAIN  LATER");
        cf_offer_wipe(o);
        free(o);
        return;
    }
    case CI_RX_SIDE: {
        if (b == 0) { rx_abort(c); return; }
        if (!api_done(c, &code)) return;
        if (c->side_out && c->buf) { snprintf(c->side_out, (size_t)c->side_cap, "%s", c->buf); memset(c->buf, 0, CF_INST_BUF); }
        c->side_code = code;
        c->side_st = 3;
        c->st = CI_RX_WAIT;
        if (code == 200 && strstr(c->side_path, "/order")) c->next_poll = now + 300u;   /* (the game is bound to the code: its offer at once) */
        return;
    }
    case CI_RX_RESUME: {
        if (b == 0) { rx_abort(c); return; }
        if (!api_done(c, &code)) return;
        if (rx_again(c, code)) return;
        cf_offer_t *o = code == 200 ? (cf_offer_t *)malloc(sizeof *o) : NULL;
        const int r = o ? take_offer(c, o) : -1;
        memset(c->buf, 0, CF_INST_BUF);
        if (r == 1 && !strcmp(o->id, c->unf_id)) { start_resume(c, o); cf_offer_wipe(o); free(o); return; }
        if (o) { cf_offer_wipe(o); free(o); }
        if (code == 200 && r == 0) {
            /* the server no longer has it: it can only be given up (B), or finished over USB */
            logf_(c, "cf: install of %s: the server no longer has it", c->unf_title);
            c->auto_tries = CF_INST_MAX_AUTO;
            c->st = CI_IDLE;
            show_result(c, 0, "THE  SEND  EXPIRED|SEND  IT  AGAIN");
            return;
        }
        if (c->auto_tries < CF_INST_MAX_AUTO) { c->auto_tries++; c->st = CI_RX_PAUSED; c->resume_at = now + CF_INST_RETRY_MS; return; }
        rx_fail(c, call_rows(code));
        return;
    }
    case CI_RX_PAUSED:
        if (b == 0) { rx_off(c); give_up(c); return; }
        if (b == 1 || due(now, c->resume_at)) { if (b == 1) c->auto_tries = 0; c->rx_resume = 1; c->st = CI_RX_HOLD; c->t0 = now; }
        return;
    case CI_RX_DRAIN:
        /* after a B: the call in flight ends; the code is given back (best effort, once) */
        if (c->rx_inflight) {
            if (!api_done(c, &code)) return;
            const int was = c->rx_inflight;
            c->rx_inflight = 0;
            if (was == CI_RX_START && code == 200) {
                char k[CF_RX_CODE_LEN + 2];
                if (jstr(c->buf, "code", k, sizeof k) == CF_RX_CODE_LEN && cf_rx_code_ok(k)) memcpy(c->code, k, sizeof c->code);
            }
            if (was == CI_RX_POLL && code == 200) {
                char ses[12] = "";
                (void)jstr(c->buf, "session", ses, sizeof ses);
                cf_offer_t *o = !strcmp(ses, "sent") ? (cf_offer_t *)malloc(sizeof *o) : NULL;
                if (o && take_offer(c, o) == 1) {
                    memset(c->buf, 0, CF_INST_BUF);
                    c->offer = *o;
                    cf_offer_wipe(o); free(o);
                    finish(c, 0, "declined", "");          /* a game came just as B was pressed: the sender is told */
                    return;
                }
                if (o) { cf_offer_wipe(o); free(o); }
            }
            if (c->buf) memset(c->buf, 0, CF_INST_BUF);
        }
        if (c->code[0] && !c->rx_cancel_sent) {
            char body[40];
            snprintf(body, sizeof body, "{\"code\":\"%s\"}", c->code);
            c->rx_cancel_sent = 1;
            if (api(c, "/link_device/recv/cancel", body)) { c->rx_inflight = CI_RX_DRAIN; return; }
        }
        memset(c->code, 0, sizeof c->code);
        c->st = CI_IDLE;
        rx_off(c);
        return;
    case CI_PEEK: {
        if (b == 0) { finish(c, 0, "declined", ""); return; }
        char t[17] = "", n[24] = "";
        const int r = c->h.peek_result(c->h.user, t, n);
        if (r < 0 && !due(now, c->t0 + CF_INST_PEEK_MS)) return;
        if (r == 0) {
            t[16] = 0; n[23] = 0;
            /* 1.1.2 (owner QA 2026-10-26: Emerald Color kept ON CHROMATIC as PM CRYSTAL, SHINY FARM COLOR as TILLER): the
               name the server sent (the catalog's, the owner's My games title) stays; the game's own header name only
               when the server sent none */
            if (t[0] && (!c->offer.title[0] || !strcmp(c->offer.title, "A GAME"))) snprintf(c->offer.title, sizeof c->offer.title, "%s", t);
            snprintf(c->note, sizeof c->note, "%s", n);
            c->power_until = now + CF_RX_POWER_MS;
            c->st = CI_WAIT_POWER;
            if (!c->h.power_ok(c->h.user)) logf_(c, "cf: install: %s waits for enough battery (or the charger)", c->offer.title);
            return;
        }
        /* no header: the image did not start to arrive. Poll again (the session still has it), a few times */
        logf_(c, "cf: install: the image's first bytes did not arrive (%d)", r);
        if (++c->rx_peeks >= CF_RX_PEEK_TRIES) { finish(c, 0, "failed", "the image did not arrive"); show_result(c, 0, "THE  GAME  DID|NOT  ARRIVE"); return; }
        cf_offer_wipe(&c->offer);
        c->st = CI_RX_WAIT;
        c->next_poll = now + c->poll_ms;
        return;
    }
    case CI_WAIT_POWER:
        if (b == 0) { finish(c, 0, "declined", ""); return; }
        if (!c->anyway && !c->h.power_ok(c->h.user)) {
            /* 1.0.5 (owner 2026-10-19): A = INSTALL ANYWAY, the write goes on the low battery */
            if (b == 1) {
                logf_(c, "cf: install: %s: INSTALL ANYWAY on a low battery (A)", c->offer.title);
                anyway_set(c, 1);
            } else {
                if (due(now, c->power_until)) { finish(c, 0, "failed", "battery low"); show_result(c, 0, "BATTERY  LOW|CHARGE,  SEND  AGAIN"); }
                return;
            }
        }
        {   /* tell the sender it has started, BEFORE the write: no second connection during the stream.
               The header read's connection is still closing right after it (the device call waits for
               it): wait for that, else the write would be armed beside it and refused (unit 2,
               2026-10-14: "refused to arm" one tick after the header). Skipped only after a while. */
            char body[120];
            snprintf(body, sizeof body, "{\"id\":\"%s\",\"state\":\"downloading\",\"detail\":\"\"}", c->offer.id);
            c->resuming = 0;
            if (!api(c, "/link_device/cart_install/ack", body)) {
                if (!c->ack_from) c->ack_from = now | 1u;
                if (!due(now, c->ack_from + CF_RX_ACK_WAIT_MS)) return;
                c->t0 = now - CF_INST_API_MS;   /* busy for long: skip it */
            }
            c->ack_from = 0;
            c->st = CI_ACKDL;
        }
        return;
    case CI_ACKDL: {
        if (c->offer.kind == CF_KIND_BACKUP) {
            /* 1.0.6: B stops a backup (the host's read / upload ends, arm() then says so) */
            if (b == 0 && c->h.backup_stop) { logf_(c, "cf: backup: stopped on the unit (B)"); c->h.backup_stop(c->h.user); }
            if (!c->arming && !api_done(c, &code)) return;
            const int k = c->h.arm(c->h.user, &c->offer);
            if (k == CF_ARM_WAIT) { if (!c->arming) { c->arming = 1; logf_(c, "cf: backup: reading the cart"); } return; }
            c->arming = 0;
            if (k == CF_ARM_BACKED_UP) {
                /* 1.0.8 (owner 2026-10-20): ONE code for the backup and the install: the code stays, receive mode
                   goes on (the server reopens it for the same sender at 'backed_up') */
                char keep[CF_RX_CODE_LEN + 1];
                memcpy(keep, c->code, sizeof keep);
                finish(c, 1, "backed_up", "");
                if (cf_rx_code_ok(keep)) {
                    memcpy(c->code, keep, sizeof c->code);
                    c->chain_wait = 1;
                    c->rx_sent = 0;
                    c->code_until = c->now + CF_RX_CHAIN_MS;
                    c->next_poll = c->now + c->poll_ms;
                    if (c->st == CI_IDLE) c->st = CI_RX_WAIT;   /* (the report goes from the wait, a moment later) */
                    logf_(c, "cf: backup: done; the same code waits for the game");
                    return;
                }
                show_result(c, 1, "");
                return;
            }
            if (k == CF_ARM_BK_STOPPED) { finish(c, 0, "failed", "the backup was stopped"); show_result(c, 0, "THE  WEBSITE|STOPPED  IT"); return; }
            if (k == CF_ARM_BK_READ) { finish(c, 0, "failed", "the cart read failed"); show_result(c, 0, "THE  CART  DID|NOT  READ"); return; }
            if (k == CF_ARM_BK_NET) { finish(c, 0, "failed", "the upload got nowhere"); show_result(c, 0, "NO  CONNECTION|TRY  AGAIN"); return; }
            finish(c, 0, "failed", "the backup did not start");
            show_result(c, 0, "NOT  ENOUGH  MEMORY|RESTART,  TRY  AGAIN");
            return;
        }
        if (!c->arming && !api_done(c, &code)) return;
        /* wifispeed1: the image downloads into the staging area (and is hashed) before the write is armed */
        const int a = c->h.arm(c->h.user, &c->offer);
        if (a == CF_ARM_WAIT) {
            if (!c->arming) { c->arming = 1; logf_(c, "cf: install of %s: downloading", c->offer.title); }
            return;
        }
        c->arming = 0;
        if (a == CF_ARM_NO_DATA) { unfinished(c, "the download stopped"); return; }
        /* 1.0.5, a patch: the unit read its own cart first; nothing was written */
        if (a == CF_ARM_WRONG_CART) { finish(c, 0, "failed", "not the cart this patch is for"); show_result(c, 0, "NOT  STOCK  TETRIS|OR  PARTY  BATTLE"); return; }
        if (a == CF_ARM_ALREADY) { finish(c, 0, "failed", "the cart already holds this version"); show_result(c, 0, "THE  CART  HAS|THIS  ALREADY"); return; }
        if (a == CF_ARM_PATCH_BAD) { finish(c, 0, "failed", "the patch did not check out"); show_result(c, 0, "THE  PATCH  DID|NOT  CHECK  OUT"); return; }
        if (a == CF_D_GONE) { finish(c, 0, "failed", "the image expired"); show_result(c, 0, "THE  SEND  EXPIRED|SEND  IT  AGAIN"); return; }
        if (a == CF_ARM_NO_CARD) { finish(c, 0, "failed", "no card that takes it"); show_result(c, 0, "NO  CARD  THAT|TAKES  IT"); return; }
        if (a == CF_ARM_CARD_FULL) { finish(c, 0, "failed", "the card is full"); show_result(c, 0, "THE  CARD  IS  FULL|"); return; }
        if (a == CF_ARM_CARD_FAIL) { finish(c, 0, "failed", "the card did not take it"); show_result(c, 0, "THE  CARD  DID|NOT  TAKE  IT"); return; }
        if (a == CF_ARM_LIB_FULL) { finish(c, 0, "failed", "no room to keep it"); show_result(c, 0, "NO  ROOM:  DELETE|A  GAME  FIRST"); return; }
        /* m110t: every refusal says its own reason (the final gate: a trimmed game refused for its size said NOT ENOUGH MEMORY,
           the one line every unnamed refusal showed); memory only when it was memory */
        if (a == CF_ARM_TOO_BIG) {
            const int card = c->offer.kind == CF_KIND_PLAY && c->offer.sd;
            finish(c, 0, "failed", "too big for the place it is for");
            show_result(c, 0, card ? "TOO  BIG  FOR|THE  CARD" : c->offer.kind == CF_KIND_PLAY ? "TOO  BIG  FOR|CHROMATIC" : "THE  GAME  DOES|NOT  FIT  THIS  CART");
            return;
        }
        if (a == CF_ARM_CANT_PLAY) { finish(c, 0, "failed", "not a game this Chromatic plays"); show_result(c, 0, "CHROMATIC  CANNOT|PLAY  THIS  GAME"); return; }
        if (a == CF_ARM_LIB_FAIL) { finish(c, 0, "failed", "the library did not take it"); show_result(c, 0, "IT  COULD  NOT  BE|KEPT:  SEND  AGAIN"); return; }
        if (a == CF_D_NO_ENGINE) { finish(c, 0, "failed", "no RAM cart on this FPGA"); show_result(c, 0, "UPDATE  THIS|CHROMATIC  FIRST"); return; }
        if (a == CF_D_BAD_IMAGE && !c->resuming) {
            finish(c, 0, "failed", c->offer.kind == CF_KIND_PLAY ? "the image did not check out" : "does not fit this cart");
            show_result(c, 0, c->offer.kind == CF_KIND_PLAY ? "THE  GAME  DID|NOT  CHECK  OUT" : "THE  GAME  DOES|NOT  FIT  THIS  CART");
            return;
        }
        if (a != 0) {
            if (c->resuming) { unfinished(c, "could not start again"); return; }      /* still unfinished: tried again */
            char d[40];
            snprintf(d, sizeof d, "refused to arm (%d)", a);
            finish(c, 0, "failed", d);
            show_result(c, 0, a == CF_D_ABORTED ? "NOT  ENOUGH  MEMORY|RESTART,  SEND  AGAIN" : "IT  DID  NOT  START|RESTART,  SEND  AGAIN");
            return;
        }
        c->st = CI_ARMED;
        return;
    }
    case CI_ARMED: {
        const int r = c->h.job_result(c->h.user);
        if (r < 0) return;
        char d[48];
        if (r == CF_D_OK) { finish(c, 1, "installed", ""); show_result(c, 1, ""); return; }
        const int pend = c->h.pending ? c->h.pending(c->h.user, NULL, NULL) : 0;
        if (pend && (r == CF_D_PAUSED || r == CF_D_VERIFY || r == CF_D_FAILED || r == CF_D_ABORTED)) {
            unfinished(c, r == CF_D_PAUSED ? "the link stopped" : r == CF_D_VERIFY ? "the cart did not read back right" : "the write stopped");
            return;
        }
        const char *rows;
        if (r == 0xEE) { snprintf(d, sizeof d, "no cart writer on this FPGA"); rows = "UPDATE  THIS|CHROMATIC  FIRST"; }
        else if (r == CF_D_GONE) { snprintf(d, sizeof d, "the image expired"); rows = "THE  SEND  EXPIRED|SEND  IT  AGAIN"; }
        else if (r == CF_D_OVERLAY) { snprintf(d, sizeof d, "a game patch is active: turn patches off"); rows = "TURN  PATCHES  OFF|SEND  IT  AGAIN"; }
        else if (r == CF_D_LOCKED) { snprintf(d, sizeof d, "the cart's first block is write-protected"); rows = "THIS  CART  IS|WRITE  PROTECTED"; }
        else if (r == CF_D_WPTEST) { snprintf(d, sizeof d, "the cart's write test failed: nothing erased"); rows = "WRITE  TEST  FAILED|NOTHING  ERASED"; }
        else if (r == CF_D_BAD_IMAGE) { snprintf(d, sizeof d, "%s", c->offer.size > CF_INST_WRITABLE ? "protected block not blank" : "does not fit this cart"); rows = "THE  GAME  DOES|NOT  FIT  THIS  CART"; }
        else if (r == CF_D_WRONG_CHIP) { snprintf(d, sizeof d, "not a cart this Chromatic can write"); rows = "THIS  CART  CANNOT|BE  WRITTEN"; }
        else if (r == CF_D_NO_ENGINE) { snprintf(d, sizeof d, "no cart writer on this FPGA"); rows = "UPDATE  THIS|CHROMATIC  FIRST"; }
        else { snprintf(d, sizeof d, "write code %d", r); rows = "THE  CART  WRITE|STOPPED"; }
        if (pend) {
            /* nothing more can come for it (expired): the cart keeps no first block until a game is written */
            snprintf(c->unf_id, sizeof c->unf_id, "%s", c->offer.id);
            snprintf(c->unf_title, sizeof c->unf_title, "%s", c->offer.title);
            finish(c, 0, "failed", d);
            c->unfinished = 1; c->unf_opened = 1;
            logf_(c, "cf: install of %s stopped for good (%s): the cart is unfinished", c->unf_title, d);
            show_result(c, 0, rows);
            return;
        }
        finish(c, 0, "failed", d);
        show_result(c, 0, rows);
        return;
    }
    case CI_REPORT:
        if (b == 1) c->rx_after = 1;                   /* cartspeed2: NEXT GAME pressed while the report goes out */
        if (b == 0) { c->rx_after = 0; if (c->res_shown) c->res_shown = 0; }
        if (!api_done(c, &code)) return;
        if (code == 200 || code == 404 || code == 400) c->rep_left = 0;      /* landed, or the server no longer has it */
        else if (code < 0 && !c->chain_wait && c->rep_quick < CF_INST_REPORT_QUICK && c->rep_left > 0) {
            /* gamecomp1: the call did not start (unit 1 2026-10-22: 20,212 bytes free right after a library keep, the TLS handshake
               needs 21,000; it came back by itself 3-16 s later): again in 2 s with receive mode kept (the link stays stopped, its heap
               with us), CF_INST_REPORT_QUICK times, then as before */
            c->rep_quick++;
            if (c->rep_left < CF_INST_REPORT_TRIES) c->rep_left++;   /* (a quick try is not one of the report's tries) */
            if (c->h.hold) c->h.hold(c->h.user, 1);             /* receive mode again: its idle kept client let go (unit 1: +1 KB) */
            logf_(c, "cf: install: the report did not start (%d): again in 2 s (%u of %u), receive mode kept", code, (unsigned)c->rep_quick, (unsigned)CF_INST_REPORT_QUICK);
            c->rep_at = now + 2000u;
            c->st = CI_REPORT_WAIT;
            return;
        } else { logf_(c, "cf: install: the report got %d: sent again later (%d tries left)", code, c->rep_left); c->rep_at = now + (c->chain_wait ? 5000u : CF_INST_RETRY_MS); }
        c->st = c->chain_wait ? CI_RX_WAIT : CI_IDLE;   /* 1.0.8: back to the code after a backup's report */
        if (c->chain_wait) c->next_poll = now + 500u;
        return;
    case CI_REPORT_WAIT:
        if (!due(now, c->rep_at)) return;
        if (!report_try(c)) { c->st = CI_IDLE; c->rep_at = now + CF_INST_RETRY_MS; }
        return;
    default:
        c->st = CI_IDLE;
        return;
    }
}

void cf_install_view(const cf_inst_t *c, cf_view_t *v)
{
    memset(v, 0, sizeof *v);
    switch (c->st) {
    case CI_RX_HOLD: case CI_RX_REG: case CI_RX_START: case CI_RX_RESUME:
        v->kind = CV_GETTING;
        snprintf(v->title, sizeof v->title, "%s", c->rx_busy ? "BUSY  RETRYING" : c->rx_resume ? "RESUMING" : "GETTING  READY");
        return;
    case CI_RX_WAIT: case CI_RX_POLL: case CI_RX_SIDE: {
        v->kind = CV_CODE;
        memcpy(v->code, c->code, sizeof v->code);
        if (c->rx_busy) snprintf(v->note, sizeof v->note, "BUSY  RETRYING");   /* 1.0.3: the server's 429: still waiting, code kept */
        else if (c->chain_wait) snprintf(v->note, sizeof v->note, "BACKED  UP:  WAITING");   /* 1.0.8: the install comes on this code */
        const int32_t left = (int32_t)(c->code_until - c->now);
        v->mins = c->rx_sent ? 0 : left <= 0 ? 0 : (unsigned)((left + 59999) / 60000);
        return;
    }
    case CI_RX_EXPIRED: v->kind = CV_EXPIRED; return;
    case CI_RX_FAIL: v->kind = CV_FAIL; snprintf(v->note, sizeof v->note, "%s", c->fail_rows ? c->fail_rows : ""); return;
    case CI_RX_PAUSED:
        v->kind = CV_UNFINISHED;
        snprintf(v->title, sizeof v->title, "%s", c->unf_title);
        snprintf(v->note, sizeof v->note, "TRYING  AGAIN  SOON");
        return;
    case CI_PEEK: v->kind = CV_ARRIVED; snprintf(v->title, sizeof v->title, "%s", c->offer.title); return;
    case CI_WAIT_POWER:
        v->kind = !c->anyway && c->h.power_ok && !c->h.power_ok(c->h.user) ? CV_BATTERY : CV_INSTALLING;
        v->anyway = c->anyway;
        snprintf(v->title, sizeof v->title, "%s", c->offer.title);
        return;
    case CI_ACKDL: case CI_ARMED: v->kind = CV_INSTALLING; v->anyway = c->anyway; v->backup = c->offer.kind == CF_KIND_BACKUP; snprintf(v->title, sizeof v->title, "%s", c->offer.title); return;
    default: break;   /* idle, cancelling, reporting */
    }
    if (c->res_shown) { v->kind = CV_RESULT; snprintf(v->title, sizeof v->title, "%s", c->res_title); snprintf(v->note, sizeof v->note, "%s", c->res_note); return; }
    if (c->unfinished) { v->kind = CV_UNFINISHED; snprintf(v->title, sizeof v->title, "%s", c->unf_title); return; }
    v->kind = CV_MENU;
}
