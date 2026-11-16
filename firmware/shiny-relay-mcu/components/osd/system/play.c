#include "play.h"

#include "menu_mgr.h"
#include "nw_dev.h"
#include "osd_shared.h"
#include "lvgl.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A game played with no cartridge, as it loads and how its start ended (1.1.0, FPGA 18.30). The RAM cart manager
// (nw/cartflash/cf_rc_dev.c) owns the game: NW_PlayView / NW_PlayButton. Lane ui110dev: SYSTEM > PLAY is gone (its list
// is part of SYSTEM > GAMES, games.c); GAMES draws this page while a game loads, for its result and for an FPGA that
// cannot play it. Same layout as the other SYSTEM pages (x 77, y 46, 9 px a row, seven rows).
enum {
    kTextLen = 24,
    kX_px = 77,
    kY_px = 46,
    kRow_px = 9,
    kRows = 7,
};

typedef struct {
    lv_obj_t *pLabel[kRows];
    uint8_t White;      // bit i: row i is drawn white
    uint8_t Styled;     // bit i: row i has its style
} Play_t;

static Play_t _Ctx;

static void Line(lv_obj_t *pScreen, int i, bool White, const char *Text)
{
    const uint8_t bit = (uint8_t)(1u << i);
    if (_Ctx.pLabel[i] == NULL)
    {
        _Ctx.pLabel[i] = lv_label_create(pScreen);
        lv_obj_align(_Ctx.pLabel[i], LV_ALIGN_TOP_LEFT, kX_px, kY_px + kRow_px * i);
        _Ctx.Styled &= (uint8_t)~bit;
        lv_obj_move_foreground(_Ctx.pLabel[i]);
    }
    if (!(_Ctx.Styled & bit) || !(_Ctx.White & bit) != !White)
    {
        if (_Ctx.Styled & bit) lv_obj_remove_style(_Ctx.pLabel[i], (_Ctx.White & bit) ? OSD_GetStyleTextWhite() : OSD_GetStyleTextGrey(), 0);
        lv_obj_add_style(_Ctx.pLabel[i], White ? OSD_GetStyleTextWhite() : OSD_GetStyleTextGrey(), 0);
        _Ctx.Styled |= bit;
        _Ctx.White = White ? (uint8_t)(_Ctx.White | bit) : (uint8_t)(_Ctx.White & ~bit);
    }
    const char *cur = lv_label_get_text(_Ctx.pLabel[i]);
    if (cur == NULL || strcmp(cur, Text) != 0) lv_label_set_text(_Ctx.pLabel[i], Text);
}

static void Rows(lv_obj_t *pScreen, const char *const R[kRows], const bool W[kRows])
{
    for (int i = 0; i < kRows; i++) Line(pScreen, i, W[i], R[i] ? R[i] : "");
}

OSD_Result_t Play_Draw(void *arg)
{
    if (arg == NULL) return kOSD_Result_Err_NullDataPtr;
    lv_obj_t *const pScreen = (lv_obj_t *const)arg;
    NwPlayView_t v;
    NW_PlayView(&v);
    char b[kTextLen + 16], r1[kTextLen] = "", r2[kTextLen] = "";
    {
        const char *bar = strchr(v.Note, '|');
        snprintf(r1, sizeof r1, "%.*s", bar ? (int)(bar - v.Note) : (int)strlen(v.Note), v.Note);
        snprintf(r2, sizeof r2, "%s", bar ? bar + 1 : "");
    }
    switch (v.Kind)
    {
    case kNwPv_Loading:
    {
        snprintf(b, sizeof b, "%d%%", v.Pct);
        const char *R[kRows] = { "LOADING", v.Title, b, "", "", "", "" };
        const bool W[kRows] = { true, false, true, false, false, false, false };
        Rows(pScreen, R, W);
        break;
    }
    case kNwPv_Playing:
    {
        const char *R[kRows] = { "PLAYING  NO  CART", v.Title, v.Note, "", "", "A:  BACK  TO  THE", "CARTRIDGE" };
        const bool W[kRows] = { true, true, false, false, false, true, true };
        Rows(pScreen, R, W);
        break;
    }
    case kNwPv_Result:
    {
        const char *R[kRows] = { v.Title[0] ? v.Title : "PLAY", r1, r2, "", "", "A:  OK", "" };
        const bool W[kRows] = { true, false, false, false, false, true, false };
        Rows(pScreen, R, W);
        break;
    }
    case kNwPv_NoFpga:
    {
        const char *R[kRows] = { "UPDATE  YOUR", "CHROMATIC  FIRST", "", "", "", "A:  OK", "" };
        const bool W[kRows] = { true, false, false, false, false, true, false };
        Rows(pScreen, R, W);
        break;
    }
    default:
    {
        const char *R[kRows] = { "", "", "", "", "", "", "" };
        const bool W[kRows] = { false, false, false, false, false, false, false };
        Rows(pScreen, R, W);
        break;
    }
    }
    return kOSD_Result_Ok;
}

OSD_Result_t Play_OnButton(const Button_t Button, const ButtonState_t State, void *arg)
{
    (void)arg;
    if (State != kButtonState_Pressed) return kOSD_Result_Ok;
    if (Button == kButton_A) NW_PlayButton(1);
    else if (Button == kButton_B) NW_PlayButton(0);
    return kOSD_Result_Ok;
}

OSD_Result_t Play_OnTransition(void *arg)
{
    (void)arg;
    for (int i = 0; i < kRows; i++)
    {
        if (_Ctx.pLabel[i] != NULL)
        {
            lv_obj_del(_Ctx.pLabel[i]);
            _Ctx.pLabel[i] = NULL;
        }
    }
    _Ctx.Styled = 0;
    return kOSD_Result_Ok;
}

#ifndef GAMES_HOST
// bench (`ppage`): the rows as drawn
static int cmd_ppage(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "show")) NW_OpenPage("GAMES");
    vTaskDelay(pdMS_TO_TICKS(argc >= 3 ? atoi(argv[2]) : 400));
    char rows[240];
    size_t k = 0;
    for (int i = 0; i < kRows && k < sizeof rows; i++)
    {
        const char *t = _Ctx.pLabel[i] ? lv_label_get_text(_Ctx.pLabel[i]) : "";
        k += (size_t)snprintf(rows + k, sizeof rows - k, "%s%s%s", i ? "|" : "", (_Ctx.White & (1u << i)) ? "!" : "", t ? t : "");
    }
    printf("ppage: e=0 rows %s\n", rows);
    return 0;
}
void Play_Register(void)
{
    const esp_console_cmd_t cmd = { .command = "ppage", .help = "a game loading / its result (bench): ppage [show] [MS]", .func = &cmd_ppage };
    (void)esp_console_cmd_register(&cmd);
}
#endif
