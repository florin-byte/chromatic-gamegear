/* cf_ota_test.c: cf_ota.c (Shiny Relay 1.1.0, the update over Wi-Fi) against a mock device API,
 * a mock download into the idle slot and a mock install. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../cf_ota.h"

#define SHA "5f6ec8a3b318f45353335195ef53079a66b09d56768fe988d0913bcbe98b66ab"
static char answer[600];
static int ans_code = 200, api_busy, delay_left, pending, registered = 1, n_regs, n_checks, wifi = 2, held_ret = 1, busy_ret, slots_ret = 1;
static int hold_on, n_hold_on, dl_running, dl_ret = -1, dl_pct, n_dl, n_cancel, ready_ret, n_install, n_forget;
static char last_body[300], last_path[64];
static cf_ota_offer_t dl_offer, ready_offer;
static int ok_n, bad_n;
static void check(int c, const char *what) { if (c) ok_n++; else { bad_n++; printf("  FAIL %s\n", what); } }

static int f_api_start(void *u, const char *path, const char *body, char *out, int cap)
{
    (void)u;
    if (api_busy) return 0;
    snprintf(last_path, sizeof last_path, "%s", path);
    snprintf(last_body, sizeof last_body, "%s", body);
    if (!strcmp(path, "/link_device/register")) { n_regs++; registered = 1; snprintf(out, cap, "{}"); pending = 200; }
    else { n_checks++; snprintf(out, cap, "%s", answer); pending = ans_code; }
    delay_left = 2;
    return 1;
}
static int f_api_result(void *u) { (void)u; if (!pending) return 0; if (delay_left-- > 0) return 0; const int c = pending; pending = 0; return c; }
static int f_registered(void *u) { (void)u; return registered; }
static void f_reg_forget(void *u) { (void)u; registered = 0; }
static int f_wifi(void *u) { (void)u; return wifi; }
static void f_hold(void *u, int on) { (void)u; hold_on = on; if (on) n_hold_on++; }
static int f_held(void *u) { (void)u; return held_ret; }
static int f_busy(void *u) { (void)u; return busy_ret; }
static int f_slots(void *u) { (void)u; return slots_ret; }
static const char *fl_ret = "ab";
static void f_me(void *u, char v[16], char f[8], char pt[65], char bl[65], char fl[8])
{ (void)u; snprintf(v, 16, "1.1.0"); snprintf(f, 8, "18.28"); memset(pt, 'a', 64); pt[64] = 0; memset(bl, 'b', 64); bl[64] = 0; snprintf(fl, 8, "%s", fl_ret); }
static int f_dl_start(void *u, const cf_ota_offer_t *o) { (void)u; dl_offer = *o; dl_running = 1; n_dl++; return 0; }
static int f_dl_result(void *u, int *pct) { (void)u; *pct = dl_pct; if (dl_ret < 0) return -1; dl_running = 0; if (dl_ret == 0) { ready_offer = dl_offer; ready_ret = 1; } return dl_ret; }
static void f_dl_cancel(void *u) { (void)u; n_cancel++; dl_running = 0; }
static int f_ready(void *u, cf_ota_offer_t *o) { (void)u; if (ready_ret) *o = ready_offer; return ready_ret; }
static void f_ready_forget(void *u) { (void)u; ready_ret = 0; n_forget++; }
static int install_ret = -1;
static cf_ota_offer_t inst_offer;
static int install_pending;
static int f_install(void *u, const cf_ota_offer_t *o) { (void)u; inst_offer = *o; n_install++; if (install_pending > 0) { install_pending--; return CF_OTA_PENDING; } return install_ret; }   /* (a real one with an app restarts) */
static void f_log(void *u, const char *m) { (void)u; if (getenv("V")) printf("    %s\n", m); }
static const cf_ota_host_t H = { NULL, f_api_start, f_api_result, f_registered, f_reg_forget, f_wifi, f_hold, f_held, f_busy, f_slots, f_me,
                                 f_dl_start, f_dl_result, f_dl_cancel, f_ready, f_ready_forget, f_install, f_log };

static cf_ota_t C;
static uint32_t T;
static void run(uint32_t ms) { for (uint32_t e = 0; e < ms; e += 100) { T += 100; cf_ota_tick(&C, T); } }
static void press(int a) { cf_ota_button(&C, a); run(100); }
static cf_ota_view_t view(void) { cf_ota_view_t v; cf_ota_view(&C, &v); return v; }
static void reset(void)
{
    ans_code = 200; api_busy = 0; pending = 0; registered = 1; n_regs = n_checks = 0; wifi = 2; held_ret = 1; busy_ret = 0; slots_ret = 1;
    hold_on = n_hold_on = 0; dl_running = 0; dl_ret = -1; dl_pct = 0; n_dl = n_cancel = 0; ready_ret = 0; n_install = n_forget = 0;
    install_ret = -1; install_pending = 0; fl_ret = "ab"; memset(&inst_offer, 0, sizeof inst_offer);
    snprintf(answer, sizeof answer, "{\"update\": false}");
    T = 1000;
    cf_ota_init(&C, &H, T);
}
#define FSHA "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define FPART "\"fpga_version\": \"18.31\", \"fpga_size\": 768397, \"fpga_sha256\": \"" FSHA "\", \"fpga_url\": \"http://dl.shinyrelay.com/gbnet/v1/fw/" FSHA ".bin\""
#define OFFER_BOTH "{\"update\": true, \"version\": \"1.1.1\", \"size\": 1756240, \"sha256\": \"" SHA "\", \"url\": \"http://dl.shinyrelay.com/gbnet/v1/fw/" SHA ".bin\", " FPART "}"
#define OFFER_FPGA "{\"update\": true, \"version\": \"1.1.1\", " FPART "}"
#define OFFER "{\"update\": true, \"version\": \"1.1.1\", \"size\": 1756240, \"sha256\": \"" SHA "\", \"url\": \"http://dl.shinyrelay.com/gbnet/v1/fw/" SHA ".bin\"}"

int main(void)
{
    cf_ota_offer_t o; const char *why;
    printf("parse\n");
    check(cf_ota_parse("{\"update\": false}", &o, &why) == 0, "none");
    check(cf_ota_parse(OFFER, &o, &why) == 1 && !strcmp(o.version, "1.1.1") && o.size == 1756240 && !strcmp(o.sha256, SHA), "an offer");
    check(cf_ota_parse("{\"update\": false, \"usb\": true, \"why\": \"fpga\", \"version\": \"1.2.0\"}", &o, &why) == 2 && !strcmp(why, "fpga") && !strcmp(o.version, "1.2.0"), "over USB: a newer FPGA");
    check(cf_ota_parse("{\"update\": false, \"usb\": true, \"why\": \"layout\"}", &o, &why) == 2 && !strcmp(why, "layout"), "over USB: another layout");
    { const char *bad[] = {
          "{\"update\": true, \"version\": \"1.1.1\", \"size\": 100, \"sha256\": \"" SHA "\", \"url\": \"http://dl.shinyrelay.com/gbnet/v1/fw/" SHA ".bin\"}",
          "{\"update\": true, \"version\": \"1.1.1\", \"size\": 1756240, \"sha256\": \"" SHA "\", \"url\": \"https://dl.shinyrelay.com/gbnet/v1/fw/" SHA ".bin\"}",
          "{\"update\": true, \"version\": \"1.1.1\", \"size\": 1756240, \"sha256\": \"" SHA "\", \"url\": \"http://dl.shinyrelay.com/gbnet/v1/fw/00" SHA ".bin\"}",
          "{\"update\": true, \"version\": \"1.1.1\", \"size\": 1756240, \"sha256\": \"" SHA "\", \"url\": \"http://dl.shinyrelay.com/x/gbnet/v1/fw/" SHA ".bin\"}",
          "{\"update\": true, \"version\": \"x;1\", \"size\": 1756240, \"sha256\": \"" SHA "\", \"url\": \"http://dl.shinyrelay.com/gbnet/v1/fw/" SHA ".bin\"}",
          "{\"update\": true, \"version\": \"1.1.1\", \"size\": 1756240, \"sha256\": \"ZZ\", \"url\": \"http://dl.shinyrelay.com/gbnet/v1/fw/ZZ.bin\"}" };
      for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) { char w[40]; snprintf(w, sizeof w, "refused #%u", i); check(cf_ota_parse(bad[i], &o, &why) == -1, w); } }

    printf("nothing happens by itself\n"); reset();
    run(3600u * 1000u);
    check(n_checks == 0 && n_hold_on == 0 && view().kind == OV_MENU, "an hour idle: no call, no hold");

    printf("up to date\n"); reset(); press(1); run(2000);
    check(n_checks == 1 && !strcmp(last_path, "/link_device/fw/check") && strstr(last_body, "\"version\":\"1.1.0\"") && strstr(last_body, "\"fpga\":\"18.28\""), "the check says this unit's version, FPGA, table and boot loader");
    check(view().kind == OV_RESULT && !strcmp(view().title, "UP  TO  DATE") && hold_on == 0, "UP TO DATE, the link back");
    press(1); check(view().kind == OV_MENU, "A: back to the menu");

    printf("an update: download, READY, LATER, then INSTALL\n"); reset(); snprintf(answer, sizeof answer, "%s", OFFER);
    press(1); run(2000);
    check(n_dl == 1 && view().kind == OV_DL && !strcmp(view().version, "1.1.1") && hold_on == 1, "DOWNLOADING 1.1.1, the link held");
    dl_pct = 42; run(300); check(view().pct == 42, "the percent");
    dl_ret = 0; run(300);
    check(view().kind == OV_READY && !strcmp(view().version, "1.1.1") && hold_on == 0, "UPDATE READY 1.1.1, the link back");
    press(0); check(view().kind == OV_MENU && ready_ret == 1, "B: LATER, it stays in the slot");
    { const int c0 = n_checks; press(1); check(view().kind == OV_READY && n_checks == c0, "A again: READY at once (no new check, no new download)"); }
    press(1); run(200);
    check(n_install == 1, "A on READY installs (the unit restarts)");
    check(view().kind == OV_RESULT && !strcmp(view().title, "UPDATE  FAILED") && n_forget == 1, "an install that did not take: UPDATE FAILED, forgotten");

    printf("refusals\n");
    reset(); snprintf(answer, sizeof answer, "{\"update\": false, \"usb\": true, \"why\": \"fpga\", \"version\": \"1.2.0\"}"); press(1); run(2000);
    check(view().kind == OV_RESULT && !strcmp(view().title, "UPDATE  OVER  USB") && strstr(view().note, "CONSOLE") && n_dl == 0, "a newer FPGA: UPDATE OVER USB, nothing downloaded");
    reset(); slots_ret = 0; press(1); run(500);
    check(view().kind == OV_RESULT && !strcmp(view().title, "UPDATE  OVER  USB") && n_checks == 0, "the 1.0.x layout: UPDATE OVER USB (no call)");
    reset(); busy_ret = 1; press(1); run(500);
    check(view().kind == OV_RESULT && !strcmp(view().title, "NOT  NOW") && n_checks == 0, "a cart job running: NOT NOW");
    reset(); wifi = 0; press(1); run(500);
    check(view().kind == OV_RESULT && !strcmp(view().title, "NO  WI-FI"), "no network saved: NO WI-FI");
    reset(); ans_code = -1; press(1); run(62000);
    check(view().kind == OV_RESULT && strstr(view().note, "CONNECTION"), "no answer: NO CONNECTION");
    reset(); registered = 0; press(1); run(3000);
    check(n_regs == 1 && n_checks == 1, "an unregistered unit registers first, then checks");
    reset(); snprintf(answer, sizeof answer, "%s", OFFER); press(1); run(2000); press(0); run(200);
    check(n_cancel == 1 && view().kind == OV_RESULT && !strcmp(view().title, "CANCELLED") && hold_on == 0, "B while downloading: cancelled, the link back");
    reset(); snprintf(answer, sizeof answer, "%s", OFFER); press(1); run(2000); dl_ret = CF_OTA_E_HASH; run(300);
    check(view().kind == OV_RESULT && !strcmp(view().title, "DOWNLOAD  FAILED") && strstr(view().note, "CHECK"), "a download that does not hash right: DOWNLOAD FAILED");

    printf("the FPGA part\n");
    check(cf_ota_parse(OFFER_BOTH, &o, &why) == 1 && o.size == 1756240 && o.fsize == 768397 && !strcmp(o.fversion, "18.31") && !strcmp(o.fsha256, FSHA) && strstr(o.furl, FSHA), "app + FPGA");
    check(cf_ota_parse(OFFER_FPGA, &o, &why) == 1 && o.size == 0 && o.fsize == 768397, "FPGA only");
    check(cf_ota_parse("{\"update\": true, \"version\": \"1.1.1\"}", &o, &why) == -1 && !strcmp(why, "empty"), "an update with no part is refused");
    { const char *bad[] = {
          "{\"update\": true, \"version\": \"1.1.1\", \"fpga_version\": \"18.31\", \"fpga_size\": 100, \"fpga_sha256\": \"" FSHA "\", \"fpga_url\": \"http://dl.shinyrelay.com/gbnet/v1/fw/" FSHA ".bin\"}",
          "{\"update\": true, \"version\": \"1.1.1\", \"fpga_version\": \"18.31\", \"fpga_size\": 2000000, \"fpga_sha256\": \"" FSHA "\", \"fpga_url\": \"http://dl.shinyrelay.com/gbnet/v1/fw/" FSHA ".bin\"}",
          "{\"update\": true, \"version\": \"1.1.1\", \"fpga_version\": \"18;30\", \"fpga_size\": 768397, \"fpga_sha256\": \"" FSHA "\", \"fpga_url\": \"http://dl.shinyrelay.com/gbnet/v1/fw/" FSHA ".bin\"}",
          "{\"update\": true, \"version\": \"1.1.1\", \"fpga_version\": \"18.31\", \"fpga_size\": 768397, \"fpga_sha256\": \"" FSHA "\", \"fpga_url\": \"http://dl.shinyrelay.com/gbnet/v1/fw/" SHA ".bin\"}" };
      for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) { char w[40]; snprintf(w, sizeof w, "FPGA part refused #%u", i); check(cf_ota_parse(bad[i], &o, &why) == -1, w); } }
    reset(); fl_ret = "full";
    press(1); run(1000);
    check(strstr(last_body, "\"fl\":\"full\"") != NULL, "the check says the FPGA boot flash's layout");
    reset();
    snprintf(answer, sizeof answer, "%s", OFFER_FPGA);
    press(1); run(1000);
    check(n_dl == 1 && dl_offer.fsize == 768397 && dl_offer.size == 0, "an FPGA-only update downloads");
    dl_ret = 0; run(300);
    check(view().kind == OV_READY, "READY");
    install_ret = 0; install_pending = 3; press(1); run(100);
    check(view().kind == OV_BUSY && !strcmp(view().title, "INSTALLING"), "while the FPGA header is written: INSTALLING (an FPGA-only update does not restart)");
    run(600);
    check(n_install == 4 && inst_offer.fsize == 768397 && view().kind == OV_RESULT && !strcmp(view().title, "UPDATED") && strstr(view().note, "OFF  AND"), "FPGA only installed: UPDATED, TURN IT OFF AND ON");
    reset();
    snprintf(answer, sizeof answer, "%s", OFFER_BOTH);
    press(1); run(1000); dl_ret = 0; run(300); press(1); run(300);
    check(n_install == 1 && inst_offer.size && inst_offer.fsize && !strcmp(view().title, "UPDATE  FAILED"), "app + FPGA: an install that returns failed (a real one restarts)");

    printf("a call held for the heap is asked again\n"); reset();
    snprintf(answer, sizeof answer, "%s", OFFER_FPGA);
    ans_code = 401;
    press(1); run(300);                                   /* the check: 401 (this server does not know the unit) */
    ans_code = 200; api_busy = 1; run(3000);              /* the register call cannot start yet (heap) */
    check(view().kind == OV_BUSY && n_regs == 0, "while the call is held: still CHECKING, no failure");
    api_busy = 0; run(2000);
    check(n_regs == 1 && n_checks == 2 && n_dl == 1, "then it registers, checks again and downloads");
    reset(); api_busy = 1;
    press(1); run(CF_OTA_API_MS + 2000);
    check(view().kind == OV_RESULT && strstr(view().note, "CONNECTION"), "held past the call deadline: NO CONNECTION");

    printf("the boot's notices\n"); reset();
    cf_ota_notice(&C, 1, "1.1.1"); check(view().kind == OV_RESULT && !strcmp(view().title, "UPDATED") && !strcmp(view().version, "1.1.1"), "UPDATED 1.1.1");
    press(1); check(view().kind == OV_MENU, "A: away");
    cf_ota_notice(&C, 2, "1.1.0"); check(!strcmp(view().title, "UPDATE  DID  NOT  START") && strstr(view().note, "OLD"), "a rollback: THE OLD VERSION IS BACK");
    press(1);
    cf_ota_notice(&C, 3, "1.1.1"); check(!strcmp(view().title, "UPDATED") && strstr(view().note, "OFF  AND"), "the app runs, its FPGA from the next power-on: TURN IT OFF AND ON");

    printf("1.1.12: UPDATE is official only, CUSTOM is its own channel\n"); reset();
#define CUSTOM "{\"update\": true, \"version\": \"1.1.1-cfw.ab12\", \"name\": \"MY  BUILD\", \"size\": 1500000, \"sha256\": \"" SHA "\", \"url\": \"http://dl.shinyrelay.com/gbnet/v1/fw/" SHA ".bin\"}"
    check(cf_ota_is_custom("1.1.11-cfw.75db") && cf_ota_is_custom("1.10.12-cfw.ab1") && !cf_ota_is_custom("1.1.12") && !cf_ota_is_custom("1.1.1-test2"), "what a custom version is");
    check(cf_ota_parse(CUSTOM, &o, &why) == 1 && !strcmp(o.name, "MY  BUILD"), "an offer's name");
    check(cf_ota_parse("{\"update\": true, \"version\": \"1.1.1-cfw.ab12\", \"name\": \"<b>x\", \"size\": 1500000, \"sha256\": \"" SHA "\", \"url\": \"http://dl.shinyrelay.com/gbnet/v1/fw/" SHA ".bin\"}", &o, &why) == 1 && !o.name[0], "a name with other characters is not shown");
    press(1); run(2000);
    check(strstr(last_body, "\"split\":1") && !strcmp(last_path, "/link_device/fw/check"), "the update check says split:1");
    press(1);
    snprintf(answer, sizeof answer, "%s", CUSTOM); press(1); run(2000);
    check(n_dl == 0 && view().kind == OV_RESULT && !strcmp(view().title, "CHECK  FAILED"), "UPDATE refuses a custom build");
    press(1);
    registered = 0; { const int r0 = n_regs; cf_ota_custom(&C); run(2000);
    check(!strcmp(last_path, "/link_device/fw/custom") && !strstr(last_body, "split") && n_regs == r0, "CUSTOM asks its own route, without registering"); }
    check(view().kind == OV_OFFER && view().custom && !strcmp(view().name, "MY  BUILD") && !strcmp(view().version, "1.1.1-cfw.ab12") && n_dl == 0 && hold_on == 1, "BUILD WAITING: shown, nothing downloaded");
    press(1); check(n_dl == 1 && view().kind == OV_DL && view().custom, "A downloads it");
    dl_ret = 0; run(300); check(view().kind == OV_READY && view().custom && hold_on == 0, "BUILD READY");
    press(0); registered = 1;
    snprintf(answer, sizeof answer, "{\"update\": false}"); press(1); run(2000);
    check(n_forget == 1 && n_install == 0 && !strcmp(view().title, "UP  TO  DATE"), "A on UPDATE with a custom build waiting: dropped, never installed");
    press(1);
    reset(); snprintf(answer, sizeof answer, "%s", OFFER); cf_ota_custom(&C); run(2000);
    check(n_dl == 0 && !strcmp(view().title, "CHECK  FAILED"), "CUSTOM refuses a release");
    press(1); ans_code = 401; cf_ota_custom(&C); run(2000);
    check(!strcmp(view().title, "NOT  LINKED") && registered == 1 && n_regs == 0, "CUSTOM refused by the server: NOT LINKED, the registration kept");
    press(1); ans_code = 200; snprintf(answer, sizeof answer, "%s", CUSTOM); cf_ota_custom(&C); run(2000); run(CF_OTA_OFFER_MS + 200);
    check(view().kind == OV_RESULT && hold_on == 0 && n_dl == 0, "BUILD WAITING left alone: ends, the link back");
    press(1); cf_ota_notice(&C, 1, "1.1.1-cfw.ab12"); check(!strcmp(view().title, "BUILD  INSTALLED") && view().custom, "a custom build that confirmed itself");

    printf("cf_ota_test: %d ok, %d failed\n", ok_n, bad_n);
    return bad_n ? 1 : 0;
}
