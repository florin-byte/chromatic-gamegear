/* cf_sup.c: see cf_sup.h (SAVE TO MY GAMES). Portable C: no ESP-IDF here, the unit's parts are the hooks. */
#include <stdio.h>
#include <string.h>

#include "cf_sup.h"

/* crc32 (IEEE, as zlib / the server): the library's own (cf_esp.c cf_crc32) on the unit and in the host test */
extern uint32_t cf_crc32(uint32_t crc, const uint8_t *b, uint32_t n);

static int hex_url(char *out, size_t n, const char *t)
{
    size_t k = 0;
    for (; *t; t++) {
        const unsigned char c = (unsigned char)*t;
        const int plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
        if (plain) { if (k + 1 >= n) return -1; out[k++] = (char)c; }
        else { if (k + 3 >= n) return -1; k += (size_t)snprintf(out + k, n - k, "%%%02X", c); }
    }
    out[k] = 0;
    return 0;
}

int cf_sup_target(char *out, size_t n, uint32_t rom_crc, uint32_t rom_size, uint32_t save_crc, const char *title)
{
    char t[CF_SUP_TITLE * 3 + 1];
    if (hex_url(t, sizeof t, title ? title : "")) return -1;
    const int w = snprintf(out, n, "/link_device/my_games/save/upload?rcrc=%08lx&rsz=%lu&scrc=%08lx&t=%s",
                           (unsigned long)rom_crc, (unsigned long)rom_size, (unsigned long)save_crc, t);
    return w > 0 && (size_t)w < n ? 0 : -1;
}

void cf_sup_begin(cf_sup_t *s, const cf_sup_hooks_t *h, uint32_t rom_crc, uint32_t rom_size, uint32_t size, uint32_t want_crc, const char *title)
{
    memset(s, 0, sizeof *s);
    s->h = *h;
    s->rom_crc = rom_crc; s->rom_size = rom_size; s->size = size; s->want_crc = want_crc;
    snprintf(s->title, sizeof s->title, "%s", title ? title : "");
    s->st = CF_SUP_ST_CHECK;
}

static int finish(cf_sup_t *s, int res)
{
    if (s->h.close) s->h.close(s->h.u);
    s->res = res;
    s->st = CF_SUP_ST_DONE;
    s->pct = res == CF_SUP_OK || res == CF_SUP_ALREADY ? 100 : s->pct;
    return 0;
}

int cf_sup_outcome(int http, const char *body)
{
    const char *b = body ? body : "";
    if (http == 200) return strstr(b, "\"dup\":true") ? CF_SUP_ALREADY : CF_SUP_OK;
    if (http == 401 || http == 403) return CF_SUP_E_AUTH;
    if (http == 404 || http == 405) return CF_SUP_E_OLD;
    if (http == 409) {
        if (strstr(b, "\"code\":\"saves_full\"")) return CF_SUP_E_FULL8;
        if (strstr(b, "\"code\":\"library_full\"")) return CF_SUP_E_LIBFULL;
        return CF_SUP_E_SERVER;
    }
    if (http == 413) return CF_SUP_E_BIG;
    if (http == 429) return CF_SUP_E_RATE;
    if (http == 400 || http == 422) return CF_SUP_E_BAD;
    return CF_SUP_E_SERVER;
}

/* Every row at most 71 px (check_words.py measures them in the font); no FPGA / flash / library / PSRAM / staging / SGZ /
   compressed / kept games / RAM cart / version words; no em dashes; two spaces between words. */
const char *cf_sup_words(int res)
{
    switch (res) {
    case CF_SUP_OK:        return "SAVED|IT  IS  IN  MY  GAMES|ON  YOUR  ACCOUNT";
    case CF_SUP_ALREADY:   return "ALREADY  SAVED|THIS  SAVE  IS|IN  MY  GAMES";
    case CF_SUP_E_NOSAVE:  return "NOT  SAVED|THIS  GAME  HAS  NO|SAVE  YET";
    case CF_SUP_E_READ:    return "NOT  SAVED|COULD  NOT  READ|THE  SAVE";
    case CF_SUP_E_NET:     return "NOT  SAVED|NO  CONNECTION|CHECK  THE  WI-FI";
    case CF_SUP_E_MEM:     return "NOT  SAVED|NOT  ENOUGH  MEMORY|TRY  AGAIN";
    case CF_SUP_E_AUTH:
    case CF_SUP_E_PAIR:    return "NOT  SAVED|PAIR  FIRST  IN|SYSTEM  >  ONLINE";
    case CF_SUP_E_FULL8:   return "NOT  SAVED|8  SAVES  KEPT.|DELETE  ONE  ONLINE";
    case CF_SUP_E_LIBFULL: return "NOT  SAVED|MY  GAMES  IS  FULL|DELETE  A  GAME";
    case CF_SUP_E_BIG:     return "NOT  SAVED|THIS  SAVE  IS|TOO  BIG";
    case CF_SUP_E_RATE:    return "NOT  SAVED|TOO  MANY  TRIES|WAIT  A  MINUTE";
    case CF_SUP_E_OLD:     return "NOT  SAVED|NOT  AVAILABLE  YET|TRY  LATER";
    case CF_SUP_E_BAD:     return "NOT  SAVED|THE  SAVE  DID  NOT|ARRIVE  WHOLE";
    case CF_SUP_E_STOP:    return "NOT  SAVED|TOOK  TOO  LONG|TRY  AGAIN";
    case CF_SUP_E_BUSY:    return "NOT  SAVED|CHROMATIC  IS  BUSY|TRY  LATER";
    case CF_SUP_E_PLAYING: return "NOT  SAVED|STOP  THE  GAME|FIRST";
    case CF_SUP_E_WIFI:    return "NOT  SAVED|NO  WI-FI  SAVED|SYSTEM  >  NETWORK";
    case CF_SUP_E_SERVER:
    default:               return "NOT  SAVED|SERVER  BUSY|TRY  AGAIN  LATER";
    }
}

int cf_sup_gate(int paired, int wifi_saved, int playing, int save_stored)
{
    if (playing) return CF_SUP_E_PLAYING;
    if (!paired) return CF_SUP_E_PAIR;
    if (!wifi_saved) return CF_SUP_E_WIFI;
    if (!save_stored) return CF_SUP_E_NOSAVE;
    return 0;
}

/* a read of the save in pieces, summing its crc32; false when a read fails */
static int pass(cf_sup_t *s, int send)
{
    for (unsigned k = 0; k < CF_SUP_STEP_MAX && s->off < s->size; k++) {
        const uint32_t n = s->size - s->off < CF_SUP_CHUNK ? s->size - s->off : CF_SUP_CHUNK;
        if (s->h.read(s->h.u, s->off, s->buf, n)) return -1;
        s->crc = cf_crc32(s->crc, s->buf, n);
        if (send && s->h.write(s->h.u, s->buf, n)) return -2;
        s->off += n;
        /* (the check pass is the first third of the bar, the sending the rest) */
        s->pct = send ? 30 + (int)((uint64_t)s->off * 70u / s->size) : (int)((uint64_t)s->off * 30u / s->size);
    }
    return 0;
}

int cf_sup_step(cf_sup_t *s)
{
    if (s->st == CF_SUP_ST_DONE || s->st == CF_SUP_ST_IDLE) return 0;
    if (s->h.stop && s->h.stop(s->h.u)) return finish(s, CF_SUP_E_STOP);
    switch (s->st) {
    case CF_SUP_ST_CHECK: {
        /* the whole save is read once and must hash as the library recorded it, before a byte goes anywhere */
        if (!s->size || s->size > 0x20000u + CF_SUP_FOOTER_MAX) return finish(s, CF_SUP_E_NOSAVE);
        if (pass(s, 0)) return finish(s, CF_SUP_E_READ);
        if (s->off < s->size) return 1;
        if (s->crc != s->want_crc) return finish(s, CF_SUP_E_READ);
        if (cf_sup_target(s->target, sizeof s->target, s->rom_crc, s->rom_size, s->crc, s->title)) return finish(s, CF_SUP_E_SERVER);
        s->off = 0; s->crc = 0; s->t0 = s->h.now(s->h.u);
        s->st = CF_SUP_ST_OPEN;
        return 1;
    }
    case CF_SUP_ST_OPEN: {
        const int r = s->h.open(s->h.u, s->target, s->size);
        if (r == 0) { s->st = CF_SUP_ST_SEND; return 1; }
        s->opens++;
        if (r < 0 && ++s->hard >= 3) return finish(s, CF_SUP_E_NET);   /* (three tries that could not connect) */
        if (s->h.now(s->h.u) - s->t0 > CF_SUP_OPEN_MS) return finish(s, r > 0 ? CF_SUP_E_MEM : CF_SUP_E_NET);
        return 1;                                   /* (asked again on the next step: the caller spaces them) */
    }
    case CF_SUP_ST_SEND: {
        const int r = pass(s, 1);
        if (r == -1) return finish(s, CF_SUP_E_READ);   /* (nothing more is written: the server gets a short body and stores nothing) */
        if (r == -2) return finish(s, CF_SUP_E_NET);
        if (s->off < s->size) return 1;
        /* the bytes that went out hashed as the check pass said: else the connection is dropped with the body unfinished */
        if (s->crc != s->want_crc) return finish(s, CF_SUP_E_READ);
        s->st = CF_SUP_ST_ANSWER;
        return 1;
    }
    case CF_SUP_ST_ANSWER: {
        s->ans[0] = 0;
        if (s->h.answer(s->h.u, &s->http, s->ans, sizeof s->ans)) return finish(s, CF_SUP_E_NET);
        return finish(s, cf_sup_outcome(s->http, s->ans));
    }
    default:
        return 0;
    }
}
