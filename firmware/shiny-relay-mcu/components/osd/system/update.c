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
    kTextLen = kUpdateTextLen,
    kX_px = 77,
    kY_px = 46,
    kRow_px = 9,
    kRows = kUpdateRows,
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

// The rows of a flow the update machine shows (a check, a download, READY, a result...), for SYSTEM > UPDATE and
// SYSTEM > CUSTOM alike (1.1.12: one machine, two pages; v->Custom says a custom build is what it is about).
// false: the machine is idle (kNwFv_Menu): the page draws its own menu. S: the caller's text space.
bool Update_FlowRows(const NwFwView_t *v, char S[4][kUpdateTextLen + 16], const char *R[kUpdateRows], bool W[kUpdateRows])
{
    char *const b = S[0], *const r1 = S[1], *const r2 = S[2];
    for (int i = 0; i < kUpdateRows; i++) { R[i] = ""; W[i] = false; }
    {
        const char *bar = strchr(v->Note, '|');
        snprintf(r1, kUpdateTextLen, "%.*s", bar ? (int)(bar - v->Note) : (int)strlen(v->Note), v->Note);
        snprintf(r2, kUpdateTextLen, "%s", bar ? bar + 1 : "");
    }
    b[0] = 0;
    switch (v->Kind)
    {
    case kNwFv_Busy:
        R[0] = v->Title; R[2] = "THE  LINK  PAUSES"; R[3] = "UNTIL  IT  IS  DONE"; R[6] = "B:  CANCEL";
        W[0] = W[6] = true;
        return true;
    case kNwFv_Offer:
        R[0] = "BUILD  WAITING"; R[1] = v->Name[0] ? v->Name : "CUSTOM  BUILD"; R[2] = v->Version;
        R[3] = "NOT  AN  OFFICIAL"; R[4] = "RELEASE"; R[5] = "A:  DOWNLOAD"; R[6] = "B:  BACK";
        W[0] = W[1] = W[5] = W[6] = true;
        return true;
    case kNwFv_Download:
        snprintf(b, kUpdateTextLen, "%d%%", v->Pct);
        R[0] = "DOWNLOADING"; R[1] = v->Version; R[2] = b; R[3] = "KEEP  IT  ON  AND"; R[4] = "NEAR  WI-FI"; R[6] = "B:  CANCEL";
        W[0] = W[2] = W[6] = true;
        return true;
    case kNwFv_Ready:
        R[0] = v->Custom ? "BUILD  READY" : "UPDATE  READY"; R[1] = v->Version; R[2] = "IT  RESTARTS  THE"; R[3] = "CHROMATIC";
        R[5] = "A:  INSTALL"; R[6] = "B:  LATER";
        W[0] = W[1] = W[5] = W[6] = true;
        return true;
    case kNwFv_Switch:
        R[0] = "INSTALLING"; R[1] = "RESTARTING";
        W[0] = true;
        return true;
    case kNwFv_Fpga:
        if (v->Pct >= 0) snprintf(b, kUpdateTextLen, "%d%%", v->Pct);
        R[0] = v->Title; R[2] = r1; R[3] = r2; R[5] = b;
        W[0] = W[5] = true;
        return true;
    case kNwFv_Result:
        R[0] = v->Title; R[1] = r1; R[2] = r2; R[3] = v->Version; R[5] = "A:  OK";
        W[0] = W[5] = true;
        return true;
    default:
        return false;
    }
}

OSD_Result_t Update_Draw(void *arg)
{
    if (arg == NULL) return kOSD_Result_Err_NullDataPtr;
    lv_obj_t *const pScreen = (lv_obj_t *const)arg;
    NwFwView_t v;
    NW_FwView(&v);
    char S[4][kUpdateTextLen + 16];
    const char *R[kRows];
    bool W[kRows];
    if (Update_FlowRows(&v, S, R, W)) { Rows(pScreen, R, W); return kOSD_Result_Ok; }
    char a[kTextLen + 16], b[kTextLen + 16];
    snprintf(a, sizeof a, "VERSION  %s", v.Running);
    // (1.1.12: a custom build waiting in the slot is SYSTEM > CUSTOM's, not shown here)
    if (v.ReadyWaits && !v.Custom)
    {
        snprintf(b, sizeof b, "%s  WAITS", v.Version);
        const char *R2[kRows] = { "SHINY  RELAY", a, "AN  UPDATE", b, "", "A:  SEE  IT", "" };
        const bool W2[kRows] = { true, false, false, false, false, true, false };
        Rows(pScreen, R2, W2);
        return kOSD_Result_Ok;
    }
    const char *R2[kRows] = { "SHINY  RELAY", a, "OFFICIAL  UPDATES", "OVER  WI-FI", "", "A:  CHECK  FOR", "UPDATE" };
    const bool W2[kRows] = { true, false, false, false, false, true, true };
    Rows(pScreen, R2, W2);
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
