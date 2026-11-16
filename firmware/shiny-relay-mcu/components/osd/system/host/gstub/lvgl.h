/* host stub of the LVGL calls SYSTEM > GAMES / GET GAMES make (games_shots.c records them) */
#pragma once
#include <stdint.h>
typedef struct lv_obj { char text[160]; const void *style; int x, y, w, h, mode, box; uint32_t rgb; } lv_obj_t;
typedef struct lv_style { int white; } lv_style_t;
enum { LV_ALIGN_TOP_LEFT = 1 };
enum { LV_LABEL_LONG_WRAP, LV_LABEL_LONG_DOT, LV_LABEL_LONG_SCROLL_CIRCULAR };
#define LV_SIZE_CONTENT 0x7FFF
lv_obj_t *lv_label_create(lv_obj_t *parent);
void lv_obj_align(lv_obj_t *o, int align, int x, int y);
void lv_obj_move_foreground(lv_obj_t *o);
void lv_obj_add_style(lv_obj_t *o, lv_style_t *s, int sel);
void lv_obj_remove_style(lv_obj_t *o, lv_style_t *s, int sel);
void lv_label_set_text(lv_obj_t *o, const char *t);
char *lv_label_get_text(const lv_obj_t *o);
void lv_obj_del(lv_obj_t *o);
void lv_label_set_long_mode(lv_obj_t *o, int m);
void lv_obj_set_size(lv_obj_t *o, int w, int h);
uint32_t lv_tick_get(void);
/* fr1: the library's background and lines */
typedef struct { uint32_t rgb; } lv_color_t;
enum { LV_OPA_COVER = 255 };
lv_obj_t *lv_obj_create(lv_obj_t *parent);
void lv_obj_remove_style_all(lv_obj_t *o);
void lv_obj_set_pos(lv_obj_t *o, int x, int y);
void lv_obj_set_style_bg_color(lv_obj_t *o, lv_color_t c, int sel);
void lv_obj_set_style_bg_opa(lv_obj_t *o, int opa, int sel);
lv_color_t lv_color_hex(uint32_t rgb);
