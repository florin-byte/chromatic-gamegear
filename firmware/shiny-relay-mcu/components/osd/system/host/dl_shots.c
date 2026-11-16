/* dl_shots.c (wifirecv1): SYSTEM > DOWNLOAD as the unit draws it, on the Mac. The real page
 * (download.c) over the real install machine (nw/cartflash/cf_install.c) driven through every
 * screen by mock hooks; each screen's seven rows are printed as osd_phone.py arguments
 * ("!" = white). make dl-shots renders them on the panel. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "osd_shared.h"
#include "nw_dev.h"
#include "../download.h"
#include "cf_install.h"
#include "cf_proto.h"

static lv_style_t W = {1}, G = {0};
lv_style_t *OSD_GetStyleTextWhite(void) { return &W; }
lv_style_t *OSD_GetStyleTextGrey(void) { return &G; }
static lv_obj_t *objs[16]; static int nobj;
lv_obj_t *lv_label_create(lv_obj_t *p) { (void)p; lv_obj_t *o = calloc(1, sizeof *o); objs[nobj++] = o; return o; }
void lv_obj_align(lv_obj_t *o, int a, int x, int y) { (void)a; o->x = x; o->y = y; }
void lv_obj_move_foreground(lv_obj_t *o) { (void)o; }
void lv_obj_add_style(lv_obj_t *o, lv_style_t *s, int sel) { (void)sel; o->style = s; }
void lv_obj_remove_style(lv_obj_t *o, lv_style_t *s, int sel) { (void)s; (void)sel; o->style = NULL; }
void lv_label_set_text(lv_obj_t *o, const char *t) { snprintf(o->text, sizeof o->text, "%s", t); }
char *lv_label_get_text(const lv_obj_t *o) { return (char *)o->text; }
void lv_obj_del(lv_obj_t *o) { for (int i = 0; i < nobj; i++) if (objs[i] == o) { objs[i] = objs[--nobj]; break; } free(o); }

/* the machine, with a scripted server */
static cf_inst_t C; static uint32_t T = 1000;
static const char *answer = "{}"; static int code = 200, power = 1, job = -1, pend = 0, wifi = 2, fpga = 1, pct = -1;
#define OFFERF " \"pending\": true, \"id\": \"0123456789abcdef0123456789abcdef\", \"title\": \"SHINY KART\", \"size\": 524288," \
    " \"crc32\": \"501a1d4b\", \"sha256\": \"2d48b2156992f3fbc71161d349c386cd866fed7f3b94499de3bae4c415781511\"," \
    " \"enc_alg\": \"AES-256-CTR\", \"enc_key\": \"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\"," \
    " \"enc_iv\": \"f0f1f2f3f4f5f6f7f8f9fafbfffffffe\", \"blob_url\": \"http://dl.shinyrelay.com/gbnet/v1/install/0123456789abcdef0123456789abcdef/blob/00112233445566778899aabbccddeeff\"}"
static int pc;
static int a_start(void *u, const char *path, const char *body, char *out, int cap)
{
    (void)u; (void)body;
    if (!strcmp(path, "/link_device/recv/start")) snprintf(out, cap, "{\"code\": \"KX7M2Q\", \"expires_in\": 600, \"poll_ms\": 4000}");
    else if (!strcmp(path, "/link_device/recv/poll") || !strcmp(path, "/link_device/recv/resume")) snprintf(out, cap, "%s", answer);
    else snprintf(out, cap, "{}");
    pc = 3;
    return 1;
}
static int a_result(void *u) { (void)u; if (pc > 0 && --pc) return 0; return code; }
static const char *base(void *u) { (void)u; return "https://api.shinyrelay.com/gbnet/v1"; }
static int one(void *u) { (void)u; return 1; }
static void none(void *u) { (void)u; }
static int f_fpga(void *u) { (void)u; return fpga; }
static int f_wifi(void *u) { (void)u; return wifi; }
static void hold(void *u, int on) { (void)u; (void)on; }
static int peek_wait;
static int peek_s(void *u, const cf_offer_t *o) { (void)u; (void)o; return 1; }
static int peek_r(void *u, char *t, char *n) { (void)u; if (peek_wait > 0) { peek_wait--; return -1; } snprintf(t, 17, "SHINY KART"); n[0] = 0; return 0; }
static int pow_(void *u) { (void)u; return power; }
static int arm(void *u, const cf_offer_t *o) { (void)u; (void)o; return 0; }
static int jobr(void *u) { (void)u; return job; }
static int pending(void *u, char *id, char *t) { (void)u; if (!pend) return 0; if (id) snprintf(id, 33, "0123456789abcdef0123456789abcdef"); if (t) snprintf(t, 17, "SHINY KART"); return 1; }
static void logm(void *u, const char *m) { (void)u; (void)m; }
static int busy(void *u) { (void)u; return 0; }
static void openp(void *u) { (void)u; }
static const cf_inst_host_t H = { NULL, a_start, a_result, base, one, none, f_fpga, f_wifi, hold, one, peek_s, peek_r, pow_, openp, arm, jobr, pending, none, logm, busy, NULL };

void NW_CartView(NwCartView_t *out)
{
    cf_view_t v; cf_install_view(&C, &v);
    memset(out, 0, sizeof *out);
    out->Kind = v.kind; snprintf(out->Title, sizeof out->Title, "%s", v.title); snprintf(out->Note, sizeof out->Note, "%s", v.note);
    snprintf(out->Code, sizeof out->Code, "%s", v.code); out->Mins = v.mins; out->Anyway = v.anyway;
    if (v.kind == CV_INSTALLING) { if (pct >= 0) snprintf(out->Note, sizeof out->Note, "%d%%", pct); else snprintf(out->Note, sizeof out->Note, "STARTING"); }
}
void NW_CartButton(int a) { cf_install_button(&C, a); }

static void run(uint32_t ms) { for (uint32_t e = 0; e < ms; e += 100) { T += 100; cf_install_tick(&C, T); } }
static void press(int a) { Download_OnButton(a ? kButton_A : kButton_B, kButtonState_Pressed, NULL); run(100); }
static void shot(const char *name)
{
    static lv_obj_t screen;
    Download_Draw(&screen);
    printf("%s", name);
    for (int r = 0; r < 7; r++) {
        const lv_obj_t *o = NULL;
        for (int i = 0; i < nobj; i++) if (objs[i]->y == 46 + 9 * r) o = objs[i];
        printf("|%s%s", o && o->style == &W ? "!" : "", o ? o->text : "");
    }
    printf("\n");
}
int main(void)
{
    cf_install_init(&C, &H, T); run(500);
    shot("01_menu");
    answer = "{\"session\": \"waiting\", \"expires_in\": 590}";
    press(1); shot("02_getting_ready");
    run(2000); shot("03_code");
    run(6u * 60u * 1000u); shot("04_code_later");
    answer = "{\"session\": \"sent\"," OFFERF; power = 0; run(4500); shot("06_battery_low");
    power = 1; run(300); pct = -1; shot("07_installing_start"); pct = 37; shot("08_installing_37");
    job = CF_D_PAUSED; pend = 1; run(1000); shot("09_unfinished_paused");
    job = -1; code = 200; run(CF_INST_RETRY_MS + 2000); pct = 81; shot("10_installing_resumed");
    job = CF_D_OK; pend = 0; run(1000); shot("11_installed");
    press(1);
    /* a fresh start for the other ends */
    cf_install_init(&C, &H, T); answer = "{\"session\": \"waiting\"}"; press(1); run(2000); run(10u * 60u * 1000u + 1000u); shot("12_code_expired");
    press(0);
    job = -1; cf_install_init(&C, &H, T); answer = "{\"session\": \"sent\"," OFFERF; peek_wait = 1000; press(1); run(2000); run(3900); shot("05_game_arrived"); peek_wait = 0;
    run(3000); job = CF_D_LOCKED; run(1000); shot("13_not_installed_locked");
    press(1);
    cf_install_init(&C, &H, T); wifi = 0; press(1); run(1000); shot("14_no_wifi"); wifi = 2; press(0);
    cf_install_init(&C, &H, T); code = 401; press(1); run(3000); shot("15_not_registered"); code = 200; press(0);
    cf_install_init(&C, &H, T); fpga = 0; press(1); run(500); shot("16_update_fpga"); fpga = 1; press(0);
    pend = 1; cf_install_init(&C, &H, T); run(500); shot("17_unfinished_after_restart"); pend = 0;
    /* 1.0.5: INSTALL ANYWAY on BATTERY LOW */
    job = -1; cf_install_init(&C, &H, T); answer = "{\"session\": \"sent\"," OFFERF; power = 0; press(1); run(6000); shot("18_battery_low_install_anyway");
    press(1); run(300); pct = 12; shot("19_installing_low_battery");
    return 0;
}
