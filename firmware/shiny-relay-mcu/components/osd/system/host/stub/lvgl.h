/* host stub of the few LVGL calls SYSTEM > DOWNLOAD makes (dl_shots.c records them) */
#pragma once
typedef struct lv_obj { char text[64]; const void *style; int x, y; } lv_obj_t;
typedef struct lv_style { int white; } lv_style_t;
enum { LV_ALIGN_TOP_LEFT = 1 };
lv_obj_t *lv_label_create(lv_obj_t *parent);
void lv_obj_align(lv_obj_t *o, int align, int x, int y);
void lv_obj_move_foreground(lv_obj_t *o);
void lv_obj_add_style(lv_obj_t *o, lv_style_t *s, int sel);
void lv_obj_remove_style(lv_obj_t *o, lv_style_t *s, int sel);
void lv_label_set_text(lv_obj_t *o, const char *t);
char *lv_label_get_text(const lv_obj_t *o);
void lv_obj_del(lv_obj_t *o);
