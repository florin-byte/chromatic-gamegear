/* cf_ota.c: see cf_ota.h (Shiny Relay 1.1.0: the update over Wi-Fi). */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cf_ota.h"

static void logf_(cf_ota_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void logf_(cf_ota_t *c, const char *fmt, ...)
{
    if (!c->h.log) return;
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    c->h.log(c->h.user, b);
}

/* ---- a flat JSON object reader (the answer is one small object) */
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
        if (*q == '\\' || n + 1 >= cap) return -1;
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
static int version_ok(const char *v)
{
    if (!v[0] || strlen(v) > 15) return 0;
    for (const char *p = v; *p; p++) if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '-')) return 0;
    return isdigit((unsigned char)v[0]);
}
static int fpga_ver_ok(const char *v)
{
    if (!v[0] || strlen(v) > 7) return 0;
    for (const char *p = v; *p; p++) if (!(isdigit((unsigned char)*p) || *p == '.')) return 0;
    return 1;
}
/* http://host[:port]/gbnet/v1/fw/<sha256>.bin, nothing after it */
static int url_ok(const char *u, const char *sha)
{
    const char *h = !strncmp(u, "http://", 7) ? u + 7 : NULL;
    if (!h || strlen(u) > CF_OTA_URL_MAX) return 0;
    const char *slash = strchr(h, '/');
    if (!slash || slash == h) return 0;
    for (const char *p = h; p < slash; p++) if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '-' || *p == ':')) return 0;
    char want[96];
    snprintf(want, sizeof want, "/gbnet/v1/fw/%s.bin", sha);
    return !strcmp(slash, want);
}

int cf_ota_parse(const char *json, cf_ota_offer_t *o, const char **why)
{
    *why = "";
    memset(o, 0, sizeof *o);
    if (!jtrue(json, "update")) {
        if (jtrue(json, "usb")) {
            char w[16] = "", v[16] = "";
            (void)jstr(json, "why", w, sizeof w);
            if (jstr(json, "version", v, sizeof v) > 0 && version_ok(v)) memcpy(o->version, v, sizeof o->version);
            *why = !strcmp(w, "fpga") ? "fpga" : "layout";
            return 2;
        }
        return 0;
    }
    if (jstr(json, "version", o->version, sizeof o->version) < 1 || !version_ok(o->version)) { *why = "version"; return -1; }
    if (jfind(json, "size")) {                                           /* the app (an FPGA-only update has none) */
        if (jnum(json, "size", &o->size) || o->size < CF_OTA_MIN || o->size > CF_OTA_MAX) { *why = "size"; return -1; }
        if (jstr(json, "sha256", o->sha256, sizeof o->sha256) != 64 || !all_hex(o->sha256, 64)) { *why = "sha256"; return -1; }
        if (jstr(json, "url", o->url, sizeof o->url) < 1 || !url_ok(o->url, o->sha256)) { *why = "url"; return -1; }
    }
    if (jfind(json, "fpga_size")) {
        if (jstr(json, "fpga_version", o->fversion, sizeof o->fversion) < 1 || !fpga_ver_ok(o->fversion)) { *why = "fpga_version"; return -1; }
        if (jnum(json, "fpga_size", &o->fsize) || o->fsize < CF_OTA_FPGA_MIN || o->fsize > CF_OTA_FPGA_MAX) { *why = "fpga_size"; return -1; }
        if (jstr(json, "fpga_sha256", o->fsha256, sizeof o->fsha256) != 64 || !all_hex(o->fsha256, 64)) { *why = "fpga_sha256"; return -1; }
        if (jstr(json, "fpga_url", o->furl, sizeof o->furl) < 1 || !url_ok(o->furl, o->fsha256)) { *why = "fpga_url"; return -1; }
    }
    if (!o->size && !o->fsize) { *why = "empty"; return -1; }
    return 1;
}

static int due(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }
static int take_btn(cf_ota_t *c) { const int b = c->btn; c->btn = -1; return b; }

static void hold(cf_ota_t *c, int on)
{
    if (!!c->rx == !!on) return;
    c->rx = on;
    if (c->h.hold) c->h.hold(c->h.user, on);
}
static void io_free(cf_ota_t *c) { if (c->buf) { memset(c->buf, 0, CF_OTA_BUF); free(c->buf); c->buf = NULL; } }
static int api(cf_ota_t *c, const char *path, const char *body)
{
    if (!c->buf && !(c->buf = (char *)malloc(CF_OTA_BUF))) return 0;
    c->buf[0] = 0;
    if (!c->h.api_start(c->h.user, path, body, c->buf, CF_OTA_BUF)) return 0;
    c->t0 = c->now;
    return 1;
}
static int api_done(cf_ota_t *c, int *code)
{
    const int r = c->h.api_result(c->h.user);
    if (r == 0 && !due(c->now, c->t0 + CF_OTA_API_MS)) return 0;
    *code = r == 0 ? -1 : r;
    return 1;
}
static void result(cf_ota_t *c, const char *title, const char *rows)
{
    hold(c, 0);
    io_free(c);
    c->res_title = title;
    c->res_rows = rows;
    c->st = OT_RESULT;
    logf_(c, "cf: update: %s (%s)", title, rows);
}

void cf_ota_init(cf_ota_t *c, const cf_ota_host_t *h, uint32_t now)
{
    memset(c, 0, sizeof *c);
    c->h = *h;
    c->now = now;
    c->btn = -1;
}
void cf_ota_button(cf_ota_t *c, int a) { c->btn = a ? 1 : 0; }
int cf_ota_busy(const cf_ota_t *c) { return !(c->st == OT_IDLE || c->st == OT_READY || c->st == OT_RESULT); }

void cf_ota_notice(cf_ota_t *c, int what, const char *version)
{
    snprintf(c->res_ver, sizeof c->res_ver, "%s", version ? version : "");
    c->res_title = what == 2 ? "UPDATE  DID  NOT  START" : "UPDATED";
    c->res_rows = what == 1 ? "SHINY  RELAY  IS|UP  TO  DATE" : what == 3 ? "TURN  IT  OFF  AND|ON  TO  FINISH" : "THE  OLD  VERSION|IS  BACK";
    c->st = OT_RESULT;
}

static void start(cf_ota_t *c)
{
    if (c->h.ready && c->h.ready(c->h.user, &c->offer)) { c->st = OT_READY; return; }   /* one is waiting already */
    if (c->h.slots && !c->h.slots(c->h.user)) { result(c, "UPDATE  OVER  USB", "THIS  CHROMATIC|NEEDS  IT  ONCE"); return; }
    if (c->h.busy && c->h.busy(c->h.user)) { result(c, "NOT  NOW", "A  CART  JOB  IS|RUNNING"); return; }
    c->n_checks++;
    c->reg_tries = 0;
    hold(c, 1);
    c->st = OT_HOLD;
    c->t0 = c->now;
    logf_(c, "cf: update: checking (the link stops meanwhile)");
}

/* a call the device API could not start yet (its heap, a call still closing): back to OT_HOLD, which asks again each
   turn, until CF_OTA_API_MS after the hold began */
static void not_yet(cf_ota_t *c)
{
    if (due(c->now, c->t0 + CF_OTA_API_MS)) { result(c, "NO  CONNECTION", "NO  CONNECTION|TRY  AGAIN"); return; }
    c->st = OT_HOLD;
}
static void ask(cf_ota_t *c)
{
    if (c->h.registered && !c->h.registered(c->h.user)) {
        const uint32_t t = c->t0;
        if (api(c, "/link_device/register", "{}")) c->st = OT_REG;
        else { c->t0 = t; not_yet(c); }
        return;
    }
    char ver[16] = "", fpga[8] = "", pt[65] = "", bl[65] = "", fl[8] = "", body[240];
    if (c->h.me) c->h.me(c->h.user, ver, fpga, pt, bl, fl);
    snprintf(body, sizeof body, "{\"version\":\"%s\",\"fpga\":\"%s\",\"pt\":\"%s\",\"bl\":\"%s\",\"fl\":\"%s\"}", ver, fpga, pt, bl, fl);
    const uint32_t t = c->t0;
    if (api(c, "/link_device/fw/check", body)) c->st = OT_CHECK;
    else { c->t0 = t; not_yet(c); }
}

static const char *call_rows(int code)
{
    if (code < 0) return "NO  CONNECTION|TRY  AGAIN";
    if (code == 429) return "TOO  MANY  TRIES|WAIT  A  MINUTE";
    if (code == 404) return "NOT  AVAILABLE|YET";
    return "SERVER  BUSY|TRY  AGAIN  LATER";
}

void cf_ota_tick(cf_ota_t *c, uint32_t now)
{
    c->now = now;
    const int b = take_btn(c);
    int code;
    switch (c->st) {
    case OT_IDLE:
        if (b == 1) start(c);
        return;
    case OT_RESULT:
        if (b == 0 || b == 1) { c->st = OT_IDLE; c->res_title = NULL; }
        return;
    case OT_HOLD: {
        if (b == 0) { result(c, "CANCELLED", "NOTHING  CHANGED|"); return; }
        const int held = !c->h.held || c->h.held(c->h.user);
        if (!held && !due(now, c->t0 + CF_OTA_HOLD_MS)) return;
        const int w = c->h.wifi ? c->h.wifi(c->h.user) : 2;
        if (w == 0) { result(c, "NO  WI-FI", "SET  UP  WI-FI  FIRST"); return; }
        if (w == 1) { if (due(now, c->t0 + CF_OTA_WIFI_MS)) result(c, "NO  WI-FI", "CHECK  THE  NETWORK"); return; }
        ask(c);
        return;
    }
    case OT_REG:
        if (!api_done(c, &code)) return;
        if (code == 200 && (!c->h.registered || c->h.registered(c->h.user))) { c->t0 = now; ask(c); return; }
        result(c, "NO  CONNECTION", call_rows(code));
        return;
    case OT_CHECK: {
        if (!api_done(c, &code)) return;
        if (code == 401 && c->reg_tries == 0 && c->h.reg_forget) { c->reg_tries = 1; c->h.reg_forget(c->h.user); c->t0 = now; ask(c); return; }
        if (code != 200) { result(c, "CHECK  FAILED", call_rows(code)); return; }
        const char *why;
        const int r = cf_ota_parse(c->buf, &c->offer, &why);
        io_free(c);
        if (r == 0) { result(c, "UP  TO  DATE", "SHINY  RELAY  HAS|NO  UPDATE  NOW"); return; }
        if (r == 2) {
            result(c, "UPDATE  OVER  USB", !strcmp(why, "fpga") ? "IT  NEEDS  A  NEW|CONSOLE  CHIP" : "IT  CHANGES  THE|FLASH  LAYOUT");
            snprintf(c->res_ver, sizeof c->res_ver, "%s", c->offer.version);
            return;
        }
        if (r < 0) { logf_(c, "cf: update: an answer the unit does not take (%s)", why); result(c, "CHECK  FAILED", "SERVER  ERROR|TRY  AGAIN  LATER"); return; }
        if (c->h.busy && c->h.busy(c->h.user)) { result(c, "NOT  NOW", "A  CART  JOB  IS|RUNNING"); return; }
        if (c->h.dl_start(c->h.user, &c->offer)) { result(c, "DOWNLOAD  FAILED", "NOT  ENOUGH  MEMORY|RESTART,  TRY  AGAIN"); return; }
        c->n_dl++;
        c->pct = 0;
        c->st = OT_DL;
        logf_(c, "cf: update: %s (app %lu bytes, FPGA %s %lu bytes) is downloading", c->offer.version, (unsigned long)c->offer.size,
              c->offer.fsize ? c->offer.fversion : "-", (unsigned long)c->offer.fsize);
        return;
    }
    case OT_DL: {
        if (b == 0) { c->h.dl_cancel(c->h.user); result(c, "CANCELLED", "NOTHING  CHANGED|"); return; }
        int pct = 0;
        const int r = c->h.dl_result(c->h.user, &pct);
        if (r < 0) { c->pct = pct; return; }
        hold(c, 0);
        if (r == 0) { c->n_ok++; c->st = OT_READY; logf_(c, "cf: update: %s is ready (A installs it)", c->offer.version); return; }
        c->n_fail++;
        result(c, "DOWNLOAD  FAILED", r == CF_OTA_E_HASH || r == CF_OTA_E_IMAGE ? "IT  DID  NOT|CHECK  OUT" : r == CF_OTA_E_GONE ? "NOT  AVAILABLE|ANY  MORE" : "NO  CONNECTION|TRY  AGAIN");
        return;
    }
    case OT_READY:
        if (b == 0) { c->st = OT_IDLE; return; }          /* LATER: it stays in the slot */
        if (b == 1) {
            if (c->h.busy && c->h.busy(c->h.user)) { result(c, "NOT  NOW", "A  CART  JOB  IS|RUNNING"); return; }
            if (c->h.ready && !c->h.ready(c->h.user, &c->offer)) { result(c, "UPDATE  GONE", "CHECK  AGAIN|"); return; }
            c->st = OT_SWITCH;
            logf_(c, "cf: update: installing %s: the unit restarts", c->offer.version);
        }
        return;
    case OT_SWITCH: {
        const int r = c->h.install(c->h.user, &c->offer);   /* with an app: restarts; a return is a failure */
        if (r == CF_OTA_PENDING) return;                    /* the FPGA header is being written: asked again */
        if (c->h.ready_forget) c->h.ready_forget(c->h.user);
        if (!c->offer.size && r == 0) {
            snprintf(c->res_ver, sizeof c->res_ver, "%s", c->offer.version);
            result(c, "UPDATED", "TURN  IT  OFF  AND|ON  TO  FINISH");
            return;
        }
        result(c, "UPDATE  FAILED", "IT  DID  NOT|CHECK  OUT");
        return;
    }
    default:
        c->st = OT_IDLE;
        return;
    }
}

void cf_ota_view(const cf_ota_t *c, cf_ota_view_t *v)
{
    memset(v, 0, sizeof *v);
    switch (c->st) {
    case OT_HOLD: case OT_REG: case OT_CHECK:
        v->kind = OV_BUSY;
        snprintf(v->title, sizeof v->title, "CHECKING");
        return;
    case OT_DL:
        v->kind = OV_DL;
        snprintf(v->version, sizeof v->version, "%s", c->offer.version);
        v->pct = c->pct;
        return;
    case OT_READY:
        v->kind = OV_READY;
        snprintf(v->version, sizeof v->version, "%s", c->offer.version);
        return;
    case OT_SWITCH:
        if (!c->offer.size) { v->kind = OV_BUSY; snprintf(v->title, sizeof v->title, "INSTALLING"); return; }   /* the FPGA alone: no restart */
        v->kind = OV_SWITCH;
        return;
    case OT_RESULT:
        v->kind = OV_RESULT;
        snprintf(v->title, sizeof v->title, "%s", c->res_title ? c->res_title : "");
        snprintf(v->note, sizeof v->note, "%s", c->res_rows ? c->res_rows : "");
        snprintf(v->version, sizeof v->version, "%s", c->res_ver);
        return;
    default:
        v->kind = OV_MENU;
        return;
    }
}
