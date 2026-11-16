/* clock_shots.c (1.1.7): SYSTEM > CLOCK as the unit draws it, on the Mac. The real page (timeset.c) over a scripted clock
 * (cwl_time.h's calls); every screen's rows printed for osd_page.py, the page's rules checked (FAIL on stderr).
 * make clock-shots renders the PNGs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "osd_shared.h"
#include "menu_mgr.h"
#include "cwl_time.h"
#include "cwl_civil.h"
#include "../timeset.h"

static int pass, fail;
#define CHECK(c, ...) do { if (c) pass++; else { fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

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
static fnOSDButtonCb_t Cap;
void MenuMgr_SetCapture(fnOSDButtonCb_t fn) { Cap = fn; }

/* ---- the unit's clock */
static uint64_t now_t, last_t;
static int src;                       /* 0 none, 1 Wi-Fi, 3 by hand */
uint64_t CwlTime_Unix(void) { return now_t; }
bool CwlTime_FromWifi(void) { return src == 1 && now_t; }
bool CwlTime_ByHand(void) { return src == 3 && now_t; }
uint64_t CwlTime_Last(void) { return last_t; }
bool CwlTime_SetByHand(uint64_t t) { if (CwlTime_FromWifi() || t <= 1735689600ull) return false; now_t = t; src = 3; return true; }

static lv_obj_t screen;
static const char *Tabs = "PLAYER  #,LINK  CABLE,ONLINE,WI-FI,*CLOCK";
static void row(int r, char *out, size_t n)
{
    const lv_obj_t *o = NULL;
    for (int i = 0; i < nobj; i++) if (objs[i]->y == 46 + 9 * r && objs[i]->x == 77) o = objs[i];
    snprintf(out, n, "%s%s", o && o->style == &W ? "!" : "", o ? o->text : "");
}
static int has(const char *t) { char b[128]; for (int r = 0; r < 7; r++) { row(r, b, sizeof b); if (strstr(b, t)) return 1; } return 0; }
static void shot(const char *name)
{
    (void)TimeSet_Draw(&screen);
    printf("%s|%s", name, Tabs);
    char b[128];
    for (int r = 0; r < 7; r++) { row(r, b, sizeof b); printf("|%s", b); }
    printf("\n");
}
static void press(Button_t b)
{
    if (Cap) (void)Cap(b, kButtonState_Pressed, NULL); else (void)TimeSet_OnButton(b, kButtonState_Pressed, NULL);
    (void)TimeSet_Draw(&screen);
}

int main(void)
{
    /* no time, none ever known */
    (void)TimeSet_Draw(&screen);
    CHECK(has("NOT  SET") && has("NO  WI-FI  TIME") && has("A:  SET  BY  HAND"), "no time: says so and offers to set it");
    shot("c1_not_set");
    /* A: edit, starting from the default; set a time by hand */
    press(kButton_A);
    CHECK(Cap != NULL && has("SET  THE  CLOCK") && has("YEAR  <  2027  >"), "A opens the editor, the D-pad is captured, it starts at 2027-01-01 00:00");
    shot("c2_edit_year");
    press(kButton_Left);
    CHECK(has("YEAR  <  2026  >"), "left: year - 1");
    press(kButton_Left); press(kButton_Left);
    CHECK(has("YEAR  <  2099  >"), "left past 2025 wraps to 2099");
    press(kButton_Right);
    CHECK(has("YEAR  <  2025  >"), "right past 2099 wraps to 2025");
    press(kButton_Right);
    press(kButton_Down);                                         /* MONTH */
    for (int i = 0; i < 10; i++) press(kButton_Right);           /* 11 */
    press(kButton_Down);                                         /* DAY */
    for (int i = 0; i < 4; i++) press(kButton_Right);            /* 5 */
    press(kButton_Down);                                         /* HOUR */
    for (int i = 0; i < 14; i++) press(kButton_Right);
    press(kButton_Down);                                         /* MINUTE */
    for (int i = 0; i < 32; i++) press(kButton_Right);
    CHECK(has("YEAR  2026") && has("MONTH  11") && has("DAY  05") && has("HOUR  14") && has("MINUTE  <  32  >"), "the fields: 2026-11-05 14:32");
    shot("c3_edit_minute");
    press(kButton_Up); press(kButton_Up);                        /* DAY */
    for (int i = 0; i < 4; i++) press(kButton_Left);             /* 5 -> 1 */
    press(kButton_Left);
    CHECK(has("DAY  <  30  >"), "day 1 - 1 wraps to the month's last day (30)");
    press(kButton_B);
    CHECK(Cap == NULL && has("NOT  SET"), "B: back, nothing set, D-pad free");
    press(kButton_A);                                            /* the editor again (Last is unknown: the default) */
    for (int i = 0; i < 4; i++) press(kButton_Down);
    press(kButton_A);
    CHECK(Cap == NULL && now_t == (uint64_t)CwlCivil_Unix(2027, 1, 1, 0, 0, 0) && has("2027-01-01") && has("00:00:00  UTC") && has("SET  BY  HAND") && has("A:  CHANGE"), "A sets it: shown, from the hand, can be changed");
    shot("c4_by_hand");
    /* February and the leap year: 2028-02-29 + month change clamps */
    now_t = (uint64_t)CwlCivil_Unix(2028, 1, 31, 23, 59, 0);
    press(kButton_A);                                            /* starts from the time now */
    CHECK(has("YEAR  <  2028  >"), "it starts from the time now");
    press(kButton_Down); press(kButton_Right);                   /* month 2 */
    press(kButton_Down);
    CHECK(has("DAY  <  29  >"), "31 January 2028 + 1 month: the day is held to 29 (leap year)");
    press(kButton_Up); press(kButton_Up); press(kButton_Right);  /* year 2029 */
    press(kButton_Down); press(kButton_Down);
    CHECK(has("DAY  <  28  >"), "... and to 28 when the year changes to a common one");
    press(kButton_B);
    /* the last known time starts the editor when the unit has none */
    now_t = 0; src = 0; last_t = (uint64_t)CwlCivil_Unix(2026, 11, 4, 9, 15, 0);
    (void)TimeSet_OnTransition(NULL);
    press(kButton_A);
    CHECK(has("YEAR  <  2026  >") && has("MONTH  11") && has("DAY  04") && has("HOUR  09") && has("MINUTE  15"), "with no time: it starts from the last time the unit knew");
    shot("c5_edit_from_last");
    press(kButton_B);
    /* Wi-Fi: the time is shown, not editable */
    now_t = (uint64_t)CwlCivil_Unix(2026, 11, 5, 14, 32, 7); src = 1;
    (void)TimeSet_Draw(&screen);
    CHECK(has("2026-11-05") && has("14:32:07  UTC") && has("FROM  WI-FI") && !has("A:"), "Wi-Fi time: shown, no way to set it");
    shot("c6_wifi");
    press(kButton_A);
    CHECK(Cap == NULL && !has("SET  THE  CLOCK"), "A does nothing when Wi-Fi gave the time");
    /* the time arrives from Wi-Fi while editing: the editor lets go */
    now_t = 0; src = 0;
    (void)TimeSet_OnTransition(NULL);
    press(kButton_A);
    now_t = (uint64_t)CwlCivil_Unix(2026, 11, 5, 14, 40, 0); src = 1;
    press(kButton_Right);
    CHECK(Cap == NULL && has("FROM  WI-FI"), "Wi-Fi time arrives while editing: back to the page, nothing set by hand");
    /* a time set by hand is replaced by the Wi-Fi one */
    now_t = (uint64_t)CwlCivil_Unix(2026, 11, 5, 14, 32, 7); src = 3;
    (void)TimeSet_OnTransition(NULL);
    (void)TimeSet_Draw(&screen);
    CHECK(has("SET  BY  HAND"), "by hand: says so");
    CHECK(!CwlTime_SetByHand(100), "a time before 2025 is refused");
    /* leaving the page while editing lets the D-pad go */
    press(kButton_A);
    CHECK(Cap != NULL, "editing");
    (void)TimeSet_OnTransition(NULL);
    CHECK(Cap == NULL && nobj == 0, "leaving the page: the D-pad is free, no labels left");
    fprintf(stderr, "clock_shots: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
