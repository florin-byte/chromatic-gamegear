/* custom_shots.c (1.1.12): SYSTEM > CUSTOM and SYSTEM > UPDATE as the unit draws them, on the Mac. The real pages
 * (custom.c, update.c) over the real update machine (nw/cartflash/cf_ota.c) with a scripted server, pairing and
 * download. Every screen's seven rows are printed ("name|row0|..|row6", "!" = white) for custom_fit / the PNGs, and
 * the rules are checked: UPDATE asks for and takes official releases only, CUSTOM asks with the pairing's call and
 * takes custom builds only, nothing is fetched before A on BUILD WAITING, B on a custom build goes back to official.
 * Exit 1 when a rule fails. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "osd_shared.h"
#include "nw_dev.h"
#include "cwl_online.h"
#include "../custom.h"
#include "../update.h"
#include "cf_ota.h"

static lv_style_t W = {1}, G = {0};
lv_style_t *OSD_GetStyleTextWhite(void) { return &W; }
lv_style_t *OSD_GetStyleTextGrey(void) { return &G; }
static lv_obj_t *objs[32]; static int nobj;
lv_obj_t *lv_label_create(lv_obj_t *p) { (void)p; lv_obj_t *o = calloc(1, sizeof *o); objs[nobj++] = o; return o; }
void lv_obj_align(lv_obj_t *o, int a, int x, int y) { (void)a; o->x = x; o->y = y; }
void lv_obj_move_foreground(lv_obj_t *o) { (void)o; }
void lv_obj_add_style(lv_obj_t *o, lv_style_t *s, int sel) { (void)sel; o->style = s; }
void lv_obj_remove_style(lv_obj_t *o, lv_style_t *s, int sel) { (void)s; (void)sel; o->style = NULL; }
void lv_label_set_text(lv_obj_t *o, const char *t) { snprintf(o->text, sizeof o->text, "%s", t); }
char *lv_label_get_text(const lv_obj_t *o) { return (char *)o->text; }
void lv_obj_del(lv_obj_t *o) { for (int i = 0; i < nobj; i++) if (objs[i] == o) { objs[i] = objs[--nobj]; break; } free(o); }

/* ---- the scripted unit */
#define SHA "5f6ec8a3b318f45353335195ef53079a66b09d56768fe988d0913bcbe98b66ab"
#define URL "\"url\": \"http://dl.shinyrelay.com/gbnet/v1/fw/" SHA ".bin\""
#define REL "{\"update\": true, \"version\": \"1.1.12\", \"size\": 1756240, \"sha256\": \"" SHA "\", " URL "}"
#define CUS "{\"update\": true, \"version\": \"1.1.12-cfw.ab12\", \"name\": \"BLUE  MENU\", \"size\": 1500000, \"sha256\": \"" SHA "\", " URL "}"
static cf_ota_t C; static uint32_t T = 1000;
static const char *running = "1.1.12";
static const char *ans_check = "{\"update\": false}", *ans_custom = "{\"update\": false}";
static int code_check = 200, code_custom = 200, pc, last_code, n_check, n_custom, n_dl, dl_ret = -1, dl_pct, ready_ret, n_forget, n_install;
static char body_check[300], body_custom[300];
static cf_ota_offer_t dl_offer, ready_offer;
static int a_start(void *u, const char *path, const char *body, char *out, int cap)
{
    (void)u;
    if (!strcmp(path, "/link_device/fw/custom")) { n_custom++; snprintf(body_custom, sizeof body_custom, "%s", body); snprintf(out, cap, "%s", ans_custom); last_code = code_custom; }
    else if (!strcmp(path, "/link_device/fw/check")) { n_check++; snprintf(body_check, sizeof body_check, "%s", body); snprintf(out, cap, "%s", ans_check); last_code = code_check; }
    else { snprintf(out, cap, "{}"); last_code = 200; }
    pc = 3;
    return 1;
}
static int a_result(void *u) { (void)u; if (pc > 0 && --pc) return 0; return last_code; }
static int one(void *u) { (void)u; return 1; }
static int two(void *u) { (void)u; return 2; }
static int zero(void *u) { (void)u; return 0; }
static void none(void *u) { (void)u; }
static void hold(void *u, int on) { (void)u; (void)on; }
static void me(void *u, char v[16], char f[8], char pt[65], char bl[65], char fl[8])
{ (void)u; snprintf(v, 16, "%.15s", running); snprintf(f, 8, "18.33"); memset(pt, 'a', 64); pt[64] = 0; memset(bl, 'b', 64); bl[64] = 0; snprintf(fl, 8, "rf"); }
static int dl_start(void *u, const cf_ota_offer_t *o) { (void)u; dl_offer = *o; n_dl++; return 0; }
static int dl_result(void *u, int *pct) { (void)u; *pct = dl_pct; if (dl_ret < 0) return -1; if (dl_ret == 0) { ready_offer = dl_offer; ready_ret = 1; } return dl_ret; }
static int ready(void *u, cf_ota_offer_t *o) { (void)u; if (ready_ret) { *o = ready_offer; o->name[0] = 0; /* (the slot's record keeps no name) */ } return ready_ret; }
static void ready_forget(void *u) { (void)u; ready_ret = 0; n_forget++; }
static int install(void *u, const cf_ota_offer_t *o) { (void)u; (void)o; n_install++; return -1; }
static void logm(void *u, const char *m) { (void)u; if (getenv("V")) fprintf(stderr, "    %s\n", m); }
static const cf_ota_host_t H = { NULL, a_start, a_result, one, none, two, hold, one, zero, one, me, dl_start, dl_result, none, ready, ready_forget, install, logm };

/* nw_dev.c / cf_dev.c's side of the page (CfDev_FwView without the FPGA views) */
void NW_FwView(NwFwView_t *out)
{
    cf_ota_view_t v; cf_ota_view(&C, &v);
    memset(out, 0, sizeof *out);
    out->Kind = v.kind; snprintf(out->Title, sizeof out->Title, "%s", v.title); snprintf(out->Note, sizeof out->Note, "%s", v.note);
    snprintf(out->Version, sizeof out->Version, "%s", v.version); snprintf(out->Running, sizeof out->Running, "%.15s", running);
    out->Pct = v.pct; out->Custom = v.custom; snprintf(out->Name, sizeof out->Name, "%s", v.name);
    if (v.kind == OV_MENU && ready_ret) { out->ReadyWaits = 1; snprintf(out->Version, sizeof out->Version, "%s", ready_offer.version); out->Custom = cf_ota_is_custom(ready_offer.version); }
}
void NW_FwButton(int a) { cf_ota_button(&C, a); }
void NW_FwCustom(void) { if (C.st == OT_IDLE) cf_ota_custom(&C); }

/* the pairing */
static int paired; static CwlPairState_t pair = kCwlPair_Idle; static int n_pair_start;
bool CwlOnline_Paired(void) { return paired; }
const char *CwlOnline_DeviceId(void) { return paired ? "00000000000d4e5f" : ""; }
void CwlOnline_PairStart(void) { n_pair_start++; pair = kCwlPair_Starting; }
void CwlOnline_PairCancel(void) { pair = kCwlPair_Idle; }
CwlPairState_t CwlOnline_PairState(void) { return pair; }
const char *CwlOnline_PairError(void) { return "CODE  EXPIRED"; }
void CwlOnline_PairCode(char *code, size_t n, char *url, size_t un) { snprintf(code, n, "WXYZ-MMWW"); if (un) url[0] = 0; }

static int ok_n, bad_n;
static void check(int c, const char *what) { if (c) ok_n++; else { bad_n++; fprintf(stderr, "  FAIL %s\n", what); } }
static void run(uint32_t ms) { for (uint32_t e = 0; e < ms; e += 100) { T += 100; cf_ota_tick(&C, T); } }
static int on_custom = 1;
static void page(int custom) { if (custom != on_custom) { if (on_custom) Custom_OnTransition(NULL); else Update_OnTransition(NULL); on_custom = custom; } }
static void press(int a)
{
    if (on_custom) Custom_OnButton(a ? kButton_A : kButton_B, kButtonState_Pressed, NULL);
    else Update_OnButton(a ? kButton_A : kButton_B, kButtonState_Pressed, NULL);
    run(100);
}
static char rows[7][64];
static void shot(const char *name)
{
    static lv_obj_t screen;
    if (on_custom) Custom_Draw(&screen); else Update_Draw(&screen);
    printf("%s", name);
    for (int r = 0; r < 7; r++) {
        const lv_obj_t *o = NULL;
        for (int i = 0; i < nobj; i++) if (objs[i]->y == 46 + 9 * r) o = objs[i];
        snprintf(rows[r], sizeof rows[r], "%s", o ? o->text : "");
        printf("|%s%s", o && o->style == &W ? "!" : "", o ? o->text : "");
    }
    printf("\n");
}
static int has(const char *t) { for (int r = 0; r < 7; r++) if (strstr(rows[r], t)) return 1; return 0; }
static void fresh(void)
{
    ans_check = ans_custom = "{\"update\": false}"; code_check = code_custom = 200; n_check = n_custom = n_dl = n_forget = n_install = 0;
    dl_ret = -1; dl_pct = 0; ready_ret = 0; body_check[0] = body_custom[0] = 0;
    cf_ota_init(&C, &H, T);
}

int main(void)
{
    /* ---- SYSTEM > CUSTOM, not linked */
    fresh(); page(1); paired = 0; pair = kCwlPair_Idle;
    shot("c01_not_linked"); check(has("NOT  LINKED  YET") && has("A:  LINK  THIS"), "not linked: says so, A links");
    press(0); check(n_check == 0 && n_custom == 0 && n_pair_start == 0, "not linked: B does nothing");
    press(1); check(n_pair_start == 1 && n_custom == 0, "not linked: A starts the pairing, no build check");
    shot("c02_getting_code");
    pair = kCwlPair_Waiting; shot("c03_code"); check(has("SHINYRELAY.COM") && has("/LINK") && has("WXYZ-MMWW"), "the code and the short address");
    press(1); check(pair == kCwlPair_Waiting, "A on the code does nothing");
    pair = kCwlPair_Failed; shot("c04_link_failed");
    press(0); check(pair == kCwlPair_Idle, "B on LINKING FAILED: back");
    /* ---- linked, official version running */
    paired = 1; pair = kCwlPair_Done; running = "1.1.12";
    shot("c05_linked"); check(has("A:  CHECK  FOR  BUILD") && !has("BACK  TO  OFFICIAL") && has("NOW  1.1.12") && has("ID  4E5F"), "linked on a release: A checks, no BACK TO OFFICIAL");
    press(0); check(n_check == 0 && n_custom == 0, "linked on a release: B does nothing");
    press(1); shot("c06_checking"); run(2000);
    check(n_custom == 1 && n_check == 0 && !strstr(body_custom, "split"), "A asks /link_device/fw/custom (the pairing's call), not the update check");
    shot("c07_no_build"); check(has("NO  BUILD  WAITING"), "none chosen: NO BUILD WAITING");
    press(1);
    ans_custom = CUS; press(1); run(2000);
    shot("c08_build_waiting"); check(has("BUILD  WAITING") && has("BLUE  MENU") && has("1.1.12-cfw.ab12") && has("A:  DOWNLOAD") && n_dl == 0, "a build waits: its name and version, nothing fetched yet");
    press(1); check(n_dl == 1 && !strcmp(dl_offer.version, "1.1.12-cfw.ab12"), "A downloads it");
    dl_pct = 37; run(300); shot("c09_downloading");
    dl_ret = 0; run(300); shot("c10_build_ready"); check(has("BUILD  READY") && has("A:  INSTALL") && has("B:  LATER"), "BUILD READY: A installs, B later");
    press(0); shot("c11_linked_build_ready_later"); check(has("A  BUILD  IS  READY") && has("A:  SEE  IT") && ready_ret == 1, "LATER: it stays in the slot and the page says so");
    page(0); shot("u01_update_menu_while_custom_waits"); check(!has("WAITS") && !has("cfw") && has("OFFICIAL  UPDATES"), "UPDATE does not show the waiting custom build");
    page(1); { const int c0 = n_custom; press(1); check(n_custom == c0 && has("") && C.st == OT_READY, "A again: READY at once"); }
    press(1); run(200); check(n_install == 1, "A on READY installs (a real unit restarts)");
    press(1);
    /* B on BUILD WAITING, and its time limit */
    fresh(); ans_custom = CUS; press(1); run(2000); press(0); shot("c12_cancelled"); check(n_dl == 0 && has("CANCELLED"), "B on BUILD WAITING: nothing fetched");
    press(1); press(1); run(2000); run(CF_OTA_OFFER_MS + 500); check(n_dl == 0 && C.st == OT_RESULT, "BUILD WAITING left alone ends by itself");
    press(1);
    /* the other answers */
    fresh(); ans_custom = "{\"update\": false, \"running\": true}"; press(1); run(2000); shot("c13_already_installed"); check(has("ALREADY  INSTALLED"), "this unit runs the build already"); press(1);
    fresh(); ans_custom = "{\"update\": false, \"first\": true}"; press(1); run(2000); shot("c14_update_first"); check(has("UPDATE  FIRST"), "the release first"); press(1);
    fresh(); ans_custom = "{\"update\": false, \"usb\": true, \"why\": \"layout\", \"version\": \"1.1.12-cfw.ab12\"}"; press(1); run(2000); shot("c15_over_usb"); check(has("INSTALL  OVER  USB"), "another flash layout: over USB"); press(1);
    fresh(); code_custom = 401; press(1); run(2000); shot("c16_not_linked_any_more"); check(has("NOT  LINKED") && n_check == 0, "the pairing was ended: NOT LINKED (the registration is not touched)"); press(1);
    fresh(); code_custom = 404; press(1); run(2000); shot("c17_server_without_custom"); check(C.st == OT_RESULT && n_dl == 0, "a server without the route: a plain result"); press(1);
    fresh(); ans_custom = REL; press(1); run(2000); shot("c18_release_refused"); check(n_dl == 0 && has("CHECK  FAILED"), "CUSTOM refuses an answer that is not a custom build"); press(1);
    fresh(); ans_custom = "{\"update\": true, \"version\": \"1.1.12-cfw.ab12\", \"size\": 1500000, \"sha256\": \"" SHA "\", " URL ", \"fpga_version\": \"18.34\", \"fpga_size\": 768397, \"fpga_sha256\": \"" SHA "\", \"fpga_url\": \"http://dl.shinyrelay.com/gbnet/v1/fw/" SHA ".bin\"}";
    press(1); run(2000); check(n_dl == 0 && has("") && C.st == OT_RESULT, "CUSTOM refuses an FPGA image over Wi-Fi"); press(1);
    /* ---- a custom build runs: BACK TO OFFICIAL */
    fresh(); running = "1.1.12-cfw.ab12";
    shot("c19_linked_on_custom"); check(has("NOW  1.1.12-cfw.ab12") && has("B:  BACK  TO  OFFICIAL"), "on a custom build: B: BACK TO OFFICIAL");
    ans_check = REL; press(0); run(2000);
    check(n_check == 1 && n_custom == 0 && strstr(body_check, "\"split\":1") && strstr(body_check, "\"version\":\"1.1.12-cfw.ab12\""), "B asks the official check (split), saying the custom version it runs");
    check(n_dl == 1 && !strcmp(dl_offer.version, "1.1.12"), "the release downloads");
    dl_pct = 80; run(200); shot("c20_back_to_official_downloading");
    dl_ret = 0; run(300); shot("c21_back_to_official_ready"); check(has("UPDATE  READY") && has("1.1.12") && !has("cfw"), "UPDATE READY 1.1.12");
    press(0);
    shot("c22_on_custom_release_waits"); check(!has("A  BUILD  IS  READY"), "a waiting release is not called a build");
    /* a custom check drops the waiting release; an update check drops a waiting build */
    ans_custom = CUS; press(1); run(2000); check(n_forget == 1 && has("") && C.st == OT_OFFER, "A: CHECK FOR BUILD with a release waiting: the release is dropped, the build shown");
    press(0); press(1);
    /* ---- SYSTEM > UPDATE: official only */
    fresh(); running = "1.1.11"; page(0);
    shot("u02_update_menu"); check(has("A:  CHECK  FOR") && has("VERSION  1.1.11"), "UPDATE's menu");
    press(1); run(2000); check(n_check == 1 && n_custom == 0 && strstr(body_check, "\"split\":1"), "UPDATE's check says split:1 and never asks for a custom build");
    shot("u03_up_to_date"); press(1);
    ans_check = CUS; press(1); run(2000); shot("u04_custom_answer_refused"); check(n_dl == 0 && has("CHECK  FAILED"), "UPDATE refuses a custom build even when a server sends one"); press(1);
    ans_check = REL; press(1); run(2000); dl_pct = 12; run(200); shot("u05_downloading"); dl_ret = 0; run(300); shot("u06_update_ready"); check(has("UPDATE  READY"), "a release: UPDATE READY");
    press(0); shot("u07_update_waits"); check(has("1.1.12  WAITS"), "LATER: the release waits");
    page(1); running = "1.1.11"; shot("c23_custom_page_release_waits"); check(!has("A  BUILD  IS  READY"), "CUSTOM does not show a waiting release as a build");
    /* a waiting custom build and A on UPDATE */
    fresh(); page(1); ans_custom = CUS; press(1); run(2000); press(1); dl_ret = 0; run(300); press(0);
    page(0); ans_check = "{\"update\": false}"; press(1); run(2000);
    check(n_forget == 1 && n_install == 0 && n_check == 1, "A on UPDATE with a custom build waiting: it is dropped and the official check runs (never installed from UPDATE)");
    press(1);
    /* the boot's notices */
    fresh(); cf_ota_notice(&C, 1, "1.1.12-cfw.ab12"); page(1); shot("c24_build_installed"); check(has("BUILD  INSTALLED"), "a custom build confirmed itself"); press(1);
    cf_ota_notice(&C, 1, "1.1.12"); page(0); shot("u08_updated"); check(has("UPDATED"), "a release confirmed itself"); press(1);
    /* (UPDATE DID NOT START, 1.1.0's own title, is 151 px: 3 px over the panel rule; not this change's, not rendered here) */
    fprintf(stderr, "custom_shots: %d ok, %d failed\n", ok_n, bad_n);
    return bad_n ? 1 : 0;
}
