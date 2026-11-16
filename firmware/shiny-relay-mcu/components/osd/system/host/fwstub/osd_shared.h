/* host stub of osd_shared.h for fw_shots.c */
#pragma once
#include "lvgl.h"
#define ARRAY_SIZE(x) (sizeof(x)/sizeof(x[0]))
typedef enum { kOSD_Result_Ok, kOSD_Result_Err_NullDataPtr, kOSD_Result_Err_CmdRegFailed } OSD_Result_t;
typedef enum { kButton_A, kButton_B, kButton_Up, kButton_Down } Button_t;
typedef enum { kButtonState_Pressed, kButtonState_Released } ButtonState_t;
lv_style_t *OSD_GetStyleTextWhite(void);
lv_style_t *OSD_GetStyleTextGrey(void);
