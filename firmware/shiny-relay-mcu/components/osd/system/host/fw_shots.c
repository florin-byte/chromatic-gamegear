/* fw_shots.c (1.1.12): SYSTEM INFO (status/fw.c) as the unit draws it, on the Mac, with the longest versions a unit
 * can run: an official release and a custom build ("1.1.12-cfw.8104", 15 characters, and the widest digits).
 * The real page over recorded LVGL calls. For every version both views (FW, and MCU / FPGA after A) are printed for
 * fw_fit.py, which draws every label with the unit's font and fails when a pixel of it falls outside the box that
 * cuts it, outside the panel, or on or right beside the picture of the Chromatic. Checked here: the MCU row holds "v" and the WHOLE
 * version (a 16 byte field once cut a custom version at 15 characters); the FPGA row is the version only, with the
 * FPGA's debug bit set or not (every Shiny Relay FPGA sets it; the page once added " (dbg)"); the rows are MCU:, the
 * version, FPGA from the top. Exit 1 when a rule fails.
 *   label|name|x|y|clip x0|clip y0|clip x1|clip y1|white|text     img|x|y|w|h     (panel pixels, x1 / y1 exclusive) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "osd_shared.h"
#include "fw.h"

enum { kScreenW = 160, kScreenH = 144, kLabelH = 8 };
static lv_style_t W = {1}, G = {0};
lv_style_t *OSD_GetStyleTextWhite(void) { return &W; }
lv_style_t *OSD_GetStyleTextGrey(void) { return &G; }
const lv_img_dsc_t img_chromatic = { 20, 31 };
static lv_obj_t *objs[32]; static int nobj;
static lv_obj_t *make(lv_obj_t *p, int kind) { lv_obj_t *o = calloc(1, sizeof *o); o->parent = p; o->kind = kind; objs[nobj++] = o; return o; }
lv_obj_t *lv_label_create(lv_obj_t *p) { return make(p, 'L'); }
lv_obj_t *lv_obj_create(lv_obj_t *p) { return make(p, 'B'); }
lv_obj_t *lv_img_create(lv_obj_t *p) { return make(p, 'I'); }
void lv_img_set_src(lv_obj_t *o, const void *src) { const lv_img_dsc_t *d = src; o->img = src; o->w = d->w; o->h = d->h; }
void lv_obj_remove_style_all(lv_obj_t *o) { (void)o; }
void lv_obj_set_size(lv_obj_t *o, int w, int h) { o->w = w; o->h = h; }
void lv_obj_align(lv_obj_t *o, int a, int x, int y) { o->align = a; o->x = x; o->y = y; }
void lv_obj_add_style(lv_obj_t *o, lv_style_t *s, int sel) { (void)sel; o->style = s; }
void lv_label_set_text(lv_obj_t *o, const char *t) { snprintf(o->text, sizeof o->text, "%s", t); }
bool lv_obj_is_valid(const lv_obj_t *o) { for (int i = 0; i < nobj; i++) if (objs[i] == o) return true; return false; }
void lv_obj_del(lv_obj_t *o)
{
    for (int i = 0; i < nobj; ) if (objs[i]->parent == o) lv_obj_del(objs[i]), i = 0; else i++;   /* LVGL deletes the children too */
    for (int i = 0; i < nobj; i++) if (objs[i] == o) { objs[i] = objs[--nobj]; break; }
    free(o);
}

static const char *ver = "";
const char *FwHost_AppVersion(void) { return ver; }

static lv_obj_t screen = { .w = kScreenW, .h = kScreenH };
/* an object's box on the panel (a label's size is its text's: the checker measures that) */
static void box(const lv_obj_t *o, int *x0, int *y0, int *x1, int *y1)
{
    if (o == &screen || o == NULL) { *x0 = 0; *y0 = 0; *x1 = kScreenW; *y1 = kScreenH; return; }
    int px0, py0, px1, py1; box(o->parent, &px0, &py0, &px1, &py1);
    if (o->align == LV_ALIGN_BOTTOM_RIGHT) { *x1 = px1 + o->x; *y1 = py1 + o->y; *x0 = *x1 - o->w; *y0 = *y1 - o->h; }
    else { *x0 = px0 + o->x; *y0 = py0 + o->y; *x1 = *x0 + o->w; *y1 = *y0 + o->h; }
}
/* what cuts an object: every parent's box (LVGL does not draw a child outside its parent) */
static void clip(const lv_obj_t *o, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = 0; *y0 = 0; *x1 = kScreenW; *y1 = kScreenH;
    for (const lv_obj_t *p = o->parent; p && p != &screen; p = p->parent) {
        int a, b, c, d; box(p, &a, &b, &c, &d);
        if (a > *x0) *x0 = a;
        if (b > *y0) *y0 = b;
        if (c < *x1) *x1 = c;
        if (d < *y1) *y1 = d;
    }
}
static int ok_n, bad_n;
static void check(int c, const char *what, const char *v) { if (c) ok_n++; else { bad_n++; fprintf(stderr, "  FAIL %s (%s)\n", what, v); } }
static const lv_obj_t *label(const char *t) { for (int i = 0; i < nobj; i++) if (objs[i]->kind == 'L' && !strcmp(objs[i]->text, t)) return objs[i]; return NULL; }
static int has_text(const char *part) { for (int i = 0; i < nobj; i++) if (objs[i]->kind == 'L' && strstr(objs[i]->text, part)) return 1; return 0; }
static int top(const lv_obj_t *o) { int x0, y0, x1, y1; if (!o) return -1; box(o, &x0, &y0, &x1, &y1); return y0; }
static void shot(const char *name)
{
    Firmware_Draw(&screen);
    printf("page|%s|%s\n", name, ver);
    for (int i = 0; i < nobj; i++) {
        const lv_obj_t *o = objs[i]; int x0, y0, x1, y1, a, b, c, d;
        box(o, &x0, &y0, &x1, &y1);
        if (o->kind == 'I') printf("img|%d|%d|%d|%d\n", x0, y0, o->w, o->h);
        if (o->kind != 'L') continue;
        clip(o, &a, &b, &c, &d);
        printf("label|%d|%d|%d|%d|%d|%d|%d|%s\n", x0, y0, a, b, c, d, o->style == &W, o->text);
    }
}

int main(void)
{
    /* the newest release; the build the owner's unit showed as "v1.1.11-cfw.8"; the same on 1.1.12; the widest digits
     * in a release and in a custom build's 15 characters; a two digit release with 3 hex left */
    static const char *const vers[] = { "1.1.12", "1.1.11-cfw.8104", "1.1.12-cfw.8104", "88.88.88", "8.8.88-cfw.8888", "0.0.00-cfw.dddd", "8.88.88-cfw.888" };
    static const char *const names[] = { "release", "custom_1_1_11", "custom_1_1_12", "release_widest", "custom_widest", "custom_widest_hex", "custom_3_hex" };
    for (size_t i = 0; i < sizeof vers / sizeof vers[0]; i++) {
        char want[40], name[48];
        ver = vers[i];
        Firmware_Initialize();
        Firmware_SetFPGAVersion(18, 33, i != 0);   /* units report 18.33 with the debug bit set; the first page without it */
        snprintf(name, sizeof name, "info_%s_fw", names[i]); shot(name);
        check(label("FW:") && label("v4.6") && !label("MCU:"), "the first view: FW and the Chromatic's version", ver);
        Firmware_OnButton(kButton_A, kButtonState_Pressed, NULL);
        snprintf(name, sizeof name, "info_%s_mcu", names[i]); shot(name);
        snprintf(want, sizeof want, "v%s", ver);
        check(label("MCU:") && label("FPGA:") && label("v18.33"), "A: MCU and FPGA, the FPGA's version alone", ver);
        check(!has_text("dbg") && !has_text("("), "the debug bit adds nothing to the page", ver);
        check(label(want) != NULL, "the MCU row holds v and the whole version, not a cut one", ver);
        check(top(label("MCU:")) >= 0 && top(label("MCU:")) < top(label(want)) && top(label(want)) < top(label("FPGA:"))
              && top(label("FPGA:")) == top(label("v18.33")), "rows from the top: MCU:, the version, FPGA", ver);
        Firmware_OnButton(kButton_B, kButtonState_Pressed, NULL);   /* back to the first view for the next version */
        Firmware_OnTransition(NULL);
        check(nobj == 0, "leaving the page deletes everything it made", ver);
    }
    fprintf(stderr, "fw_shots: %d ok, %d failed\n", ok_n, bad_n);
    return bad_n ? 1 : 0;
}
