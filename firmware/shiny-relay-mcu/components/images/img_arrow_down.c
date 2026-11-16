// img_arrow_down: 5 x 3, LV_IMG_CF_INDEXED_1BIT (11 bytes: a 2-entry palette + 1 bit per pixel).
// Made by tools/lv_indexed.py from the LVGL converter's LV_IMG_CF_TRUE_COLOR output (30 bytes at 16 bit):
// pixel for pixel the same at LV_COLOR_DEPTH 16 (lane fwshrink1). The source picture is img_arrow_down.png.
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

#ifndef LV_ATTRIBUTE_IMG_IMG_ARROW_DOWN
#define LV_ATTRIBUTE_IMG_IMG_ARROW_DOWN
#endif

const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_IMG_ARROW_DOWN uint8_t img_arrow_down_map[] = {
  0x00, 0x00, 0x00, 0xff, 0xf8, 0xfc, 0xf8, 0xff, 0xf8, 0x70, 0x20,
};

const lv_img_dsc_t img_arrow_down = {
  .header.cf = LV_IMG_CF_INDEXED_1BIT,
  .header.always_zero = 0,
  .header.reserved = 0,
  .header.w = 5,
  .header.h = 3,
  .data_size = 11,
  .data = img_arrow_down_map,
};
