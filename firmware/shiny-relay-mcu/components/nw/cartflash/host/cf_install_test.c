/* cf_install_test.c: cf_install.c (the streamed Wi-Fi install, owner rulings 2026-10-11; receive
 * mode + code, lane wifirecv1) against a mock device API, a mock header peek, a mock link hold, a
 * mock OSD page and a mock stream job. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "../cf_install.h"
#include "../cf_proto.h"

static const char *BASE = "https://api.shinyrelay.com/gbnet/v1";
#define KEYHEX "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
#define IVHEX  "f0f1f2f3f4f5f6f7f8f9fafbfffffffe"
#define BLOB   "http://dl.shinyrelay.com/gbnet/v1/install/0123456789abcdef0123456789abcdef/blob/00112233445566778899aabbccddeeff"
#define OFFER_FIELDS \
    " \"pending\": true, \"id\": \"0123456789abcdef0123456789abcdef\", \"game_state_id\": \"x\"," \
    " \"title\": \"Shiny Monst\", \"url\": \"install/0123456789abcdef0123456789abcdef/rom\", \"size\": 262144," \
    " \"crc32\": \"501a1d4b\", \"sha256\": \"2d48b2156992f3fbc71161d349c386cd866fed7f3b94499de3bae4c415781511\"," \
    " \"enc_alg\": \"AES-256-CTR\", \"enc_key\": \"" KEYHEX "\", \"enc_iv\": \"" IVHEX "\"," \
    " \"blob_url\": \"" BLOB "\", \"state\": \"pending\", \"expires_in\": 86000}"
static const char *OFFER = "{" OFFER_FIELDS;
static const char *SENT = "{\"session\": \"sent\"," OFFER_FIELDS;
static const char *WAITING = "{\"session\": \"waiting\", \"expires_in\": 590}";
static const char *EXPIRED = "{\"session\": \"expired\"}";
#define CODE "KX7M2Q"
static const char *START_OK = "{\"code\": \"" CODE "\", \"expires_in\": 600, \"poll_ms\": 4000}";

/* the mock server */
static char poll_answer[900], start_answer[200], resume_answer[900];
static int start_code, poll_code, ack_code, reg_code, resume_code, start_401s;
static int api_busy, api_pending_code, api_delay, api_hang, closing_calls, closing_after_peek;
static char last_start_body[160];
static char side_answer[700] = "SGC1|0|2|8\nkart|262144|SHINY KART COLOR|RACE.\n", last_side_body[200];   /* lane ui110dev */
static int side_code = 200, n_side, side_during_poll;
static char acks[8][200], last_poll_body[80], last_cancel_body[80], last_resume_body[80];
static int n_acks, n_polls, n_starts, n_regs, n_cancels, n_resumes, n_calls;
static char calls_seen[40][48];
/* the unit */
static int registered, reg_forgets, wifi_state, hold_on, n_hold_on, n_hold_off, held_ret, fpga_ok, n_open;
static int peek_res = 0, peek_running, peek_delay, peek_ticks = 4, n_peek;
static char peek_title[17] = "FOREIGN ROM", peek_note[24] = "";
static int power = 1, armed, job = -1, arm_ret = 0, pend = 0, cancelled, ack_dl_before_arm;
static char pend_id[33], pend_title[17], logbuf[16384];
/* dst = src with the first `from` replaced by `to` */
static void subst(char *dst, size_t cap, const char *src, const char *from, const char *to)
{
    const char *p = strstr(src, from);
    if (!p) { snprintf(dst, cap, "%s", src); return; }
    snprintf(dst, cap, "%.*s%s%s", (int)(p - src), src, to, p + strlen(from));
}
static int ok_n, bad_n;

static const char *last_ack(void) { return n_acks ? acks[(n_acks - 1) % 8] : ""; }
static int f_api_start(void *u, const char *path, const char *body, char *out, int cap)
{
    (void)u;
    if (api_busy) return 0;
    if (closing_calls > 0) { closing_calls--; return 0; }   /* the header read's connection still closing */
    snprintf(calls_seen[n_calls % 40], sizeof calls_seen[0], "%s", path); n_calls++;
    api_delay = 2;
    if (!strcmp(path, "/link_device/register")) { n_regs++; snprintf(out, cap, "{}"); api_pending_code = reg_code; if (reg_code == 200) registered = 1; }
    else if (!strcmp(path, "/link_device/recv/start")) {
        n_starts++; snprintf(last_start_body, sizeof last_start_body, "%s", body);
        if (start_401s > 0) { start_401s--; snprintf(out, cap, "{}"); api_pending_code = 401; }
        else { snprintf(out, cap, "%s", start_answer); api_pending_code = start_code; }
    }
    else if (!strcmp(path, "/link_device/recv/poll")) { n_polls++; snprintf(last_poll_body, sizeof last_poll_body, "%s", body); snprintf(out, cap, "%s", poll_answer); api_pending_code = poll_code; }
    else if (!strcmp(path, "/link_device/recv/cancel")) { n_cancels++; snprintf(last_cancel_body, sizeof last_cancel_body, "%s", body); snprintf(out, cap, "{}"); api_pending_code = 200; }
    else if (!strcmp(path, "/link_device/recv/resume")) { n_resumes++; snprintf(last_resume_body, sizeof last_resume_body, "%s", body); snprintf(out, cap, "%s", resume_answer); api_pending_code = resume_code; }
    else if (!strcmp(path, "/link_device/cart_install/ack")) {
        snprintf(acks[n_acks % 8], sizeof acks[0], "%s", body); n_acks++;
        if (strstr(body, "downloading") && !armed) ack_dl_before_arm = 1;
        snprintf(out, cap, "{}"); api_pending_code = ack_code;
    } else if (!strcmp(path, "/link_device/catalog") || !strcmp(path, "/link_device/catalog/order")) {
        /* lane ui110dev: GET GAMES's calls */
        n_side++; snprintf(last_side_body, sizeof last_side_body, "%s", body); side_during_poll |= api_pending_code != 0;
        snprintf(out, cap, "%s", side_answer); api_pending_code = side_code;
    } else { printf("  unexpected call %s\n", path); bad_n++; snprintf(out, cap, "{}"); api_pending_code = 404; }
    if (api_hang) api_pending_code = 0;   /* never answers */
    return 1;
}
static int f_api_result(void *u) { (void)u; if (!api_pending_code) return 0; if (api_delay > 0) { api_delay--; return 0; } const int c = api_pending_code; api_pending_code = 0; return c; }
static const char *f_base(void *u) { (void)u; return BASE; }
static int f_registered(void *u) { (void)u; return registered; }
static void f_reg_forget(void *u) { (void)u; registered = 0; reg_forgets++; }
static int f_fpga(void *u) { (void)u; return fpga_ok; }
static int f_wifi(void *u) { (void)u; return wifi_state; }
static void f_hold(void *u, int on) { (void)u; hold_on = on; if (on) n_hold_on++; else n_hold_off++; }
static int f_held(void *u) { (void)u; return held_ret; }
static int f_peek_start(void *u, const cf_offer_t *o) { (void)u; (void)o; peek_running = 1; peek_delay = peek_ticks; n_peek++; return 1; }
static int f_peek_result(void *u, char *t, char *n) { (void)u; if (!peek_running) return -1; if (peek_delay-- > 0) return -1; peek_running = 0; if (peek_res) return peek_res; closing_calls = closing_after_peek; snprintf(t, 17, "%s", peek_title); snprintf(n, 24, "%s", peek_note); return 0; }
static int f_power(void *u) { (void)u; return power; }
static void f_open(void *u) { (void)u; n_open++; }
static int arm_waits, arm_once, n_arm_calls;   /* wifispeed1: CF_ARM_WAIT this often (the download), then arm_once once */
static int f_arm(void *u, const cf_offer_t *o) { (void)u; n_arm_calls++;
    if (arm_waits > 0) { arm_waits--; return CF_ARM_WAIT; }
    if (arm_once) { const int r = arm_once; arm_once = 0; return r; }
    if (arm_ret == 0) { armed++; snprintf(pend_id, sizeof pend_id, "%s", o->id); snprintf(pend_title, sizeof pend_title, "%s", o->title); } return arm_ret; }
static int f_job(void *u) { (void)u; return job; }
static int f_pending(void *u, char *id, char *t) { (void)u; if (!pend) return 0; if (id) memcpy(id, pend_id, 33); if (t) memcpy(t, pend_title, 17); return 1; }
static void f_cancel(void *u) { (void)u; cancelled++; pend = 0; }
static void f_log(void *u, const char *m) { (void)u; strncat(logbuf, m, sizeof logbuf - strlen(logbuf) - 2); strcat(logbuf, "\n"); if (getenv("V")) printf("    %s\n", m); }
static int f_busy(void *u) { (void)u; return api_pending_code != 0; }
static int anyway_on, n_anyway_on, n_anyway_off;   /* 1.0.5: the host's INSTALL ANYWAY override */
static void f_anyway(void *u, int on) { (void)u; anyway_on = on; if (on) n_anyway_on++; else n_anyway_off++; }
static int n_bk_stop;                               /* 1.0.6: B during a backup */
static void f_bk_stop(void *u) { (void)u; n_bk_stop++; }
static const cf_inst_host_t H = { NULL, f_api_start, f_api_result, f_base, f_registered, f_reg_forget, f_fpga, f_wifi, f_hold, f_held,
                                  f_peek_start, f_peek_result, f_power, f_open, f_arm, f_job, f_pending, f_cancel, f_log, f_busy, f_anyway, f_bk_stop };

static void check(int c, const char *what) { if (c) ok_n++; else { bad_n++; printf("  FAIL %s\n", what); } }

static cf_inst_t C;
static uint32_t T;
static void run(uint32_t ms) { for (uint32_t e = 0; e < ms; e += 100) { T += 100; cf_install_tick(&C, T); } }
static void press(int a) { cf_install_button(&C, a); run(100); }
static cf_view_t view(void) { cf_view_t v; cf_install_view(&C, &v); return v; }
static int key_wiped(void) { int z = 1; for (int i = 0; i < 32; i++) z &= C.offer.key[i] == 0; for (int i = 0; i < 16; i++) z &= C.offer.iv[i] == 0; return z && C.offer.blob_url[0] == 0; }
static void reset(void)
{
    snprintf(poll_answer, sizeof poll_answer, "%s", SENT);
    snprintf(start_answer, sizeof start_answer, "%s", START_OK);
    snprintf(resume_answer, sizeof resume_answer, "%s", SENT);
    start_code = poll_code = ack_code = reg_code = resume_code = 200; start_401s = 0;
    api_busy = 0; api_pending_code = 0; api_hang = 0; closing_calls = closing_after_peek = 0; n_acks = n_polls = n_starts = n_regs = n_cancels = n_resumes = n_calls = 0;
    memset(acks, 0, sizeof acks); last_poll_body[0] = last_cancel_body[0] = last_resume_body[0] = 0;
    registered = 1; reg_forgets = 0; wifi_state = 2; hold_on = 0; n_hold_on = n_hold_off = 0; held_ret = 1; fpga_ok = 1; n_open = 0;
    peek_res = 0; peek_running = 0; n_peek = 0; peek_ticks = 4; snprintf(peek_title, sizeof peek_title, "FOREIGN ROM"); peek_note[0] = 0;
    arm_waits = arm_once = n_arm_calls = 0;
    power = 1; armed = cancelled = 0; job = -1; arm_ret = 0; pend = 0; ack_dl_before_arm = 0; logbuf[0] = 0;
    anyway_on = n_anyway_on = n_anyway_off = 0; n_bk_stop = 0;
    T = 1000;
    cf_install_init(&C, &H, T);
}
/* RECEIVE GAME with the server answering "waiting" until the test says otherwise */
static void sd_body(void *u, char *b, int n) { (void)u; snprintf(b, (size_t)n, "{\"caps\":\"" CF_RX_CAPS ",sd1\",\"sd\":{\"games\":12,\"free_mb\":29000,\"mb\":29512}}"); }
static void to_code(void) { snprintf(poll_answer, sizeof poll_answer, "%s", WAITING); press(1); run(2000); }
static void game_arrives(void) { snprintf(poll_answer, sizeof poll_answer, "%s", SENT); run(CF_RX_POLL_MS + 2000); }

int main(void)
{
    cf_offer_t o; const char *why;
    char j[900], b2[900];
    printf("parse\n");
    check(cf_install_parse("{\"pending\": false}", BASE, &o, &why) == 0, "nothing pending");
    /* lane mygames1 (caps psave1): a play send WITH its save: size is the whole download, obj_* the game's part, save_* the
       save's; the parts must add up; an answer that filled the buffer is not taken (its last keys were cut) */
    { char b3[1100];
      snprintf(b3, sizeof b3, "{\"kind\": \"play\", \"obj_size\": 229376, \"obj_crc32\": \"0badf00d\", \"save_size\": 32768, \"save_crc32\": \"76de2acd\", %s", OFFER + 1);
      check(cf_install_parse(b3, BASE, &o, &why) == 1 && o.save_size == 32768 && o.obj_size == 229376 && o.obj_crc32 == 0x0badf00du && o.save_crc32 == 0x76de2acdu &&
            CF_OBJ_SIZE(&o) == 229376 && CF_OBJ_CRC(&o) == 0x0badf00du && o.size == 262144, "a play send with its save: the game's part and the save's");
      snprintf(b3, sizeof b3, "{\"kind\": \"play\", \"obj_size\": 229000, \"obj_crc32\": \"0badf00d\", \"save_size\": 32768, \"save_crc32\": \"76de2acd\", %s", OFFER + 1);
      check(cf_install_parse(b3, BASE, &o, &why) == -1 && !strcmp(why, "save_size"), "the parts do not add up: refused");
      snprintf(b3, sizeof b3, "{\"kind\": \"play\", \"obj_size\": 100, \"obj_crc32\": \"0badf00d\", \"save_size\": 262044, \"save_crc32\": \"76de2acd\", %s", OFFER + 1);
      check(cf_install_parse(b3, BASE, &o, &why) == -1, "a save over 128 KiB + 48: refused");
      snprintf(b3, sizeof b3, "{\"kind\": \"play\", %s", OFFER + 1);
      check(cf_install_parse(b3, BASE, &o, &why) == 1 && o.save_size == 0 && CF_OBJ_SIZE(&o) == 262144 && CF_OBJ_CRC(&o) == 0x501a1d4bu, "no save: the object is all of it, as before");
      char *big = malloc(CF_INST_BUF + 8);
      snprintf(big, CF_INST_BUF + 8, "{\"kind\": \"play\", \"pad\": \"%0*d\", %s", (int)(CF_INST_BUF - strlen(OFFER) - 20), 0, OFFER + 1);
      check(cf_install_parse(big, BASE, &o, &why) == -1 && !strcmp(why, "answer too long"), "an answer that filled the buffer: not taken");
      free(big);
      check(CF_INST_BUF >= 1000, "the answer buffer takes a card send of a packed game with its save (about 880 bytes)"); }
    /* 1.0.5: a patch bundle (kind "patch"): its own size rule; any other kind is not taken */
    { char b2[1000]; snprintf(b2, sizeof b2, "%s", OFFER);
      char *q = strstr(b2, "\"size\": 262144"); memcpy(q, "\"size\":    100", 14);
      check(cf_install_parse(b2, BASE, &o, &why) == -1, "100 bytes as a ROM: refused" ) ; }
    { char b2[1000]; snprintf(b2, sizeof b2, "{\"kind\": \"patch\", %s", OFFER + 1);
      check(cf_install_parse(b2, BASE, &o, &why) == 1 && o.kind == CF_KIND_PATCH && o.size == 262144, "kind patch: a patch offer");
      snprintf(b2, sizeof b2, "{\"kind\": \"patch\", %s", OFFER + 1); char *q = strstr(b2, "\"size\": 262144"); memcpy(q, "\"size\":    100", 14);
      check(cf_install_parse(b2, BASE, &o, &why) == 1 && o.kind == CF_KIND_PATCH && o.size == 100, "a patch of 100 bytes is taken (a ROM of that size is not)");
      snprintf(b2, sizeof b2, "{\"kind\": \"patch\", %s", OFFER + 1); q = strstr(b2, "\"size\": 262144"); memcpy(q, "\"size\": 999999", 14);
      check(cf_install_parse(b2, BASE, &o, &why) == -1, "a patch over 512 KiB: refused");
      snprintf(b2, sizeof b2, "{\"kind\": \"wasm\", %s", OFFER + 1);
      check(cf_install_parse(b2, BASE, &o, &why) == -1 && !strcmp(why, "kind"), "an unknown kind: refused");
      snprintf(b2, sizeof b2, "{\"kind\": \"rom\", %s", OFFER + 1);
      check(cf_install_parse(b2, BASE, &o, &why) == 1 && o.kind == CF_KIND_ROM, "kind rom: a ROM");
      check(cf_install_parse(OFFER, BASE, &o, &why) == 1 && o.kind == CF_KIND_ROM, "no kind: a ROM (servers before 1.0.5)"); }
    check(cf_install_parse(OFFER, BASE, &o, &why) == 1 && o.size == 262144 && o.crc32 == 0x501a1d4b && !strcmp(o.title, "SHINY MONST")
          && !strcmp(o.blob_url, BLOB), "the backend's offer");
    check(o.key[0] == 0x00 && o.key[31] == 0x1f && o.iv[0] == 0xf0 && o.iv[15] == 0xfe, "the key and the first counter block, as bytes");
    snprintf(j, sizeof j, "%s", OFFER);
    { char *p = strstr(j, "\"id\": \"0123456789abcdef0123456789abcdef\""); snprintf(b2, sizeof b2, "%.*s\"id\": \"01234567-89ab-cdef-0123-456789abcdef\"%s", (int)(p - j), j, p + 40);
      check(cf_install_parse(b2, BASE, &o, &why) == 1 && !strcmp(o.id, "0123456789abcdef0123456789abcdef"), "a uuid with dashes is the same id"); }
    /* the wire format: the AES fields are REQUIRED; without them the offer is not taken */
    { const char *drop[] = { "\"enc_alg\"", "\"enc_key\"", "\"enc_iv\"", "\"blob_url\"" };
      for (unsigned i = 0; i < 4; i++) {
          char *p = strstr(j, drop[i]); char *q = strchr(strchr(strchr(p + strlen(drop[i]), '"') + 1, '"') + 1, ',');
          snprintf(b2, sizeof b2, "%.*s%s", (int)(p - j), j, q + 1);
          char what[80]; snprintf(what, sizeof what, "refused without %s (and nothing of it kept)", drop[i]);
          check(cf_install_parse(b2, BASE, &o, &why) == -1 && o.key[31] == 0 && o.blob_url[0] == 0, what);
      } }
    { char k2[900]; snprintf(k2, sizeof k2, "%s", OFFER); char *p = strstr(k2, KEYHEX); for (int i = 0; i < 64; i++) p[i] = (char)toupper((unsigned char)p[i]);
      check(cf_install_parse(k2, BASE, &o, &why) == 1 && o.key[31] == 0x1f, "upper-case hex key taken"); }
    const char *goods[][2] = { { "\"size\": 262144", "\"size\": 200003" }, { "\"size\": 262144", "\"size\": 2031616" }, { "\"size\": 262144", "\"size\": 2097152" },
                               { " \"title\": \"Shiny Monst\",", "" } };
    for (unsigned i = 0; i < sizeof goods / sizeof goods[0]; i++) {
        char *p = strstr(j, goods[i][0]);
        snprintf(b2, sizeof b2, "%.*s%s%s", (int)(p - j), j, goods[i][1], p + strlen(goods[i][0]));
        char what[80]; snprintf(what, sizeof what, "taken: %s", goods[i][1][0] ? goods[i][1] : "(no title)");
        check(cf_install_parse(b2, BASE, &o, &why) == 1, what);
    }
    const char *bads[][2] = {
        { "\"size\": 262144", "\"size\": 16384" }, { "\"size\": 262144", "\"size\": 2097153" },
        { "\"crc32\": \"501a1d4b\"", "\"crc32\": \"501A1D4B\"" }, { "\"sha256\": \"2d48", "\"sha256\": \"zz48" },
        { "\"id\": \"0123", "\"id\": \"x123" },
        { "AES-256-CTR", "AES-128-CTR" }, { "\"enc_iv\": \"f0f1", "\"enc_iv\": \"f0" }, { "\"enc_key\": \"0001", "\"enc_key\": \"zz01" },
        { "http://dl.shinyrelay.com/gbnet/v1/install/0123", "ftp://dl.shinyrelay.com/gbnet/v1/install/0123" },
        { "dl.shinyrelay.com/gbnet/v1/install/0123", "dl.shinyrelay.com/gbnet/v1/install/9123" },
        { "dl.shinyrelay.com/gbnet", "dl.shinyrelay.com/x/gbnet" }, { "dl.shinyrelay.com/", "user@dl.shinyrelay.com/" },
        { "/blob/0011", "/blob/../11" }, { "ccddeeff\"", "ccddeeff?a=b\"" },
    };
    for (unsigned i = 0; i < sizeof bads / sizeof bads[0]; i++) {
        char *p = strstr(j, bads[i][0]);
        snprintf(b2, sizeof b2, "%.*s%s%s", (int)(p - j), j, bads[i][1], p + strlen(bads[i][0]));
        char what[80]; snprintf(what, sizeof what, "refused: %s", bads[i][1]);
        check(cf_install_parse(b2, BASE, &o, &why) == -1, what);
    }


    printf("nothing polls outside receive mode\n"); reset();
    run(3u * 3600u * 1000u);
    check(n_calls == 0 && n_hold_on == 0, "three hours idle: not one device call, no hold");
    check(view().kind == CV_MENU, "SYSTEM > DOWNLOAD shows RECEIVE GAME");
    check(C.buf == NULL, "idle: no answer buffer (no static RAM for it)");

    printf("receive mode: enter, code, poll, a game arrives, install, report\n"); reset();
    snprintf(peek_note, sizeof peek_note, "MAY  NOT  RUN");
    to_code();
    check(n_hold_on == 1 && hold_on == 1, "RECEIVE GAME: the link stops (the hold is on) before any call");
    check(!strcmp(calls_seen[0], "/link_device/recv/start") && n_starts == 1 && n_regs == 0, "a registered unit asks for a code at once");
    check(strstr(last_start_body, "\"caps\":\"patch1,backup1,chain1,save1,play1,z1,keep1,psave1\"") != NULL, "1.0.5 .. 1.1.0 + gamecomp1 + ui110dev + mygames1: recv/start says the unit takes patches, backups, saves, games to play, SGZ1, keep, a play game's save (caps patch1,backup1,chain1,save1,play1,z1,keep1,psave1)");
    { cf_view_t v = view(); check(v.kind == CV_CODE && !strcmp(v.code, CODE) && v.mins == 10, "the code is on the screen, 10 minutes left"); }
    check(strstr(logbuf, CODE) == NULL, "the code is never in the log");
    check(n_polls == 0, "no poll before the server's poll_ms");
    run(CF_RX_POLL_MS * 3 + 500);
    check(n_polls == 3 && strstr(last_poll_body, "\"code\":\"" CODE "\"") != NULL, "polls every 4 s with the code");
    check(view().kind == CV_CODE, "still waiting");
    game_arrives();
    check(n_peek == 1, "a game came: its header is read");
    check(armed == 1 && C.st == CI_ARMED, "installed without a question (the player asked for it)");
    check(ack_dl_before_arm && strstr(acks[0], "\"downloading\""), "'downloading' goes to the server BEFORE the write is armed");
    { cf_view_t v = view(); check(v.kind == CV_INSTALLING && !strcmp(v.title, "SHINY MONST"), "INSTALLING with the server's title, not the ROM's header (1.1.2)"); }
    check(hold_on == 1, "the hold stays while the cart is written");
    { const int p0 = n_polls; run(20000); check(n_polls == p0, "no poll while installing"); }
    job = CF_D_OK; run(1000);
    check(strstr(last_ack(), "\"installed\"") != NULL && C.n_installs == 1, "installed: reported");
    { cf_view_t v = view(); check(v.kind == CV_RESULT && !strcmp(v.title, "INSTALLED"), "INSTALLED on the screen"); }
    check(key_wiped(), "the key, the iv and the blob URL are wiped when the install ends");
    run(3000);
    check(hold_on == 0 && n_hold_off == 1, "the link comes back after the install");
    check(C.st == CI_IDLE && C.rep_left == 0 && C.buf == NULL, "idle, nothing left to report, the buffer given back");
    press(0);
    check(view().kind == CV_MENU && n_starts == 1, "B puts INSTALLED away (no new code)");
    { const int c0 = n_calls; run(3600u * 1000u); check(n_calls == c0, "and nothing polls afterwards"); }

    printf("cartspeed2: two games in a row: A on INSTALLED is NEXT GAME (receive mode at once)\n");
    reset(); to_code(); game_arrives(); run(5000);
    job = CF_D_OK; run(1000);
    { cf_view_t v = view(); check(v.kind == CV_RESULT, "INSTALLED on the screen"); }
    run(3000);
    snprintf(poll_answer, sizeof poll_answer, "%s", WAITING);
    { const int s0 = n_starts; press(1); run(10000); check(n_starts == s0 + 1 && view().kind == CV_CODE, "one A on INSTALLED: a new code on the screen"); if (n_starts != s0 + 1 || view().kind != CV_CODE) printf("    (%d starts, view %d)\n", n_starts - s0, view().kind); }
    reset(); to_code(); game_arrives(); run(5000);
    snprintf(poll_answer, sizeof poll_answer, "%s", WAITING);
    job = CF_D_OK;
    for (int i = 0; i < 2000 && C.st != CI_REPORT; i++) run(1);
    check(C.st == CI_REPORT, "the 'installed' report is going out");
    cf_install_button(&C, 1); run(1);
    { const int s0 = n_starts; run(10000); check(n_starts == s0 + 1 && view().kind == CV_CODE, "A while the 'installed' report goes out: remembered, a new code once it is done"); }

    printf("the header read's connection still closing (unit 2, 2026-10-14)\n"); reset(); closing_after_peek = 30; to_code(); game_arrives(); run(5000);
    check(armed == 1 && ack_dl_before_arm && strstr(acks[0], "\"downloading\""), "'downloading' waits for the connection to close, then the write is armed (not refused)");
    reset(); closing_after_peek = 100000; to_code(); game_arrives(); run(CF_RX_ACK_WAIT_MS + 2000);
    check(armed == 1 && n_acks == 0, "a connection that never closes: 'downloading' skipped after a while, the write still armed");

    printf("cancel\n"); reset(); to_code(); run(CF_RX_POLL_MS + 500);
    press(0); run(1000);
    check(n_cancels == 1 && strstr(last_cancel_body, CODE) != NULL, "B: the code is given back to the server");
    check(hold_on == 0 && view().kind == CV_MENU && C.st == CI_IDLE, "B: the link comes back, RECEIVE GAME again");
    { const int c0 = n_calls; run(600000); check(n_calls == c0, "no more polls"); }
    reset(); snprintf(poll_answer, sizeof poll_answer, "%s", WAITING); press(1); run(100);
    check(C.st == CI_RX_START, "asking for a code ...");
    press(0);
    check(view().kind == CV_MENU, "... B at once: the menu is back");
    run(1000);
    check(n_cancels == 1 && strstr(last_cancel_body, CODE), "the code that came after the B is given back");
    check(hold_on == 0, "1.0.3: the link comes back once the code is given back (the cancel goes over the kept connection)");
    reset(); held_ret = 0; press(1); run(500); press(0); run(1000);
    check(n_calls == 0 && hold_on == 0 && C.st == CI_IDLE, "B while the link is still stopping: no call at all");
    reset(); to_code(); peek_ticks = 200;
    snprintf(poll_answer, sizeof poll_answer, "%s", SENT); run(CF_RX_POLL_MS + 400);
    check(C.st == CI_PEEK, "a game came, its header is being read");
    check(view().kind == CV_ARRIVED, "the screen says a game came");
    press(0); run(1000);
    check(armed == 0 && strstr(last_ack(), "\"declined\"") && key_wiped(), "B before the write: declined, nothing written, the key wiped");
    run(3000);
    check(hold_on == 0 && view().kind == CV_MENU, "and back to the menu");
    reset(); to_code(); { snprintf(poll_answer, sizeof poll_answer, "%s", SENT); run(CF_RX_POLL_MS - 1900); }
    check(C.st == CI_RX_POLL || C.st == CI_RX_WAIT, "(a poll is out)");
    { int guard = 0; while (C.st != CI_RX_POLL && guard++ < 100) run(100); }
    press(0); run(1000);
    check(armed == 0 && strstr(last_ack(), "\"declined\"") != NULL, "B while the poll that brings the game is out: the sender is told 'declined'");

    printf("expiry\n"); reset(); to_code();
    run(9u * 60u * 1000u + 30000u);
    check(view().mins == 1, "one minute left");
    run(60000);
    { cf_view_t v = view(); check(v.kind == CV_EXPIRED && C.n_expired == 1, "CODE EXPIRED after 10 minutes"); }
    run(2000);
    check(hold_on == 0, "the link comes back while the expired screen shows");
    { const int c0 = n_calls; run(600000); check(n_calls == c0, "no polls after the expiry"); }
    press(1); run(2000);
    check(n_starts == 2 && view().kind == CV_CODE && hold_on == 1, "A: a new code (the hold again)");
    press(0); run(1000);
    reset(); to_code(); snprintf(poll_answer, sizeof poll_answer, "%s", EXPIRED); run(CF_RX_POLL_MS + 1000);
    check(view().kind == CV_EXPIRED, "the server says the code is over: CODE EXPIRED");
    press(0); run(500);
    check(view().kind == CV_MENU, "B: back to the menu");

    printf("no Wi-Fi\n"); reset(); wifi_state = 0; press(1); run(2000);
    { cf_view_t v = view(); check(v.kind == CV_FAIL && strstr(v.note, "NO  WI-FI") && n_calls == 0, "no network saved: NO WI-FI, no call"); }
    run(2000);
    check(hold_on == 0, "and the link is back");
    reset(); wifi_state = 1; press(1); run(CF_RX_WIFI_MS - 2000);
    check(view().kind == CV_GETTING && n_calls == 0, "joining the network: GETTING READY, no call yet");
    run(3000);
    check(view().kind == CV_FAIL && strstr(view().note, "CHECK  THE  NETWORK"), "never joined: NO WI-FI / CHECK THE NETWORK");
    wifi_state = 2; press(1); run(2000);
    check(view().kind == CV_CODE, "A on the failure: tried again, the code");
    reset(); start_code = -1; press(1); run(2000);
    check(view().kind == CV_GETTING && n_starts >= 1, "cartspeed2: no connection: asked again (not NO CONNECTION at once)");
    run(5000);
    check(view().kind == CV_FAIL && strstr(view().note, "NO  CONNECTION") && n_starts == 3, "the server cannot be reached: NO CONNECTION after 3 tries");
    reset(); start_code = -1; press(1); run(2500); start_code = 200; run(3000);
    check(view().kind == CV_CODE, "cartspeed2: no connection once, then the code (the heap came back)");
    reset(); to_code(); poll_code = -1; run(CF_RX_POLL_MS * (CF_RX_POLL_FAILS + 1) + 3000);
    check(view().kind == CV_FAIL && strstr(view().note, "NO  CONNECTION"), "the connection goes while waiting: NO CONNECTION after 5 tries");
    reset(); to_code(); poll_code = 503; run(CF_RX_POLL_MS * 2 + 1000); poll_code = 200; snprintf(poll_answer, sizeof poll_answer, "%s", WAITING); run(CF_RX_POLL_MS + 1000);
    check(view().kind == CV_CODE, "a failed poll or two: still waiting");
    reset(); start_code = 429; press(1); run(2000);
    check(strstr(view().note, "TOO  MANY  TRIES") != NULL, "429: TOO MANY TRIES");
    reset(); api_hang = 1; press(1); run(CF_INST_API_MS + 2000);
    check(view().kind == CV_FAIL, "a call that never answers: given up after a minute");

    printf("registration\n"); reset(); registered = 0; to_code();
    check(n_regs == 1 && !strcmp(calls_seen[0], "/link_device/register") && !strcmp(calls_seen[1], "/link_device/recv/start"), "an unregistered unit registers first, then asks for a code");
    check(view().kind == CV_CODE, "... and shows it");
    press(0); run(1000);
    reset(); start_401s = 1; to_code();
    check(reg_forgets == 1 && n_regs == 1 && n_starts == 2 && view().kind == CV_CODE, "the server forgot the registration (401): registered again once");
    press(0); run(1000);
    reset(); start_401s = 5; press(1); run(3000);
    check(view().kind == CV_FAIL && strstr(view().note, "NOT  REGISTERED") && n_regs == 1, "401 again after a new registration: NOT REGISTERED (no loop)");
    reset(); registered = 0; reg_code = 503; press(1); run(2000);
    check(view().kind == CV_FAIL && n_starts == 0, "registration refused: no code asked for");
    reset(); registered = 0; reg_code = 429; press(1); run(2000);
    check(strstr(view().note, "TOO  MANY  TRIES") != NULL, "registration rate-limited: TOO MANY TRIES");

    printf("FPGA rule\n"); reset(); fpga_ok = 0; press(1); run(2000);
    check(view().kind == CV_FAIL && strstr(view().note, "UPDATE") && n_calls == 0 && n_hold_on == 0, "not 18.24+: UPDATE THIS CHROMATIC FIRST, no hold, no call");

    printf("battery\n"); reset(); power = 0; to_code(); game_arrives();
    check(view().kind == CV_BATTERY && armed == 0 && n_acks == 0, "a low battery: BATTERY LOW, nothing written, no question");
    run(60000);
    check(view().kind == CV_BATTERY, "it waits");
    power = 1; run(1000);
    check(armed == 1, "on the charger: it installs by itself");
    reset(); power = 0; to_code(); game_arrives(); run(CF_RX_POWER_MS + 1000);
    check(strstr(last_ack(), "battery low") && view().kind == CV_RESULT && strstr(view().note, "BATTERY"), "no charger for 10 minutes: NOT INSTALLED, BATTERY LOW");
    reset(); power = 0; to_code(); game_arrives(); press(0); run(1000);
    check(strstr(last_ack(), "\"declined\"") != NULL, "B on BATTERY LOW: declined");
    check(armed == 0 && n_anyway_on == 0, "B on BATTERY LOW: nothing written, no override");

    /* 1.0.5 (owner 2026-10-19): A on BATTERY LOW = INSTALL ANYWAY */
    printf("battery: INSTALL ANYWAY\n");
    reset(); power = 0; to_code(); game_arrives();
    check(view().kind == CV_BATTERY && !view().anyway, "low battery: BATTERY LOW (no warning flag yet)");
    run(30000); press(1); run(1000);
    check(armed == 1 && anyway_on == 1 && n_anyway_on == 1, "A on BATTERY LOW: the write is armed, the host's override is on");
    check(strstr(acks[0], "\"downloading\"") != NULL && ack_dl_before_arm, "the sender is told 'downloading' before the write");
    check(view().kind == CV_INSTALLING && view().anyway, "INSTALLING with the low-battery warning");
    check(strstr(logbuf, "INSTALL ANYWAY") != NULL, "logged");
    job = CF_D_OK; run(1000);
    check(strstr(last_ack(), "\"installed\"") && view().kind == CV_RESULT && !strcmp(view().title, "INSTALLED"), "it installs: INSTALLED, reported");
    check(anyway_on == 0 && C.anyway == 0, "the override ends with the install");
    {   /* the next install on the same low battery asks again */
        snprintf(poll_answer, sizeof poll_answer, "%s", WAITING);
        press(1); run(3000);
        snprintf(poll_answer, sizeof poll_answer, "{\"session\": \"sent\", \"pending\": true, \"id\": \"1123456789abcdef0123456789abcdef\", \"title\": \"Two\", \"size\": 262144,"
                 " \"crc32\": \"501a1d4b\", \"sha256\": \"2d48b2156992f3fbc71161d349c386cd866fed7f3b94499de3bae4c415781511\", \"enc_alg\": \"AES-256-CTR\", \"enc_key\": \"" KEYHEX "\","
                 " \"enc_iv\": \"" IVHEX "\", \"blob_url\": \"http://dl.shinyrelay.com/gbnet/v1/install/1123456789abcdef0123456789abcdef/blob/00112233445566778899aabbccddeeff\"}");
        job = -1; run(CF_RX_POLL_MS + 2000);
        check(view().kind == CV_BATTERY && armed == 1, "the next game on the same low battery: BATTERY LOW again (the choice was for one install)");
        press(0); run(2000);
    }
    reset(); power = 0; to_code(); game_arrives(); press(1); run(1000);
    job = CF_D_WPTEST; run(1000);
    check(strstr(last_ack(), "\"failed\"") && anyway_on == 0, "INSTALL ANYWAY, the write fails: reported failed, override off");
    reset(); power = 0; to_code(); game_arrives(); run(60000); power = 1; run(1000);
    check(armed == 1 && n_anyway_on == 0 && view().kind == CV_INSTALLING && !view().anyway, "the charger plugged in on BATTERY LOW: installs by itself, no override, no warning");
    reset(); power = 0; to_code(); game_arrives(); run(CF_RX_POWER_MS - 5000); press(1); run(1000);
    check(armed == 1 && anyway_on == 1, "A just before the 10 minutes run out: installs");
    reset(); power = 0; to_code(); game_arrives(); run(CF_RX_POWER_MS + 1000); press(1); run(1000);
    check(armed == 0 && n_anyway_on == 0 && strstr(acks[(n_acks + 7) % 8], "battery low") , "no answer for 10 minutes: still fails cleanly (battery low); a late A is NEXT GAME, not a write");
    press(0); run(2000);
    reset(); power = 0; to_code(); game_arrives(); press(1); run(1000);
    pend = 1; job = CF_D_PAUSED; run(1000);
    check(C.st == CI_RX_PAUSED && anyway_on == 1, "INSTALL ANYWAY then the link stopped: paused, the automatic resume keeps the choice");
    press(0); run(2000);

    printf("patch: the unit's own refusals\n");
    reset(); arm_once = CF_ARM_WRONG_CART; to_code(); game_arrives(); run(2000);
    check(strstr(last_ack(), "not the cart this patch is for") && view().kind == CV_RESULT && strstr(view().note, "TETRIS") && armed == 0, "a wrong cart: NOT INSTALLED, reported, nothing written");
    reset(); arm_once = CF_ARM_ALREADY; to_code(); game_arrives(); run(2000);
    check(strstr(last_ack(), "already holds") && view().kind == CV_RESULT && armed == 0, "the cart already has it: NOT INSTALLED (THE CART HAS THIS ALREADY)");
    reset(); arm_once = CF_ARM_PATCH_BAD; to_code(); game_arrives(); run(2000);
    check(strstr(last_ack(), "patch did not check out") && view().kind == CV_RESULT && armed == 0, "a patch that does not check out: NOT INSTALLED");
    reset(); arm_waits = 60; to_code(); game_arrives();
    check(view().kind == CV_INSTALLING && armed == 0, "while the unit reads and patches (arm waits): INSTALLING");
    run(8000);
    check(armed == 1, "then the write of the result is armed");
    printf("the header does not arrive\n"); reset(); to_code(); peek_res = 3; game_arrives();
    check(C.st == CI_RX_WAIT || C.st == CI_RX_POLL || C.st == CI_PEEK, "first try failed: back to polling");
    run((CF_RX_POLL_MS + 1500) * CF_RX_PEEK_TRIES);
    check(strstr(last_ack(), "the image did not arrive") && view().kind == CV_RESULT, "after 3 tries: NOT INSTALLED, reported");
    reset(); to_code(); peek_res = 3; game_arrives(); peek_res = 0; run(CF_RX_POLL_MS + 2000);
    check(armed == 1, "it came with the next poll: installed");

    printf("the link stops in the middle: unfinished, it resumes by itself (no poll)\n"); reset(); to_code(); game_arrives();
    job = CF_D_PAUSED; pend = 1; run(1000);
    check(C.unfinished && C.st == CI_RX_PAUSED && !strstr(last_ack(), "failed"), "paused: unfinished, nothing reported as failed");
    { cf_view_t v = view(); check(v.kind == CV_UNFINISHED && strstr(v.note, "SOON"), "INSTALL NOT FINISHED, trying again soon"); }
    check(hold_on == 1 && key_wiped(), "the hold stays; no key kept meanwhile");
    { const int p0 = n_polls; job = -1; run(CF_INST_RETRY_MS + 2000);
      check(n_resumes == 1 && strstr(last_resume_body, "0123456789abcdef0123456789abcdef") && n_polls == p0, "a minute later: the resume route with the install id, not a poll");
      check(armed == 2 && C.st == CI_ARMED && C.n_resumes == 1 && C.offer.key[31] == 0x1f, "it resumes without a question, the key again"); }
    check(strstr(last_ack(), "\"resuming\"") != NULL, "'resuming' is reported first (the server keeps the image longer)");
    job = CF_D_OK; pend = 0; run(1000);
    check(strstr(last_ack(), "\"installed\"") && !C.unfinished, "finished and reported");
    run(2000);
    check(hold_on == 0, "the link comes back");

    printf("three pauses: then it waits for A\n"); reset(); to_code(); game_arrives();
    for (int i = 0; i < CF_INST_MAX_AUTO; i++) { job = CF_D_VERIFY; pend = 1; run(1000); job = -1; run(CF_INST_RETRY_MS + 2000); }
    job = CF_D_VERIFY; run(1000); job = -1;
    { const int a0 = armed; run(CF_INST_RETRY_MS * 3);
      check(armed == a0 && armed == CF_INST_MAX_AUTO + 1 && C.st == CI_IDLE && hold_on == 0, "after 3 tries by itself: no more, the link comes back"); }
    check(view().kind == CV_UNFINISHED, "INSTALL NOT FINISHED (A: RESUME  B: CANCEL)");
    { const int c0 = n_calls; run(3600u * 1000u); check(n_calls == c0, "nothing is called while it waits"); }
    press(1); run(3000);
    check(armed == CF_INST_MAX_AUTO + 2 && hold_on == 1, "A (RESUME): receive mode for it, it starts again");
    job = CF_D_OK; pend = 0; run(3000);
    check(strstr(last_ack(), "\"installed\"") != NULL, "installed");

    printf("power lost in the middle: after the restart\n"); reset();
    pend = 1; snprintf(pend_id, sizeof pend_id, "0123456789abcdef0123456789abcdef"); snprintf(pend_title, sizeof pend_title, "FOREIGN ROM");
    T = 1000; cf_install_init(&C, &H, T);
    run(1000);
    check(C.unfinished && view().kind == CV_UNFINISHED && n_open == 1, "the unfinished install is known; SYSTEM > DOWNLOAD opens for it");
    run(600000);
    check(n_calls == 0 && armed == 0, "it does not resume by itself (nothing is called)");
    press(1); run(3000);
    check(n_resumes == 1 && armed == 1 && n_peek == 0 && n_polls == 0, "A: the resume route, no header read, no question");
    job = CF_D_OK; pend = 0; run(1000);
    reset(); pend = 1; snprintf(pend_id, sizeof pend_id, "0123456789abcdef0123456789abcdef"); snprintf(pend_title, sizeof pend_title, "FOREIGN ROM");
    T = 1000; cf_install_init(&C, &H, T); run(500); snprintf(resume_answer, sizeof resume_answer, "{\"pending\": false}");
    press(1); run(3000);
    check(view().kind == CV_RESULT && strstr(view().note, "EXPIRED") && armed == 0, "the server no longer has it: THE SEND EXPIRED");
    press(1);
    check(view().kind == CV_UNFINISHED, "then INSTALL NOT FINISHED again (B gives it up)");
    press(0); run(1000);
    check(cancelled == 1 && strstr(last_ack(), "cancelled on the unit") && !C.unfinished, "B (CANCEL): the journal forgets it, the server is told");
    check(view().kind == CV_MENU, "then RECEIVE GAME");

    printf("give it up while paused\n"); reset(); to_code(); game_arrives(); job = CF_D_PAUSED; pend = 1; run(1500); job = -1;
    press(0); run(1000);
    check(cancelled == 1 && strstr(last_ack(), "cancelled on the unit") && !C.unfinished, "B on the paused install: forgotten, the server told");
    run(1000);
    check(hold_on == 0, "the link comes back");

    printf("the server no longer has it mid-write\n"); reset(); to_code(); game_arrives(); job = CF_D_GONE; pend = 1; run(1500);
    check(strstr(last_ack(), "the image expired") && C.unfinished, "expired mid-write: reported failed, the cart is unfinished");
    check(view().kind == CV_RESULT && strstr(view().note, "EXPIRED"), "the result first ...");
    press(1);
    check(view().kind == CV_UNFINISHED, "... then INSTALL NOT FINISHED once it is put away");

    printf("how a write can end\n");
    { struct { int code; const char *ack, *rows; } e[] = {
        { 0xEE, "no cart writer on this FPGA", "UPDATE" }, { CF_D_LOCKED, "write-protected", "WRITE  PROTECTED" },
        { CF_D_WPTEST, "write test failed", "NOTHING  ERASED" }, { CF_D_BAD_IMAGE, "does not fit this cart", "NOT  FIT" },
        { CF_D_WRONG_CHIP, "not a cart this Chromatic can write", "CANNOT" }, { CF_D_OVERLAY, "turn patches off", "PATCHES" } };
      for (unsigned i = 0; i < sizeof e / sizeof e[0]; i++) {
          reset(); to_code(); game_arrives(); job = e[i].code; run(1000);
          char w[80]; snprintf(w, sizeof w, "code %d: reported '%s', NOT INSTALLED %s", e[i].code, e[i].ack, e[i].rows);
          cf_view_t v = view();
          check(strstr(last_ack(), e[i].ack) && v.kind == CV_RESULT && !strcmp(v.title, "NOT  INSTALLED") && strstr(v.note, e[i].rows), w);
      } }
    reset(); { char *z; snprintf(poll_answer, sizeof poll_answer, "%s", SENT); z = strstr(poll_answer, "262144,"); memcpy(z, "2097152", 7); }
    press(1); run(CF_RX_POLL_MS + 3000); job = CF_D_BAD_IMAGE; run(1000);
    check(strstr(last_ack(), "protected block not blank") != NULL, "a 2 MiB image on a cart whose protected block is not blank");
    reset(); arm_ret = CF_D_ABORTED; to_code(); game_arrives();
    check(strstr(last_ack(), "refused to arm") && strstr(view().note, "MEMORY"), "refused to arm: NOT ENOUGH MEMORY");
    run(CF_INST_RESULT_MS + 2000);
    check(view().kind == CV_MENU, "the result goes by itself after a while");

    printf("the image downloads into the staging area before the write (wifispeed1)\n"); reset(); arm_waits = 300; to_code(); game_arrives();
    check(armed == 0 && C.st == CI_ACKDL && C.arming, "the game came: downloading, the write not armed yet");
    check(n_acks == 1 && strstr(acks[0], "\"downloading\""), "'downloading' told once, before the download");
    { cf_view_t v = view(); check(v.kind == CV_INSTALLING, "INSTALLING on the screen while it downloads"); }
    { const int p0 = n_polls, a0 = n_acks; run(20000); check(n_polls == p0 && n_acks == a0 && hold_on == 1, "no poll, no report, the hold stays while it downloads"); }
    run(15000);
    check(armed == 1 && C.st == CI_ARMED && !C.arming, "downloaded and hashed: the write is armed");
    job = CF_D_OK; run(1000);
    check(strstr(last_ack(), "\"installed\"") != NULL, "installed, reported");
    reset(); arm_waits = 50; arm_once = CF_ARM_NO_DATA; to_code(); game_arrives(); run(6000);
    check(C.unfinished && C.st == CI_RX_PAUSED && !strstr(last_ack(), "failed") && armed == 0, "the download stopped: paused, NOT reported failed (the server keeps the image)");
    check(key_wiped() && hold_on == 1, "the key wiped meanwhile, the hold stays");
    run(CF_INST_RETRY_MS + 3000);
    check(n_resumes == 1 && armed == 1 && strstr(last_ack(), "\"resuming\""), "a minute later: the resume route, downloaded again, armed");
    job = CF_D_OK; run(1000);
    reset(); arm_waits = 10; arm_once = CF_D_GONE; to_code(); game_arrives(); run(3000);
    check(strstr(last_ack(), "the image expired") && view().kind == CV_RESULT && strstr(view().note, "EXPIRED") && armed == 0, "the server says the image is gone: THE SEND EXPIRED");
    reset(); arm_waits = 30; to_code(); game_arrives(); press(0); run(1000);
    check(C.st == CI_ACKDL || C.st == CI_ARMED, "(no buttons while it downloads: B does nothing)");

    printf("reports\n"); reset(); ack_code = -1; to_code(); game_arrives(); job = CF_D_OK; run(1000);
    /* gamecomp1: a report that cannot start (-1: the heap after a big install) is tried again every 2 s with receive mode kept,
       those quick tries not counted, CF_INST_REPORT_QUICK of them; then as before (a minute) */
    check((C.st == CI_REPORT_WAIT || C.st == CI_REPORT) && hold_on == 1 && C.rep_left == CF_INST_REPORT_TRIES, "the report did not start (-1): quick tries, receive mode kept, no try used");
    check(!cf_install_report_only_waits(&C), "(during the quick tries a game still waits)");
    ack_code = 200; { const int a0 = n_acks; run(5000);
      check(n_acks > a0 && strstr(last_ack(), "\"installed\"") && C.rep_left == 0, "a quick try a few seconds later lands"); }
    reset(); ack_code = -1; to_code(); game_arrives(); job = CF_D_OK; run(2300u * CF_INST_REPORT_QUICK + 5000u);
    check(C.rep_quick == CF_INST_REPORT_QUICK && C.rep_left > 0 && C.rep_left < CF_INST_REPORT_TRIES && C.st == CI_IDLE, "never starting: the quick tries, then one counted try and the minute timer");
    check(cf_install_report_only_waits(&C) && cf_install_busy(&C), "then only the report waits: a game waiting for it may start");
    ack_code = 200; { const int a0 = n_acks; run(CF_INST_RETRY_MS + 3000);
      check(n_acks == a0 + 1 && strstr(last_ack(), "\"installed\"") && C.rep_left == 0, "sent again a minute later, and it landed"); }
    reset(); ack_code = 503; to_code(); game_arrives(); job = CF_D_OK; run(CF_INST_RETRY_MS * (CF_INST_REPORT_TRIES + 2));
    check(C.rep_left == 0 && n_acks == CF_INST_REPORT_TRIES + 1, "a server that never takes it: a bounded number of tries ('downloading' + 6)");
    reset(); to_code(); game_arrives(); job = CF_D_OK; run(2000);
    snprintf(poll_answer, sizeof poll_answer, "%s", SENT); press(1); press(1); run(CF_RX_POLL_MS + 2000);
    check(armed == 1 && view().kind == CV_EXPIRED, "the same install again (its id this boot's last): not written twice");

    printf("the link task busy\n"); reset(); api_busy = 1; press(1); run(10000);
    check(view().kind == CV_GETTING && n_calls == 0, "waits for the link task");
    api_busy = 0; run(2000);
    check(view().kind == CV_CODE, "then the code");

    printf("power rule\n");
    check(cf_power_policy(0, 0.0f, 3.40f, 1), "charging: yes");
    check(!cf_power_policy(0, 3.9f, 3.10f, 0), "no valid reading: no");
    check(!cf_power_policy(1, 3.05f, 3.10f, 0) && !cf_power_policy(1, 3.10f, 3.10f, 0) && cf_power_policy(1, 3.15f, 3.10f, 0) && cf_power_policy(1, 3.30f, 3.10f, 0) && cf_power_policy(1, 3.42f, 3.10f, 0), "LiPo: above the low mark (yellow and green install, red waits)");
    check(!cf_power_policy(1, 2.75f, 2.80f, 0) && cf_power_policy(1, 2.9f, 2.80f, 0) && cf_power_policy(1, 3.20f, 2.80f, 0), "AA: above the low mark");

    /* ---- 1.0.3: the server's 429 is not a failure: backoff, the code kept, BUSY, RETRYING */
    printf("429 backoff\n");
    reset(); to_code();
    check(view().kind == CV_CODE && C.code[0], "a code is on the page");
    {   /* polls refused with the limiter's own wait: retry_after_s 30 (the body), honoured; the code and its page stay */
        poll_code = 429; snprintf(poll_answer, sizeof poll_answer, "{\"error\":\"Too many requests\",\"retry_after_s\":30}");
        const int p0 = n_polls; run(CF_RX_POLL_MS + 300);
        check(n_polls == p0 + 1, "the refused poll went out");
        const int p1 = n_polls; run(25000);
        check(n_polls == p1, "no poll before retry_after_s (30 s)");
        cf_view_t v = view();
        check(v.kind == CV_CODE && v.code[0] && !strcmp(v.note, "BUSY  RETRYING"), "the code stays on the page, with BUSY, RETRYING");
        run(8000);
        check(n_polls == p1 + 1, "the next poll after 30 s (+ jitter under 1 s)");
        check(strstr(logbuf, "TOO  MANY") == NULL && C.st != CI_RX_FAIL, "no TOO MANY TRIES for a refused poll");
    }
    {   /* polls refused without a wait: 4, 8, 16, 32, 60, 60 s (+ < 1 s jitter), never NO CONNECTION; one 200 resets it */
        snprintf(poll_answer, sizeof poll_answer, "{\"error\":\"Too many requests\"}");
        logbuf[0] = 0;
        run(240000);
        /* the waits the machine chose, from its log: 4, 8, 16, 32, 60, 60 s (+ < 1 s jitter) */
        uint32_t w[16]; int nw = 0; const char *q = logbuf;
        while ((q = strstr(q, "again in ")) && nw < 16) { w[nw++] = (uint32_t)strtoul(q + 9, NULL, 10); q += 9; }
        const uint32_t want[] = { 4000, 8000, 16000, 32000, 60000, 60000 };
        int ok = nw >= 6;
        for (int i = 0; i < 6 && i < nw; i++) ok &= w[i] >= want[i] && w[i] <= want[i] + 1000;
        printf("    waits:"); for (int i = 0; i < nw; i++) printf(" %u", (unsigned)w[i]); printf("\n");
        check(ok, "no-wait 429s: 4, 8, 16, 32, 60, 60 s (+ jitter under 1 s)");
        check(C.st == CI_RX_WAIT || C.st == CI_RX_POLL, "still waiting for a game after many refused polls (no NO CONNECTION)");
        poll_code = 200; snprintf(poll_answer, sizeof poll_answer, "%s", WAITING);
        run(61500);
        check(!C.rx_busy && view().note[0] == 0, "one answered poll: BUSY gone");
        const int qn = n_polls; run(CF_RX_POLL_MS + 200);
        check(n_polls == qn + 1, "and the poll interval is back to normal");
    }
    press(0); run(2000);
    {   /* a start refused by the limiter (retry_after_s): asked again after the wait, GETTING READY says BUSY, RETRYING; then a code */
        reset(); start_code = 429; snprintf(start_answer, sizeof start_answer, "{\"error\":\"Too many requests\",\"retry_after_s\":12}");
        snprintf(poll_answer, sizeof poll_answer, "%s", WAITING);
        press(1); run(1500);
        check(view().kind == CV_GETTING && !strcmp(view().title, "BUSY  RETRYING"), "start refused (limiter): BUSY, RETRYING");
        const int s0 = n_starts; run(10000);
        check(n_starts == s0, "not asked again before 12 s");
        start_code = 200; snprintf(start_answer, sizeof start_answer, "%s", START_OK);
        run(4000);
        check(n_starts == s0 + 1 && view().kind == CV_CODE && !view().note[0], "asked again after the wait: a code, no BUSY");
        press(0); run(2000);
    }
    {   /* the handler's own limit (no retry_after_s: too many codes asked for): TOO MANY TRIES as before */
        reset(); start_code = 429; snprintf(start_answer, sizeof start_answer, "{\"error\":\"Too many tries. Wait a few minutes and try again.\"}");
        press(1); run(3000);
        check(view().kind == CV_FAIL && strstr(view().note, "TOO  MANY  TRIES"), "a start refused by the handler's limit: TOO MANY TRIES");
    }
    /* 1.0.6 (owner 2026-10-20): a BACKUP of this cart to the sender's browser */
    {
        char bk[900];
        snprintf(bk, sizeof bk, "{\"session\": \"sent\", \"pending\": true, \"kind\": \"backup\", \"id\": \"fedcba9876543210fedcba9876543210\","
                 " \"title\": \"BACKUP\", \"enc_alg\": \"AES-256-CTR\", \"enc_key\": \"" KEYHEX "\", \"enc_iv\": \"" IVHEX "\","
                 " \"blob_url\": \"http://dl.shinyrelay.com/gbnet/v1/install/fedcba9876543210fedcba9876543210/blob/00112233445566778899aabbccddeeff\"}");
        printf("backup: parse\n");
        check(cf_install_parse(bk, BASE, &o, &why) == 1 && o.kind == CF_KIND_BACKUP && o.size == 0 && !strcmp(o.title, "YOUR  CART") && strstr(o.blob_url, "/install/fedcba98") != NULL && o.key[31] == 0x1f,
              "kind backup: no size or hashes asked for, the key and the upload URL taken");
        { char b3[900]; snprintf(b3, sizeof b3, "%s", bk); char *q = strstr(b3, "\"enc_key\""); memcpy(q + 1, "enc_kex", 7);
          check(cf_install_parse(b3, BASE, &o, &why) == -1, "a backup without its key: refused"); }

        printf("backup: reading ack, no header read, no battery rule, then BACKED UP\n"); reset();
        power = 0; to_code();
        snprintf(poll_answer, sizeof poll_answer, "%s", bk);
        arm_waits = 30; arm_once = CF_ARM_BACKED_UP;
        for (int i = 0; i < 200 && C.st != CI_ACKDL; i++) run(100);
        snprintf(poll_answer, sizeof poll_answer, "%s", WAITING);   /* the server reopens the code at backed_up */
        run(1000);
        check(n_peek == 0, "no header read for a backup");
        check(n_acks >= 1 && strstr(acks[0], "\"reading\"") != NULL, "'reading' goes to the server first");
        { cf_view_t v = view(); check(v.kind == CV_INSTALLING && v.backup == 1 && !strcmp(v.title, "YOUR  CART"), "BACKING UP YOUR CART on the screen (a low battery does not stop a read)"); }
        check(hold_on == 1, "the hold stays while it runs");
        run(5000);
        check(strstr(last_ack(), "\"backed_up\"") != NULL, "the end: backed_up reported");
        { cf_view_t v = view(); check(v.kind == CV_CODE && strstr(v.note, "BACKED") != NULL, "1.0.8: BACKED UP: WAITING with the same code (no result screen)"); }
        check(key_wiped(), "the key and the URL are wiped when it ends");
        check(armed == 0, "nothing was written to the cart");
        press(0); run(3000);
        check(hold_on == 0, "B: the link comes back afterwards");

        printf("backup then install on ONE code (1.0.8)\n"); reset(); to_code();
        { const int s0 = n_starts;
          snprintf(poll_answer, sizeof poll_answer, "%s", bk);
          arm_waits = 5; arm_once = CF_ARM_BACKED_UP;
          for (int i = 0; i < 200 && C.st != CI_ACKDL; i++) run(100);
          snprintf(poll_answer, sizeof poll_answer, "%s", WAITING);
          run(3000);
          check(strstr(last_ack(), "\"backed_up\"") != NULL, "backed_up reported");
          cf_view_t v = view();
          check(v.kind == CV_CODE && !strcmp(v.code, CODE) && strstr(v.note, "BACKED") != NULL, "the SAME code is on the screen again, BACKED UP: WAITING");
          check(hold_on == 1 && C.chain_wait == 1, "receive mode stays on");
          const int p0 = n_polls; run(CF_RX_POLL_MS * 2 + 500);
          check(n_polls > p0 && strstr(last_poll_body, CODE) != NULL && n_starts == s0, "it polls the same code (no new code asked for)");
          game_arrives(); run(5000);
          check(armed == 1 && C.st == CI_ARMED && C.chain_wait == 0, "the game sent on that code installs");
          job = CF_D_OK; run(1000);
          check(view().kind == CV_RESULT && !strcmp(view().title, "INSTALLED"), "INSTALLED"); }
        reset(); to_code(); snprintf(poll_answer, sizeof poll_answer, "%s", bk); arm_waits = 2; arm_once = CF_ARM_BACKED_UP;
        for (int i = 0; i < 200 && C.st != CI_ACKDL; i++) run(100);
        snprintf(poll_answer, sizeof poll_answer, "%s", WAITING);
        run(3000);
        press(0); run(2000);
        check(C.chain_wait == 0 && strstr(last_cancel_body, CODE) != NULL && view().kind == CV_MENU, "B on BACKED UP: WAITING gives the code back and ends receive mode");
        printf("backup: B stops it; the website stops it\n"); reset(); to_code();
        snprintf(poll_answer, sizeof poll_answer, "%s", bk);
        arm_waits = 1000;
        run(CF_RX_POLL_MS + 1000);
        press(0);
        check(n_bk_stop == 1, "B asks the host to stop the read / upload");
        arm_waits = 0; arm_once = CF_ARM_BK_STOPPED; run(2000);
        check(strstr(last_ack(), "\"failed\"") != NULL && strstr(last_ack(), "stopped") != NULL, "reported failed: the backup was stopped");
        { cf_view_t v = view(); check(v.kind == CV_RESULT && !strcmp(v.title, "BACKUP  STOPPED"), "BACKUP STOPPED on the screen"); }
        reset(); to_code(); snprintf(poll_answer, sizeof poll_answer, "%s", bk); arm_waits = 3; arm_once = CF_ARM_BK_READ; run(CF_RX_POLL_MS + 3000);
        { cf_view_t v = view(); check(v.kind == CV_RESULT && !strcmp(v.title, "BACKUP  STOPPED") && strstr(v.note, "READ") != NULL, "the cart read failed: BACKUP STOPPED, THE CART DID NOT READ"); }
        check(strstr(last_ack(), "the cart read failed") != NULL, "and the server is told why");
    }
    /* 1.0.7: the backup's answers as Cloudflare sends them (prod, 2026-10-20: ~970 bytes of headers; 1.0.6 read 512) */
    {
        printf("http answer: long headers, any split, chunked, refusals\n");
        char cf[2048];
        snprintf(cf, sizeof cf, "HTTP/1.1 200 OK\r\nDate: Tue, 20 Oct 2026 18:54:44 GMT\r\nContent-Type: application/octet-stream\r\nContent-Length: 8\r\n"
            "Connection: keep-alive\r\nServer: cloudflare\r\nNel: {\"report_to\":\"cf-nel\",\"success_fraction\":0.0,\"max_age\":604800}\r\n"
            "cache-control: private, no-store, no-transform\r\nx-total-length: 8\r\nvary: origin, access-control-request-method, access-control-request-headers\r\n"
            "vary: origin, access-control-request-method, access-control-request-headers\r\naccess-control-expose-headers: retry-after,x-total-length,content-range,x-gbnet-notify\r\n"
            "Report-To: {\"group\":\"cf-nel\",\"max_age\":604800,\"endpoints\":[{\"url\":\"https://a.nel.cloudflare.com/report/v4?s=xddWPkxKXHV6GhZnUbMqJQ1J7N8O3FTC%%2Bw5hcSuakoW72hOSK3qjpnVQQpG15Zd2JjXXG49SLcZcLHPrFWek2iQJuMMmn9iq7q23jjzuaWAa3X3eoEO7GSrQ4pySGHgdYYi4AmumZb8wjaPvgEmM4g%%3D%%3D\"}]}\r\n"
            "Server-Timing: cfL4;desc=\"?proto=TCP&rtt=41234&min_rtt=38000&rtt_var=4100&sent=48&recv=52&lost=0&retrans=0&sent_bytes=5120&recv_bytes=70144&delivery_rate=1700000&cwnd=254&unsent_bytes=0&cid=7b1c2d3e4f5a6b7c&ts=63&x=0\"\r\n"
            "CF-RAY: a4da17ec89894f7f-BOS\r\nalt-svc: h3=\":443\"; ma=86400\r\n\r\nOK 65536");
        check(strlen(cf) > 1000, "the sample is longer than prod's (970)");
        int all = 1;
        for (size_t cut = 1; cut < strlen(cf); cut++) {
            cf_http_ans_t a; cf_http_ans_init(&a);
            int r = cf_http_ans_feed(&a, cf, (uint32_t)cut);
            if (r == 0) r = cf_http_ans_feed(&a, cf + cut, (uint32_t)(strlen(cf) - cut));
            if (!(r == 1 && a.code == 200 && !strcmp(a.body, "OK 65536") && !a.closing)) { all = 0; printf("    cut %zu: r %d code %d body '%s'\n", cut, r, a.code, a.body); break; }
        }
        check(all, "Cloudflare's 970 bytes of headers, split at every byte: 200, OK 65536");
        { cf_http_ans_t a; cf_http_ans_init(&a); int r = 0; for (size_t i = 0; i < strlen(cf) && r == 0; i++) r = cf_http_ans_feed(&a, cf + i, 1);
          check(r == 1 && !strcmp(a.body, "OK 65536"), "one byte at a time"); }
        { const char *x = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: keep-alive\r\nx-gbnet-reason: no such blob\r\n\r\n";
          cf_http_ans_t a; cf_http_ans_init(&a); check(cf_http_ans_feed(&a, x, (uint32_t)strlen(x)) == 1 && a.code == 404 && !a.body[0], "an empty 404 (stopped)"); }
        { const char *x = "HTTP/1.1 503 Service Unavailable\r\ncontent-length: 10\r\nconnection: close\r\n\r\nWAIT 65536";
          cf_http_ans_t a; cf_http_ans_init(&a); check(cf_http_ans_feed(&a, x, (uint32_t)strlen(x)) == 1 && a.code == 503 && !strcmp(a.body, "WAIT 65536") && a.closing, "503 WAIT, connection close"); }
        { const char *x = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nOK \r\n5\r\n65536\r\n0\r\n\r\n";
          cf_http_ans_t a; cf_http_ans_init(&a); check(cf_http_ans_feed(&a, x, (uint32_t)strlen(x)) == 1 && a.code == 200 && !strcmp(a.body, "OK 65536"), "a chunked body"); }
        { const char *x = "<html>403</html>\r\n\r\n";
          cf_http_ans_t a; cf_http_ans_init(&a); check(cf_http_ans_feed(&a, x, (uint32_t)strlen(x)) == -1, "not HTTP: refused"); }
        { const char *x = "HTTP/1.1 200 OK\r\nContent-Length: 999999\r\n\r\n";
          cf_http_ans_t a; cf_http_ans_init(&a); check(cf_http_ans_feed(&a, x, (uint32_t)strlen(x)) == -1, "a body far too long for an answer: refused"); }
        { char big[12000]; memset(big, 'x', sizeof big); memcpy(big, "HTTP/1.1 200 OK\r\nX: ", 21);
          cf_http_ans_t a; cf_http_ans_init(&a); check(cf_http_ans_feed(&a, big, sizeof big) == -1, "a header line with no end: refused"); }
    }
    /* 1.0.9: a save alone, and a game with its save */
    {
        printf("1.0.9 kinds: save, romsave\n");
        cf_offer_t o2; const char *w2; char b3[1100];
        /* 1.1.0: kind play, up to the RAM cart's 4 MiB (bigger than any cart) */
        char b4[1100];
        snprintf(b3, sizeof b3, "{\"kind\": \"play\", %s", OFFER + 1);
        check(cf_install_parse(b3, BASE, &o2, &w2) == 1 && o2.kind == CF_KIND_PLAY && o2.size == 262144, "kind play taken");
        subst(b4, sizeof b4, b3, "\"size\": 262144", "\"size\": 4194304");
        check(cf_install_parse(b4, BASE, &o2, &w2) == 1 && o2.kind == CF_KIND_PLAY && o2.size == 0x400000, "kind play: 4 MiB taken (no cart to fit)");
        subst(b4, sizeof b4, b3, "\"size\": 262144", "\"size\": 4194305");
        check(cf_install_parse(b4, BASE, &o2, &w2) == -1 && !strcmp(w2, "size"), "kind play past 4 MiB: refused");
        subst(b4, sizeof b4, OFFER, "\"size\": 262144", "\"size\": 4194304");
        check(cf_install_parse(b4, BASE, &o2, &w2) == -1, "a cart install of 4 MiB: still refused");
        /* lane gamecomp1: a play send as an SGZ1 container ("z": "sgz1"; size/crc32/sha256 the container's, game_* the game's) */
        {
            char bz[1300];
            snprintf(bz, sizeof bz, "{\"kind\": \"play\", \"z\": \"sgz1\", \"game_size\": 2097152, \"game_crc32\": \"85229f87\", \"game_sha256\": \"%s\", %s",
                     "ab2c47fc0b7a5f7b0fb6ae6a1d92e58bd3a3d49cf3b5b8b3c9a7a3ff2c5e1f00", OFFER + 1);
            check(cf_install_parse(bz, BASE, &o2, &w2) == 1 && o2.kind == CF_KIND_PLAY && o2.z == 1 && o2.size == 262144 && o2.rom_size == 0x200000 &&
                  o2.rom_crc32 == 0x85229f87 && !strcmp(o2.rom_sha256, "ab2c47fc0b7a5f7b0fb6ae6a1d92e58bd3a3d49cf3b5b8b3c9a7a3ff2c5e1f00"), "gamecomp1: a z play offer: the container's size, the game's size and hashes");
            subst(b4, sizeof b4, bz, "\"size\": 262144", "\"size\": 2031617");
            check(cf_install_parse(b4, BASE, &o2, &w2) == 1 && o2.size == 2031617, "gamecomp1: a container bigger than the staging area: taken (it goes into the library, the download decides)");
            subst(b4, sizeof b4, bz, "\"size\": 262144", "\"size\": 4194305");
            check(cf_install_parse(b4, BASE, &o2, &w2) == -1 && !strcmp(w2, "size"), "gamecomp1: a container past 4 MiB: refused");
            subst(b4, sizeof b4, bz, "\"game_size\": 2097152", "\"game_size\": 4194305");
            check(cf_install_parse(b4, BASE, &o2, &w2) == -1 && !strcmp(w2, "size"), "gamecomp1: a z game past 4 MiB: refused");
            subst(b4, sizeof b4, bz, "\"sgz1\"", "\"lz4x\"");
            check(cf_install_parse(b4, BASE, &o2, &w2) == -1 && !strcmp(w2, "z"), "gamecomp1: an unknown z: refused");
            subst(b4, sizeof b4, bz, "\"game_crc32\": \"85229f87\", ", "");
            check(cf_install_parse(b4, BASE, &o2, &w2) == -1 && !strcmp(w2, "game_crc32"), "gamecomp1: z without the game's crc32: refused");
            check(cf_install_parse(b3, BASE, &o2, &w2) == 1 && o2.z == 0, "gamecomp1: a play offer with no z: the image is the game (1.1.0)");
        }
        snprintf(b3, sizeof b3, "{\"kind\": \"save\", %s", OFFER + 1);
        char *q = strstr(b3, "\"size\": 262144"); memcpy(q, "\"size\":  32768", 14);
        check(cf_install_parse(b3, BASE, &o2, &w2) == 1 && o2.kind == CF_KIND_SAVE && o2.size == 32768, "kind save: exactly 32 KiB taken");
        snprintf(b3, sizeof b3, "{\"kind\": \"save\", %s", OFFER + 1);
        check(cf_install_parse(b3, BASE, &o2, &w2) == -1 && !strcmp(w2, "size"), "a save that is not 32 KiB: refused");
        snprintf(b3, sizeof b3, "{\"kind\": \"romsave\", \"rom_size\": 229376, \"rom_crc32\": \"0a0b0c0d\", \"rom_sha256\": \"%s\", %s",
                 "2d48b2156992f3fbc71161d349c386cd866fed7f3b94499de3bae4c415781511", OFFER + 1);
        check(cf_install_parse(b3, BASE, &o2, &w2) == 1 && o2.kind == CF_KIND_ROMSAVE && o2.rom_size == 229376 && o2.rom_crc32 == 0x0a0b0c0d && o2.size == 262144,
              "kind romsave: the game's size and hashes, then 32 KiB of save");
        snprintf(b3, sizeof b3, "{\"kind\": \"romsave\", \"rom_size\": 200000, \"rom_crc32\": \"0a0b0c0d\", \"rom_sha256\": \"%s\", %s",
                 "2d48b2156992f3fbc71161d349c386cd866fed7f3b94499de3bae4c415781511", OFFER + 1);
        check(cf_install_parse(b3, BASE, &o2, &w2) == -1, "romsave: the sizes do not add up: refused");
        snprintf(b3, sizeof b3, "{\"kind\": \"romsave\", \"rom_size\": 229376, \"rom_crc32\": \"0a0b0c0d\", %s", OFFER + 1);
        check(cf_install_parse(b3, BASE, &o2, &w2) == -1, "romsave without the game's sha256: refused");
        /* the machine: a save send arrives, is written, INSTALLED */
        reset(); to_code();
        snprintf(b3, sizeof b3, "{\"session\": \"sent\", \"kind\": \"save\", %s", OFFER + 1);
        q = strstr(b3, "\"size\": 262144"); memcpy(q, "\"size\":  32768", 14);
        snprintf(poll_answer, sizeof poll_answer, "%s", b3);
        run(CF_RX_POLL_MS + 7000);
        check(armed == 1 && C.st == CI_ARMED && C.offer.kind == CF_KIND_SAVE, "a save send: the write is armed");
        job = CF_D_OK; run(1000);
        check(view().kind == CV_RESULT && !strcmp(view().title, "INSTALLED"), "INSTALLED");
        /* lane sdcard1: sends to the microSD card */
        printf("lane sdcard1: card sends\n");
        snprintf(b3, sizeof b3, "{\"kind\": \"play\", \"sd\": \"keep\", \"file\": \"Shiny Kart.gbc\", %s", OFFER + 1);
        check(cf_install_parse(b3, BASE, &o2, &w2) == 1 && o2.kind == CF_KIND_PLAY && o2.sd == CF_SD_KEEP && !strcmp(o2.file, "Shiny Kart.gbc"), "sd keep: a game banked on the card, its file name");
        snprintf(b3, sizeof b3, "{\"kind\": \"play\", \"sd\": \"play\", \"file\": \"a.gb\", %s", OFFER + 1);
        check(cf_install_parse(b3, BASE, &o2, &w2) == 1 && o2.sd == CF_SD_PLAY, "sd play: banked and played");
        snprintf(b3, sizeof b3, "{\"kind\": \"play\", %s", OFFER + 1);
        check(cf_install_parse(b3, BASE, &o2, &w2) == 1 && o2.sd == CF_SD_NONE && !o2.file[0], "no sd: 1.1.0's play exactly");
        const char *badf[] = { "../x.gb", "x.gba", ".x.gb", "a/b.gb", "a\\\\b.gb", "", "x", "noext" };
        int allbad = 1;
        for (unsigned i = 0; i < sizeof badf / sizeof *badf; i++) {
            snprintf(b3, sizeof b3, "{\"kind\": \"play\", \"sd\": \"keep\", \"file\": \"%s\", %s", badf[i], OFFER + 1);
            if (cf_install_parse(b3, BASE, &o2, &w2) != -1) { allbad = 0; printf("  taken: %s\n", badf[i]); }
        }
        check(allbad, "sd: a file name with a path, no game extension, hidden or empty: refused");
        snprintf(b3, sizeof b3, "{\"kind\": \"play\", \"sd\": \"keep\", %s", OFFER + 1);
        check(cf_install_parse(b3, BASE, &o2, &w2) == -1 && !strcmp(w2, "file"), "sd without a file: refused");
        snprintf(b3, sizeof b3, "{\"kind\": \"play\", \"sd\": \"burn\", \"file\": \"a.gb\", %s", OFFER + 1);
        check(cf_install_parse(b3, BASE, &o2, &w2) == -1 && !strcmp(w2, "sd"), "an unknown sd: refused");
        snprintf(b3, sizeof b3, "{\"kind\": \"rom\", \"sd\": \"keep\", \"file\": \"a.gb\", %s", OFFER + 1);
        check(cf_install_parse(b3, BASE, &o2, &w2) == -1 && !strcmp(w2, "sd"), "sd on a cart install: refused");
        snprintf(b3, sizeof b3, "{\"kind\": \"save\", \"sd\": \"keep\", \"file\": \"Shiny Kart.sav\", %s", OFFER + 1);
        q = strstr(b3, "\"size\": 262144"); memcpy(q, "\"size\":   8240", 14);
        check(cf_install_parse(b3, BASE, &o2, &w2) == 1 && o2.kind == CF_KIND_SAVE && o2.sd == CF_SD_KEEP && o2.size == 8240, "a card save of any size (8 KiB + the clock footer)");
        snprintf(b3, sizeof b3, "{\"kind\": \"save\", \"sd\": \"keep\", \"file\": \"Shiny Kart.sav\", %s", OFFER + 1);
        q = strstr(b3, "\"size\": 262144"); memcpy(q, "\"size\": 131121", 14);
        check(cf_install_parse(b3, BASE, &o2, &w2) == -1 && !strcmp(w2, "size"), "a card save over 128 KiB + 48: refused");
        snprintf(b3, sizeof b3, "{\"kind\": \"save\", \"sd\": \"play\", \"file\": \"a.sav\", %s", OFFER + 1);
        check(cf_install_parse(b3, BASE, &o2, &w2) == -1, "sd play on a save: refused");
        snprintf(b3, sizeof b3, "{\"kind\": \"save\", \"sd\": \"keep\", \"file\": \"a.gb\", %s", OFFER + 1);
        check(cf_install_parse(b3, BASE, &o2, &w2) == -1 && !strcmp(w2, "file"), "a card save must be a .sav");
        /* the machine: the start body from the host (a card), a card bank's result words */
        reset(); C.h.start_body = sd_body; to_code();
        check(strstr(last_start_body, "\"caps\":\"patch1,backup1,chain1,save1,play1,z1,keep1,psave1,sd1\"") && strstr(last_start_body, "\"free_mb\":29000"), "recv/start: a unit with a card says sd1 and its summary");
        snprintf(b3, sizeof b3, "{\"session\": \"sent\", \"kind\": \"play\", \"sd\": \"keep\", \"file\": \"Shiny Kart.gbc\", %s", OFFER + 1);
        snprintf(poll_answer, sizeof poll_answer, "%s", b3);
        run(CF_RX_POLL_MS + 7000);
        check(armed == 1 && C.st == CI_ARMED && C.offer.sd == CF_SD_KEEP, "a card bank: armed");
        job = CF_D_OK; run(1000);
        check(view().kind == CV_RESULT && !strcmp(view().title, "ON  THE  CARD"), "ON  THE  CARD");
        C.h.start_body = NULL;
    }
    printf("lane ui110dev: GET GAMES (catalog mode, the page's calls, lib keep / none)\n");
    {
        char jj[1100];
        snprintf(jj, sizeof jj, "{\"kind\": \"play\", \"lib\": \"keep\",%s", OFFER_FIELDS);
        check(cf_install_parse(jj, BASE, &o, &why) == 1 && o.kind == CF_KIND_PLAY && o.lib == CF_LIB_KEEP, "a play offer's lib keep");
        snprintf(jj, sizeof jj, "{\"kind\": \"play\", \"lib\": \"none\",%s", OFFER_FIELDS);
        check(cf_install_parse(jj, BASE, &o, &why) == 1 && o.lib == CF_LIB_NONE, "a play offer's lib none");
        snprintf(jj, sizeof jj, "{\"kind\": \"play\",%s", OFFER_FIELDS);
        check(cf_install_parse(jj, BASE, &o, &why) == 1 && o.lib == 0, "no lib: 0 (keep when it fits, and play)");
        snprintf(jj, sizeof jj, "{\"kind\": \"play\", \"lib\": \"later\",%s", OFFER_FIELDS);
        check(cf_install_parse(jj, BASE, &o, &why) == -1 && !strcmp(why, "lib"), "lib: keep / none only");
        snprintf(jj, sizeof jj, "{\"lib\": \"keep\",%s", OFFER_FIELDS);
        check(cf_install_parse(jj, BASE, &o, &why) == -1, "lib on a cart install: refused");
        snprintf(jj, sizeof jj, "{\"kind\": \"play\", \"sd\": \"keep\", \"file\": \"KART.gbc\", \"lib\": \"keep\",%s", OFFER_FIELDS);
        check(cf_install_parse(jj, BASE, &o, &why) == -1, "lib with a card send: refused (sd says where)");
        check(strstr(CF_RX_CAPS, ",keep1") != NULL, "the caps say keep1");

        reset();
        snprintf(poll_answer, sizeof poll_answer, "%s", WAITING);
        char out[700];
        int code = 0;
        check(cf_install_side(&C, "/link_device/catalog", "{\"p\":0,\"code\":\"%s\"}", out, sizeof out) == -1, "no call before receive mode has its code");
        cf_install_catalog(&C);
        check(C.cat == 1 && C.st == CI_RX_HOLD, "GET GAMES: receive mode for the catalog");
        run(2000);
        check(cf_install_has_code(&C) && view().kind == CV_CODE, "it has its code (the page does not show it)");
        n_side = 0; side_during_poll = 0;
        check(cf_install_side(&C, "/link_device/catalog", "{\"p\":0,\"code\":\"%s\"}", out, sizeof out) == 0, "a page asked");
        check(cf_install_side(&C, "/link_device/catalog", "{\"p\":1,\"code\":\"%s\"}", out, sizeof out) == -1, "one at a time");
        check(cf_install_side_done(&C, &code) == 0, "running");
        run(1500);
        check(cf_install_side_done(&C, &code) == 1 && code == 200 && !strncmp(out, "SGC1|0|2|8", 10), "the page came (code 200, its text)");
        check(strstr(last_side_body, "\"code\":\"" CODE "\"") != NULL, "the body carries this receive mode's code");
        check(n_side == 1 && !side_during_poll, "made between two polls, never beside one");
        check(cf_install_side_done(&C, &code) == -1, "and then forgotten");
        const int np = n_polls;
        snprintf(side_answer, sizeof side_answer, "{\"ok\":true,\"title\":\"SHINY KART COLOR\"}");
        check(cf_install_side(&C, "/link_device/catalog/order", "{\"code\":\"%s\",\"id\":\"kart\",\"to\":\"lib\",\"play\":true}", out, sizeof out) == 0, "an order asked");
        snprintf(poll_answer, sizeof poll_answer, "{\"session\": \"sent\", \"kind\": \"play\", \"lib\": \"keep\",%s", OFFER_FIELDS);
        run(1200);
        check(cf_install_side_done(&C, &code) == 1 && code == 200, "the order answered");
        check(n_polls > np && view().kind != CV_CODE, "the poll at once after the order: the offer came (no 4 s wait)");
        check(C.offer.lib == CF_LIB_KEEP, "the offer's lib kept");
        arm_once = CF_ARM_LIB_FULL;
        run(8000);
        cf_view_t v = view();
        check(v.kind == CV_RESULT && !strcmp(v.title, "NOT  KEPT") && strstr(v.note, "NO  ROOM:  DELETE|A  GAME  FIRST"), "no room: NOT KEPT, NO ROOM: DELETE A GAME FIRST (lane m110u: what to do)");
        reset();
        snprintf(poll_answer, sizeof poll_answer, "{\"session\": \"sent\", \"kind\": \"play\", \"lib\": \"keep\",%s", OFFER_FIELDS);
        cf_install_catalog(&C);
        run(2000); job = CF_D_OK; run(20000);
        v = view();
        check(v.kind == CV_RESULT && !strcmp(v.title, "ON  CHROMATIC"), "kept: ON CHROMATIC");
        /* m110t (the final gate: SHINY COLLECTION trimmed, refused for its size, said NOT ENOUGH MEMORY): each refusal its own line */
        { struct { const char *kind; int code; const char *title, *note, *ack; } r[] = {
            { "\"kind\": \"play\", \"lib\": \"keep\",", CF_ARM_CANT_PLAY, "NOT  KEPT", "CHROMATIC  CANNOT|PLAY  THIS  GAME", "not a game this Chromatic plays" },
            { "\"kind\": \"play\", \"lib\": \"keep\",", CF_ARM_TOO_BIG, "NOT  KEPT", "TOO  BIG  FOR|CHROMATIC", "too big" },
            { "\"kind\": \"play\", \"lib\": \"keep\",", CF_ARM_LIB_FAIL, "NOT  KEPT", "IT  COULD  NOT  BE|KEPT:  SEND  AGAIN", "the library did not take it" },
            { "\"kind\": \"play\", \"lib\": \"keep\",", CF_D_NO_ENGINE, "NOT  KEPT", "UPDATE  THIS|CHROMATIC  FIRST", "no RAM cart" },
            { "\"kind\": \"play\", \"lib\": \"keep\",", CF_D_BAD_IMAGE, "NOT  KEPT", "THE  GAME  DID|NOT  CHECK  OUT", "did not check out" },
            { "\"kind\": \"play\", \"lib\": \"keep\",", CF_D_ABORTED, "NOT  KEPT", "NOT  ENOUGH  MEMORY|RESTART,  SEND  AGAIN", "refused to arm" },
            { "\"kind\": \"play\", \"lib\": \"keep\",", CF_D_FAILED, "NOT  KEPT", "IT  DID  NOT  START|RESTART,  SEND  AGAIN", "refused to arm (" },
            { "\"kind\": \"play\", \"sd\": \"keep\", \"file\": \"a.gbc\",", CF_ARM_TOO_BIG, "NOT  ON  THE  CARD", "TOO  BIG  FOR|THE  CARD", "too big" },
            { "\"kind\": \"play\", \"sd\": \"keep\", \"file\": \"a.gbc\",", CF_ARM_CANT_PLAY, "NOT  ON  THE  CARD", "CHROMATIC  CANNOT|PLAY  THIS  GAME", "not a game" },
            { "\"kind\": \"play\", \"lib\": \"none\",", CF_ARM_CANT_PLAY, "NOT  PLAYING", "CHROMATIC  CANNOT|PLAY  THIS  GAME", "not a game" },
            { "\"kind\": \"play\", \"lib\": \"none\",", CF_ARM_TOO_BIG, "NOT  PLAYING", "TOO  BIG  FOR|CHROMATIC", "too big" },
          };
          for (unsigned i = 0; i < sizeof r / sizeof r[0]; i++) {
              reset();
              snprintf(poll_answer, sizeof poll_answer, "{\"session\": \"sent\", %s%s", r[i].kind, OFFER_FIELDS);
              cf_install_catalog(&C);
              arm_once = r[i].code;
              run(8000);
              v = view();
              char w[400];
              snprintf(w, sizeof w, "m110t: arm %#x (%s): %s / %s (got %s / %s, ack %s)", r[i].code, r[i].kind, r[i].title, r[i].note, v.title, v.note, last_ack());
              check(v.kind == CV_RESULT && !strcmp(v.title, r[i].title) && !strcmp(v.note, r[i].note) && strstr(last_ack(), r[i].ack), w);
              check(strstr(v.note, "MEMORY") == NULL || r[i].code == CF_D_ABORTED, "m110t: memory said only for memory");
          } }
        reset(); arm_ret = CF_D_BAD_IMAGE; to_code(); game_arrives();
        { cf_view_t vb = view(); check(vb.kind == CV_RESULT && !strcmp(vb.title, "NOT  INSTALLED") && strstr(vb.note, "NOT  FIT  THIS  CART") && strstr(last_ack(), "does not fit this cart"),
                                       "m110t: a cart image refused at the arm: THE GAME DOES NOT FIT THIS CART (not memory)"); }
        reset();
        snprintf(poll_answer, sizeof poll_answer, "%s", WAITING);
        cf_install_catalog(&C);
        run(2000);
        const int ns = n_starts;
        snprintf(poll_answer, sizeof poll_answer, "%s", EXPIRED);
        run(CF_RX_POLL_MS + 1000);
        snprintf(poll_answer, sizeof poll_answer, "%s", WAITING);
        run(3000);
        check(n_starts == ns + 1 && view().kind == CV_CODE && C.cat_renew == 1, "the code ended: a new one, no EXPIRED screen");
        run(11u * 60u * 1000u);
        check(view().kind == CV_CODE && C.cat_renew >= 2, "ten minutes on: still browsing (a new code by itself)");
        check(cf_install_side(&C, "/link_device/catalog", "{\"p\":1,\"code\":\"%s\"}", out, sizeof out) == 0, "a page asked");
        api_busy = 1;
        press(0);
        api_busy = 0;
        run(3000);
        check(cf_install_side_done(&C, &code) == -1 && code == -1, "B before the call: no answer will come (-1)");
        check(n_cancels >= 1 && !C.rx, "B: the code given back, receive mode over");
        press(1);
        check(C.cat == 0, "RECEIVE GAME: not GET GAMES's");
    }
    printf("cf_install_test: %d ok, %d failed\n", ok_n, bad_n);
    return bad_n ? 1 : 0;
}
