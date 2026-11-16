/* host stub of osd_shared.h for games_shots.c */
#pragma once
#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>
typedef enum { kOSD_Result_Ok, kOSD_Result_Err_NullDataPtr } OSD_Result_t;
typedef enum { kButton_A, kButton_B, kButton_Up, kButton_Down, kButton_Left, kButton_Right, kButton_Start, kButton_Select } Button_t;
typedef enum { kButtonState_Pressed, kButtonState_Released } ButtonState_t;
typedef OSD_Result_t (*fnOSDButtonCb_t)(const Button_t, const ButtonState_t, void *);
lv_style_t *OSD_GetStyleTextWhite(void);
lv_style_t *OSD_GetStyleTextGrey(void);
