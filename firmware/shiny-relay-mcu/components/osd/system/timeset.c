#include "timeset.h"

#include "cwl_time.h"
#include "cwl_civil.h"
#include "menu_mgr.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// SYSTEM > CLOCK (1.1.7): the date and time the game clocks (Pokemon Gold / Silver / Crystal) are caught up by. Wi-Fi gives it
// at power-on; with no Wi-Fi it is set here by hand. A time from Wi-Fi is only shown (it wins; a time set by hand is replaced by
// it when it arrives). Shown and set in UTC, the one time the unit keeps whichever way it came, so a time set by hand and a
// Wi-Fi time agree and a game's clock only moves by the time that really passed.
//   main  the time and where it came from; A: set by hand (not when Wi-Fi gave it)
//   edit  UP / DOWN: the field, LEFT / RIGHT: its value, A: set, B: back (the D-pad is captured while editing)
// Same layout as SYSTEM > UPDATE (x 77, y 46, 9 px a row, seven rows).
enum {
    kTextLen = 24,
    kX_px = 77,
    kY_px = 46,
    kRow_px = 9,
    kRows = 7,
    kFields = 5,
    kYearMin = 2025,
    kYearMax = 2099,
};

typedef struct {
    lv_obj_t *pLabel[kRows];
    uint8_t White;
    uint8_t Styled;
    bool Editing;
    int Sel;
    int F[kFields];      // year, month, day, hour, minute
} TimeSet_t;

static TimeSet_t _Ctx;

static void Fill(int64_t t)
{
    int s;
    CwlCivil_Split(t, &_Ctx.F[0], &_Ctx.F[1], &_Ctx.F[2], &_Ctx.F[3], &_Ctx.F[4], &s);
}

static void ClampDay(void)
{
    const int dim = CwlCivil_DaysIn(_Ctx.F[0], _Ctx.F[1]);
    if (_Ctx.F[2] > dim) _Ctx.F[2] = dim;
}

static OSD_Result_t OnCapture(const Button_t Button, const ButtonState_t State, void *arg);

static void SetEditing(bool On)
{
    _Ctx.Editing = On;
    MenuMgr_SetCapture(On ? OnCapture : NULL);
}

static void Step(int Dir)
{
    static const int kLo[kFields] = { kYearMin, 1, 1, 0, 0 };
    int hi[kFields] = { kYearMax, 12, CwlCivil_DaysIn(_Ctx.F[0], _Ctx.F[1]), 23, 59 };
    int *v = &_Ctx.F[_Ctx.Sel];
    *v += Dir;
    if (*v > hi[_Ctx.Sel]) *v = kLo[_Ctx.Sel];
    if (*v < kLo[_Ctx.Sel]) *v = hi[_Ctx.Sel];
    ClampDay();
}

static void Begin(void)
{
    // where it starts: the time now (set by hand before), else the last one the unit knew, else a day of this year
    uint64_t t = CwlTime_Unix();
    if (!t) t = CwlTime_Last();
    if (!t) t = 1798761600ull;   // 2027-01-01 00:00 (before the unit has ever known the time)
    Fill((int64_t)t);
    _Ctx.Sel = 0;
    SetEditing(true);
}

static void Commit(void)
{
    ClampDay();
    (void)CwlTime_SetByHand((uint64_t)CwlCivil_Unix(_Ctx.F[0], _Ctx.F[1], _Ctx.F[2], _Ctx.F[3], _Ctx.F[4], 0));
    SetEditing(false);   // (Wi-Fi gave the time meanwhile: it stays, the page shows it)
}

static OSD_Result_t OnCapture(const Button_t Button, const ButtonState_t State, void *arg)
{
    (void)arg;
    if (State != kButtonState_Pressed) return kOSD_Result_Ok;
    if (CwlTime_FromWifi()) { SetEditing(false); return kOSD_Result_Ok; }
    switch (Button)
    {
    case kButton_Up: _Ctx.Sel = (_Ctx.Sel + kFields - 1) % kFields; break;
    case kButton_Down: _Ctx.Sel = (_Ctx.Sel + 1) % kFields; break;
    case kButton_Left: Step(-1); break;
    case kButton_Right: Step(1); break;
    case kButton_A: Commit(); break;
    case kButton_B: SetEditing(false); break;
    default: break;
    }
    return kOSD_Result_Ok;
}

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
    snprintf(t, sizeof t, "%s", Text);
    const char *cur = lv_label_get_text(_Ctx.pLabel[i]);
    if (cur == NULL || strcmp(cur, t) != 0) lv_label_set_text(_Ctx.pLabel[i], t);
}

OSD_Result_t TimeSet_Draw(void *arg)
{
    if (arg == NULL) return kOSD_Result_Err_NullDataPtr;
    lv_obj_t *const pScreen = (lv_obj_t *const)arg;
    char R[kRows][kTextLen] = { "" };
    bool W[kRows] = { false };
    if (_Ctx.Editing)
    {
        static const char *const kName[kFields] = { "YEAR", "MONTH", "DAY", "HOUR", "MINUTE" };
        snprintf(R[0], kTextLen, "SET  THE  CLOCK  (UTC)");
        W[0] = true;
        for (int i = 0; i < kFields; i++)
        {
            const bool On = i == _Ctx.Sel;
            snprintf(R[1 + i], kTextLen, i == 0 ? (On ? "%.6s  <  %04d  >" : "%.6s  %04d") : (On ? "%.6s  <  %02d  >" : "%.6s  %02d"), kName[i], _Ctx.F[i]);
            W[1 + i] = On;
        }
        snprintf(R[6], kTextLen, "A:  SET    B:  BACK");
        W[6] = true;
    }
    else
    {
        const uint64_t now = CwlTime_Unix();
        snprintf(R[0], kTextLen, "CLOCK");
        W[0] = true;
        if (now)
        {
            int y, mo, d, h, mi, s;
            CwlCivil_Split((int64_t)now, &y, &mo, &d, &h, &mi, &s);
            snprintf(R[1], kTextLen, "%04d-%02d-%02d", y, mo, d);
            snprintf(R[2], kTextLen, "%02d:%02d:%02d  UTC", h, mi, s);
            W[1] = W[2] = true;
            if (CwlTime_FromWifi()) snprintf(R[3], kTextLen, "FROM  WI-FI");
            else
            {
                snprintf(R[3], kTextLen, "SET  BY  HAND");
                snprintf(R[5], kTextLen, "A:  CHANGE");
                W[5] = true;
            }
        }
        else
        {
            snprintf(R[1], kTextLen, "NOT  SET");
            snprintf(R[2], kTextLen, "NO  WI-FI  TIME");
            W[1] = true;
            snprintf(R[5], kTextLen, "A:  SET  BY  HAND");
            W[5] = true;
        }
    }
    for (int i = 0; i < kRows; i++) Line(pScreen, i, W[i], R[i]);
    return kOSD_Result_Ok;
}

OSD_Result_t TimeSet_OnButton(const Button_t Button, const ButtonState_t State, void *arg)
{
    (void)arg;
    if (State == kButtonState_Pressed && Button == kButton_A && !_Ctx.Editing && !CwlTime_FromWifi()) Begin();
    return kOSD_Result_Ok;
}

OSD_Result_t TimeSet_OnTransition(void *arg)
{
    (void)arg;
    if (_Ctx.Editing) SetEditing(false);
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
