/* host stub of the LVGL calls SYSTEM INFO (status/fw.c) makes; fw_shots.c records them: every object keeps its
 * parent, its place and its size, so the checker knows where a label is drawn and what box cuts it */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
typedef struct lv_obj { struct lv_obj *parent; char text[64]; const void *style; const void *img; int align, x, y, w, h, kind; } lv_obj_t;
typedef struct lv_style { int white; } lv_style_t;
typedef struct { int x, y; } lv_point_t;
typedef struct { int w, h; } lv_img_dsc_t;
enum { LV_ALIGN_TOP_LEFT = 1, LV_ALIGN_BOTTOM_RIGHT = 2 };
enum { LV_PART_MAIN = 0 };
#define LV_IMG_DECLARE(n) extern const lv_img_dsc_t n
#define lv_snprintf snprintf
lv_obj_t *lv_label_create(lv_obj_t *parent);
lv_obj_t *lv_obj_create(lv_obj_t *parent);
lv_obj_t *lv_img_create(lv_obj_t *parent);
void lv_img_set_src(lv_obj_t *o, const void *src);
void lv_obj_remove_style_all(lv_obj_t *o);
void lv_obj_set_size(lv_obj_t *o, int w, int h);
void lv_obj_align(lv_obj_t *o, int align, int x, int y);
void lv_obj_add_style(lv_obj_t *o, lv_style_t *s, int sel);
void lv_label_set_text(lv_obj_t *o, const char *t);
bool lv_obj_is_valid(const lv_obj_t *o);
void lv_obj_del(lv_obj_t *o);
