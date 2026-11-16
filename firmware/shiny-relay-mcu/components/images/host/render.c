// render.c (lane fwshrink1): LVGL 8.4 with the firmware's own config (its sdkconfig.h: 16-bit colour, chroma key FF00FF,
// no image cache, the same widgets), a 160 x 144 direct-mode full-refresh display as main.c sets it up. Each picture is
// drawn as the original converter output (orig_<name>) and as the indexed form (<name>) on a chroma screen and on a
// gradient, at three places (whole, clipped top-left, clipped bottom-right); the two frame buffers must be the same bytes.
#include "lvgl.h"
#include <stdio.h>
#include <string.h>
#define W 160
#define H 144
static lv_color_t buf[W * H], shot[W * H];
static void flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *c) { (void)a; (void)c; memcpy(shot, d->draw_buf->buf1, sizeof shot); lv_disp_flush_ready(d); }
#define IMGS X(icon_bat_large_g) X(icon_bat_large_r) X(icon_bat_large_y) X(icon_charging) X(img_a_right) X(img_arrow_down) X(img_arrow_up) \
  X(img_b_left) X(img_brightness) X(img_chromatic) X(img_chromatic_eyebrow) X(img_dot_grey) X(img_dot_white) X(img_option_dis) \
  X(img_option_en) X(img_toggle_off) X(img_toggle_on) X(menu_controls) X(menu_display) X(menu_palette) X(menu_palette_gbc) X(menu_status) X(menu_system)
#define X(n) extern const lv_img_dsc_t n, orig_##n;
IMGS
#undef X
static void draw(const lv_img_dsc_t *d, int bg, int x, int y, lv_color_t *out) {
    lv_obj_t *s = lv_scr_act();
    lv_obj_clean(s);
    lv_obj_set_style_bg_color(s, lv_color_hex(0xFF00FF), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    if (bg) {
        lv_obj_t *g = lv_obj_create(s);
        lv_obj_set_pos(g, 0, 0); lv_obj_set_size(g, W, H);
        lv_obj_set_style_radius(g, 0, 0); lv_obj_set_style_border_width(g, 0, 0); lv_obj_set_style_pad_all(g, 0, 0);
        lv_obj_set_style_bg_color(g, lv_color_hex(0xFF2000), 0); lv_obj_set_style_bg_grad_color(g, lv_color_hex(0x0040FF), 0);
        lv_obj_set_style_bg_grad_dir(g, bg == 1 ? LV_GRAD_DIR_VER : LV_GRAD_DIR_HOR, 0);
    }
    lv_obj_t *i = lv_img_create(s);
    lv_img_set_src(i, d);
    lv_obj_set_pos(i, x, y);
    lv_obj_invalidate(s);
    lv_refr_now(NULL);
    memcpy(out, shot, sizeof shot);
}
int main(void) {
    lv_init();
    static lv_disp_draw_buf_t db; lv_disp_draw_buf_init(&db, buf, NULL, W * H);
    static lv_disp_drv_t dd; lv_disp_drv_init(&dd);
    dd.hor_res = W; dd.ver_res = H; dd.direct_mode = 1; dd.full_refresh = 1; dd.flush_cb = flush; dd.draw_buf = &db;
    lv_disp_drv_register(&dd);
    static lv_color_t a[W * H], b[W * H];
    int bad = 0, runs = 0;
    const int pos[3][2] = {{3, 5}, {-7, -4}, {W - 30, H - 20}};
#define X(n) for (int bg = 0; bg < 3; bg++) for (int p = 0; p < 3; p++) { \
        draw(&orig_##n, bg, pos[p][0], pos[p][1], a); draw(&n, bg, pos[p][0], pos[p][1], b); runs++; \
        int d = 0; for (int k = 0; k < W * H; k++) d += a[k].full != b[k].full; \
        if (d) { bad++; printf("%s bg %d pos %d: %d pixels differ\n", #n, bg, p, d); } } \
    printf("%-22s cf %d -> %d (%u -> %u bytes)\n", #n, orig_##n.header.cf, n.header.cf, (unsigned)orig_##n.data_size, (unsigned)n.data_size);
    IMGS
#undef X
    /* the check itself: two different pictures must differ, and a menu picture must change the frame at all */
    draw(&orig_menu_status, 1, 3, 5, a); draw(&menu_system, 1, 3, 5, b);
    int s1 = 0; for (int k = 0; k < W * H; k++) s1 += a[k].full != b[k].full;
    draw(&orig_menu_status, 1, 3, 5, a); draw(&img_dot_grey, 1, 300, 300, b);
    int s2 = 0; for (int k = 0; k < W * H; k++) s2 += a[k].full != b[k].full;
    printf("sanity: status vs system %d pixels differ, status vs nothing %d\n", s1, s2);
    if (!s1 || !s2) bad++;
    printf("render: %d draws compared, %d differ\n", runs, bad);
    return bad != 0;
}
