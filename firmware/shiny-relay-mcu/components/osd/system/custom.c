#include "custom.h"

#include "cwl_online.h"
#include "nw_dev.h"
#include "osd_shared.h"
#include "update.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// SYSTEM > CUSTOM (1.1.12): see custom.h. Same layout as SYSTEM > UPDATE (x 77, y 46, 9 px a row, seven rows);
// a flow of the update machine (a check, a download, READY, a result) is drawn by UPDATE's own rows
// (Update_FlowRows), so the two pages can never say different things about the same step.
//
//   not linked   A: LINK THIS CHROMATIC -> the code to type at SHINYRELAY.COM /LINK (B: CANCEL; on two rows:
//                one row is 5 px too wide for the panel, and the font's lower case reads badly)
//   linked       A: CHECK FOR BUILD -> BUILD WAITING name version (A: DOWNLOAD) -> BUILD READY (A: INSTALL)
//                on a custom build also B: BACK TO OFFICIAL (the official check, as SYSTEM > UPDATE's A)
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
} Custom_t;

static Custom_t _Ctx;   // (this page's own labels)

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

typedef enum { kLink_None, kLink_Starting, kLink_Code, kLink_Failed, kLink_Linked } Link_t;

static Link_t LinkState(void)
{
    const CwlPairState_t p = CwlOnline_PairState();
    if (p == kCwlPair_Starting) return kLink_Starting;
    if (p == kCwlPair_Waiting) return kLink_Code;
    if (p == kCwlPair_Failed) return kLink_Failed;
    return CwlOnline_Paired() ? kLink_Linked : kLink_None;
}

static bool RunsCustom(const NwFwView_t *v) { return strstr(v->Running, "-cfw.") != NULL; }

OSD_Result_t Custom_Draw(void *arg)
{
    if (arg == NULL) return kOSD_Result_Err_NullDataPtr;
    lv_obj_t *const pScreen = (lv_obj_t *const)arg;
    NwFwView_t v;
    NW_FwView(&v);
    char S[4][kUpdateTextLen + 16];
    const char *F[kRows];
    bool FW[kRows];
    if (Update_FlowRows(&v, S, F, FW)) { Rows(pScreen, F, FW); return kOSD_Result_Ok; }
    char now[kTextLen + 16], code[16] = "", url[4] = "";
    snprintf(now, sizeof now, "NOW  %s", v.Running);
    switch (LinkState())
    {
    case kLink_Starting:
    {
        const char *R[kRows] = { "GETTING  A  CODE...", "", "", "", "", "", "B:  CANCEL" };
        const bool W[kRows] = { true, false, false, false, false, false, true };
        Rows(pScreen, R, W);
        break;
    }
    case kLink_Code:
    {
        CwlOnline_PairCode(code, sizeof code, url, sizeof url);
        const char *R[kRows] = { "ON  THE  WEB  OPEN", "SHINYRELAY.COM", "/LINK", "SIGN  IN  AND  ENTER", code, "", "B:  CANCEL" };
        const bool W[kRows] = { false, true, true, false, true, false, true };
        Rows(pScreen, R, W);
        break;
    }
    case kLink_Failed:
    {
        const char *R[kRows] = { "LINKING  FAILED", CwlOnline_PairError(), "", "", "", "A:  TRY  AGAIN", "B:  BACK" };
        const bool W[kRows] = { true, false, false, false, false, true, true };
        Rows(pScreen, R, W);
        break;
    }
    case kLink_Linked:
    {
        const char *back = RunsCustom(&v) ? "B:  BACK  TO  OFFICIAL" : "";
        // the last four characters of this pairing's id: the website lists each Chromatic by them ("Chromatic 1B2C")
        char id[kTextLen] = "";
        {
            const char *d = CwlOnline_DeviceId();
            const size_t n = strlen(d);
            if (n >= 4) {
                snprintf(id, sizeof id, "ID  %s", d + n - 4);
                for (char *c = id + 4; *c; c++) if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 'a' + 'A');
            }
        }
        if (v.ReadyWaits && v.Custom)
        {
            const char *R[kRows] = { "CUSTOM  FIRMWARE", now, "A  BUILD  IS  READY", v.Version, id, "A:  SEE  IT", back };
            const bool W[kRows] = { true, false, false, false, false, true, true };
            Rows(pScreen, R, W);
            break;
        }
        const char *R[kRows] = { "CUSTOM  FIRMWARE", now, "SEND  A  BUILD  FROM", "SHINYRELAY.COM", id, "A:  CHECK  FOR  BUILD", back };
        const bool W[kRows] = { true, false, false, false, false, true, true };
        Rows(pScreen, R, W);
        break;
    }
    case kLink_None:
    default:
    {
        const char *R[kRows] = { "CUSTOM  FIRMWARE", "MAKE  YOUR  OWN  AT", "SHINYRELAY.COM", "", "NOT  LINKED  YET", "A:  LINK  THIS", "CHROMATIC" };
        const bool W[kRows] = { true, false, false, false, false, true, true };
        Rows(pScreen, R, W);
        break;
    }
    }
    return kOSD_Result_Ok;
}

OSD_Result_t Custom_OnButton(const Button_t Button, const ButtonState_t State, void *arg)
{
    (void)arg;
    if (State != kButtonState_Pressed) return kOSD_Result_Ok;
    if (Button != kButton_A && Button != kButton_B) return kOSD_Result_Ok;
    const bool A = Button == kButton_A;
    NwFwView_t v;
    NW_FwView(&v);
    // a flow is on the page (either channel's): its buttons are the update machine's
    if (v.Kind != kNwFv_Menu) { NW_FwButton(A ? 1 : 0); return kOSD_Result_Ok; }
    switch (LinkState())
    {
    case kLink_None:
        if (A) CwlOnline_PairStart();
        break;
    case kLink_Starting:
    case kLink_Code:
        if (!A) CwlOnline_PairCancel();
        break;
    case kLink_Failed:
        if (A) CwlOnline_PairStart(); else CwlOnline_PairCancel();
        break;
    case kLink_Linked:
        if (A) NW_FwCustom();                         // the build this account sent to this Chromatic
        else if (RunsCustom(&v)) NW_FwButton(1);      // back to official: the official check (UPDATE's A)
        break;
    default:
        break;
    }
    return kOSD_Result_Ok;
}

OSD_Result_t Custom_OnTransition(void *arg)
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

#ifdef ESP_PLATFORM
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
// bench console (as `gpage`): cpage [show|a|b] [MS], then the seven rows as drawn ("!" = white)
static int cmd_cpage(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "show")) NW_OpenPage("CUSTOM");
    if (argc >= 2 && !strcmp(argv[1], "a")) (void)Custom_OnButton(kButton_A, kButtonState_Pressed, NULL);
    if (argc >= 2 && !strcmp(argv[1], "b")) (void)Custom_OnButton(kButton_B, kButtonState_Pressed, NULL);
    vTaskDelay(pdMS_TO_TICKS(argc >= 3 ? atoi(argv[2]) : 400));
    printf("cpage: e=0 rows ");
    for (int i = 0; i < kRows; i++)
    {
        const char *t = _Ctx.pLabel[i] ? lv_label_get_text(_Ctx.pLabel[i]) : "";
        printf("%s%s%s", i ? "|" : "", (_Ctx.White & (1u << i)) ? "!" : "", t ? t : "");
    }
    printf("\n");
    return 0;
}
void Custom_Register(void)
{
    const esp_console_cmd_t cmd = { .command = "cpage", .help = "SYSTEM > CUSTOM (bench): cpage [show|a|b] [MS]", .func = &cmd_cpage };
    (void)esp_console_cmd_register(&cmd);
}
#endif
