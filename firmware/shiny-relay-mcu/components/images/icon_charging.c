// icon_charging: 9 x 9, LV_IMG_CF_INDEXED_2BIT (43 bytes: a 4-entry palette + 2 bit per pixel).
// Made by tools/lv_indexed.py from the LVGL converter's LV_IMG_CF_TRUE_COLOR_ALPHA output (243 bytes at 16 bit):
// pixel for pixel the same at LV_COLOR_DEPTH 16 (lane fwshrink1). The source picture is icon_charging.png.
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

#ifndef LV_ATTRIBUTE_IMG_ICON_CHARGING
#define LV_ATTRIBUTE_IMG_ICON_CHARGING
#endif

const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_ICON_CHARGING uint8_t icon_charging_map[] = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xf8, 0xfc, 0xf8, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x01, 0x90, 0x00, 0x06, 0x90,
  0x00, 0x1a, 0x95, 0x00, 0x6a, 0xaa, 0x40, 0x15, 0xa9, 0x00, 0x01, 0xa4, 0x00, 0x01, 0x90, 0x00, 0x00, 0x40, 0x00,
};

const lv_img_dsc_t icon_charging = {
  .header.cf = LV_IMG_CF_INDEXED_2BIT,
  .header.always_zero = 0,
  .header.reserved = 0,
  .header.w = 9,
  .header.h = 9,
  .data_size = 43,
  .data = icon_charging_map,
};
