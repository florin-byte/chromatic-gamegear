// img_a_right: 7 x 13, LV_IMG_CF_INDEXED_1BIT (21 bytes: a 2-entry palette + 1 bit per pixel).
// Made by tools/lv_indexed.py from the LVGL converter's LV_IMG_CF_TRUE_COLOR output (182 bytes at 16 bit):
// pixel for pixel the same at LV_COLOR_DEPTH 16 (lane fwshrink1). The source picture is img_a_right.png.
#ifdef __has_include
    #if __has_include("lvgl.h")
        #ifndef LV_LVGL_H_INCLUDE_SIMPLE
            #define LV_LVGL_H_INCLUDE_SIMPLE
        #endif
    #endif
#endif

#if defined(LV_LVGL_H_INCLUDE_SIMPLE)
    #include "lvgl.h"
#else
    #include "lvgl/lvgl.h"
#endif

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

#ifndef LV_ATTRIBUTE_IMG_IMG_A_RIGHT
#define LV_ATTRIBUTE_IMG_IMG_A_RIGHT
#endif

const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_IMG_A_RIGHT uint8_t img_a_right_map[] = {
  0x00, 0x00, 0x00, 0xff, 0xf8, 0xfc, 0xf8, 0xff, 0x80, 0xc0, 0xe0, 0xf0, 0x88, 0xac, 0x8e, 0xac, 0xa8, 0xf0, 0xe0, 0xc0, 0x80,
};

const lv_img_dsc_t img_a_right = {
  .header.cf = LV_IMG_CF_INDEXED_1BIT,
  .header.always_zero = 0,
  .header.reserved = 0,
  .header.w = 7,
  .header.h = 13,
  .data_size = 21,
  .data = img_a_right_map,
};
