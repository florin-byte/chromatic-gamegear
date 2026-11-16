#include "update.h"

#include "nw_dev.h"
#include "osd_shared.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// SYSTEM > UPDATE (1.1.0, owner 2026-10-20): Shiny Relay's own update over Wi-Fi. A checks; a newer
// version downloads into the app slot that is not running; UPDATE READY waits for A (INSTALL) or B
// (LATER: it stays there). The update machine (nw/cartflash/cf_ota.c) owns what the page shows
// (NW_FwView) and takes its buttons (NW_FwButton); this file only draws (as SYSTEM > DOWNLOAD).
// Same layout as SYSTEM > NETWORK's rows (x 77, y 46, 9 px a row, seven rows).
enum {
    kTextLen = 24,
    kX_px = 77,
    kY_px = 46,
    kRow_px = 9,
    kRows = 7,
};

// No text cache of its own (static RAM is short: the TLS gate): the label's own text is compared.
typedef struct {
    lv_obj_t *pLabel[kRows];
    uint8_t White;      // bit i: row i is drawn white
    uint8_t Styled;     // bit i: row i has its style
} Download_t;

static Download_t _Ctx;   // (this page's own labels)

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
    char t[kTextLen];
    snprintf(t, sizeof t, "%s", Text);   // at most kTextLen - 1 characters, as every OSD row
    const char *cur = lv_label_get_text(_Ctx.pLabel[i]);
    if (cur == NULL || strcmp(cur, t) != 0) lv_label_set_text(_Ctx.pLabel[i], t);
}

static void Rows(lv_obj_t *pScreen, const char *const R[kRows], const bool W[kRows])
{
    for (int i = 0; i < kRows; i++) Line(pScreen, i, W[i], R[i] ? R[i] : "");
}

OSD_Result_t Update_Draw(void *arg)
{
    if (arg == NULL) return kOSD_Result_Err_NullDataPtr;
    lv_obj_t *const pScreen = (lv_obj_t *const)arg;
    NwFwView_t v;
    NW_FwView(&v);
    char a[kTextLen + 16], b[kTextLen + 16], r1[kTextLen] = "", r2[kTextLen] = "";
    {
        const char *bar = strchr(v.Note, '|');
        snprintf(r1, sizeof r1, "%.*s", bar ? (int)(bar - v.Note) : (int)strlen(v.Note), v.Note);
        snprintf(r2, sizeof r2, "%s", bar ? bar + 1 : "");
    }
    snprintf(a, sizeof a, "VERSION  %s", v.Running);
    switch (v.Kind)
    {
    case kNwFv_Busy:
    {
        const char *R[kRows] = { v.Title, "", "THE  LINK  PAUSES", "UNTIL  IT  IS  DONE", "", "", "B:  CANCEL" };
        const bool W[kRows] = { true, false, false, false, false, false, true };
        Rows(pScreen, R, W);
        break;
    }
    case kNwFv_Download:
    {
        snprintf(b, sizeof b, "%d%%", v.Pct);
        const char *R[kRows] = { "DOWNLOADING", v.Version, b, "KEEP  IT  ON  AND", "NEAR  WI-FI", "", "B:  CANCEL" };
        const bool W[kRows] = { true, false, true, false, false, false, true };
        Rows(pScreen, R, W);
        break;
    }
    case kNwFv_Ready:
    {
        const char *R[kRows] = { "UPDATE  READY", v.Version, "IT  RESTARTS  THE", "CHROMATIC", "", "A:  INSTALL", "B:  LATER" };
        const bool W[kRows] = { true, true, false, false, false, true, true };
        Rows(pScreen, R, W);
        break;
    }
    case kNwFv_Switch:
    {
        const char *R[kRows] = { "INSTALLING", "RESTARTING", "", "", "", "", "" };
        const bool W[kRows] = { true, false, false, false, false, false, false };
        Rows(pScreen, R, W);
        break;
    }
    case kNwFv_Fpga:
    {
        if (v.Pct >= 0) snprintf(b, sizeof b, "%d%%", v.Pct); else b[0] = 0;
        const char *R[kRows] = { v.Title, "", r1, r2, "", b, "" };
        const bool W[kRows] = { true, false, false, false, false, true, false };
        Rows(pScreen, R, W);
        break;
    }
    case kNwFv_Result:
    {
        const char *R[kRows] = { v.Title, r1, r2, v.Version[0] ? v.Version : "", "", "A:  OK", "" };
        const bool W[kRows] = { true, false, false, false, false, true, false };
        Rows(pScreen, R, W);
        break;
    }
    case kNwFv_Menu:
    default:
    {
        if (v.ReadyWaits)
        {
            snprintf(b, sizeof b, "%s  WAITS", v.Version);
            const char *R[kRows] = { "SHINY  RELAY", a, "AN  UPDATE", b, "", "A:  SEE  IT", "" };
            const bool W[kRows] = { true, false, false, false, false, true, false };
            Rows(pScreen, R, W);
            break;
        }
        const char *R[kRows] = { "SHINY  RELAY", a, "UPDATE  OVER  WI-FI", "", "", "A:  CHECK  FOR", "UPDATE" };
        const bool W[kRows] = { true, false, false, false, false, true, true };
        Rows(pScreen, R, W);
        break;
    }
    }
    return kOSD_Result_Ok;
}

OSD_Result_t Update_OnButton(const Button_t Button, const ButtonState_t State, void *arg)
{
    (void)arg;
    if (State != kButtonState_Pressed) return kOSD_Result_Ok;
    if (Button == kButton_A) NW_FwButton(1);
    else if (Button == kButton_B) NW_FwButton(0);
    return kOSD_Result_Ok;
}

OSD_Result_t Update_OnTransition(void *arg)
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
