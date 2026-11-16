#include "download.h"
#include "share.h"

#include "nw_dev.h"
#include "osd_shared.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// SYSTEM > DOWNLOAD (lane wifirecv1): "Send to my Chromatic" in an explicit receive mode.
// One action, RECEIVE GAME (A): the link stops, the unit asks the server for a short code and
// shows it; the sender types it on shinyrelay.com; the game is written to the cart. B cancels
// any time before the write starts. The install machine (nw/cartflash/cf_install.c) owns what
// the page shows (NW_CartView) and takes its buttons (NW_CartButton); this file only draws.
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
    uint8_t ShareWhy;   // m110s: B: FROM A CHROMATIC did not start (kShare_*): said until a button
} Download_t;

static Download_t _Ctx;

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

OSD_Result_t Download_Draw(void *arg)
{
    if (arg == NULL) return kOSD_Result_Err_NullDataPtr;
    lv_obj_t *const pScreen = (lv_obj_t *const)arg;
    {
        // lane share1: a share with another Chromatic (started here or on GAMES / PLAY) shows here while it runs
        ShareRows_t sr;
        if (Share_Rows(&sr)) { for (int i = 0; i < kRows; i++) Line(pScreen, i, (sr.White >> i) & 1u, sr.T[i]); return kOSD_Result_Ok; }
    }
    NwCartView_t v;
    NW_CartView(&v);
    if (_Ctx.ShareWhy && v.Kind == kNwCv_Menu)
    {
        // m110s: before, a receive that could not start did nothing at all
        const bool mem = _Ctx.ShareWhy == kShare_NoMemory;
        const char *R[kRows] = { "NOT  STARTED", mem ? "NOT  ENOUGH  MEMORY" : "SOMETHING  ELSE  IS", mem ? "TURN  IT  OFF  AND  ON" : "RUNNING:  WAIT", "", "", "A:  OK", "" };
        const bool W[kRows] = { true, false, false, false, false, true, false };
        Rows(pScreen, R, W);
        return kOSD_Result_Ok;
    }
    char a[kTextLen], b[kTextLen];
    // two rows of why: "ROW1|ROW2"
    char r1[kTextLen] = "", r2[kTextLen] = "";
    {
        const char *bar = strchr(v.Note, '|');
        snprintf(r1, sizeof r1, "%.*s", bar ? (int)(bar - v.Note) : (int)strlen(v.Note), v.Note);
        snprintf(r2, sizeof r2, "%s", bar ? bar + 1 : "");
    }
    switch (v.Kind)
    {
    case kNwCv_Getting:
    {
        const char *R[kRows] = { v.Title, "", "THE  LINK  PAUSES", "UNTIL  IT  IS  DONE", "", "", "B:  CANCEL" };
        const bool W[kRows] = { true, false, false, false, false, false, true };
        Rows(pScreen, R, W);
        break;
    }
    case kNwCv_Code:
    {
        // the code, its letters apart: easy to read off the small panel
        int n = 0;
        for (int i = 0; v.Code[i] && n < kTextLen - 4; i++)
        {
            if (i) { a[n++] = ' '; a[n++] = ' '; }
            a[n++] = v.Code[i];
        }
        a[n] = '\0';
        if (v.Mins) snprintf(b, sizeof b, "%u  MIN  LEFT", v.Mins);
        else snprintf(b, sizeof b, "A  GAME  CAME");
        // 1.0.3: the server's 429 (its rate limits): the code stays, the unit asks again later
        const char *R[kRows] = { "YOUR  CODE", a, "TYPE  IT  AT", "SHINYRELAY.COM", v.Note[0] ? v.Note : "WAITING  FOR  A  GAME", b, "B:  CANCEL" };
        const bool W[kRows] = { false, true, false, false, false, false, true };
        Rows(pScreen, R, W);
        break;
    }
    case kNwCv_Expired:
    {
        const char *R[kRows] = { "CODE  EXPIRED", "NO  GAME  CAME", "", "", "", "A:  NEW  CODE", "B:  BACK" };
        const bool W[kRows] = { true, false, false, false, false, true, true };
        Rows(pScreen, R, W);
        break;
    }
    case kNwCv_Fail:
    {
        const char *R[kRows] = { r1, r2, "", "", "", "A:  TRY  AGAIN", "B:  BACK" };
        const bool W[kRows] = { true, false, false, false, false, true, true };
        Rows(pScreen, R, W);
        break;
    }
    case kNwCv_Arrived:
    {
        const char *R[kRows] = { "GAME  ARRIVED", v.Title, "GETTING  READY", "", "", "", "B:  CANCEL" };
        const bool W[kRows] = { true, false, false, false, false, false, true };
        Rows(pScreen, R, W);
        break;
    }
    case kNwCv_Battery:
    {
        // it installs by itself once there is power (the charger); 1.0.5 (owner 2026-10-19): or A, INSTALL ANYWAY
        const char *R[kRows] = { "BATTERY  LOW", v.Title, "CHARGE  IT  TO", "INSTALL", "", "A:  INSTALL  ANYWAY", "B:  CANCEL" };
        const bool W[kRows] = { true, false, false, false, false, true, true };
        Rows(pScreen, R, W);
        break;
    }
    case kNwCv_Installing:
    {
        if (v.Backup)
        {
            // 1.0.6: the cart is read, then sent to the website (B stops it; nothing is written to the cart)
            const char *R[kRows] = { "BACKING  UP", v.Title, v.Note, "KEEP  IT  ON  AND", "NEAR  WI-FI", "", "B:  STOP" };
            const bool W[kRows] = { true, false, true, false, false, false, true };
            Rows(pScreen, R, W);
            break;
        }
        // no buttons: the menu may be closed any time
        // 1.0.5: after INSTALL ANYWAY the one warning: the battery is low, keep it on
        const char *R[kRows] = { "INSTALLING", v.Title, v.Note, v.Anyway ? "LOW  BATTERY:  KEEP" : "KEEP  IT  ON  AND",
                                 v.Anyway ? "IT  ON  NEAR  WI-FI" : "NEAR  WI-FI", "", "" };
        const bool W[kRows] = { true, false, true, false, false, false, false };
        Rows(pScreen, R, W);
        break;
    }
    case kNwCv_Result:
    {
        // INSTALLED, or NOT INSTALLED + two rows of why
        // cartspeed2: A starts receive mode again (a second game right after the first), B closes
        const char *R[kRows] = { v.Title, r1, r2, "", "", "A:  NEXT  GAME", "B:  DONE" };   // (lane m110u: the result says where it went)
        const bool W[kRows] = { true, false, false, false, false, true, true };
        Rows(pScreen, R, W);
        break;
    }
    case kNwCv_Unfinished:
    {
        // an install that was cut off: the cart has no first block until it is finished
        const char *R[kRows] = { "INSTALL  NOT", "FINISHED", v.Title, v.Note[0] ? v.Note : "CART  WILL  NOT", v.Note[0] ? "" : "START  UNTIL  DONE",
                                 "A:  RESUME", "B:  CANCEL" };
        const bool W[kRows] = { true, true, false, false, false, true, true };
        Rows(pScreen, R, W);
        break;
    }
    case kNwCv_Menu:
    default:
    {
        // lane share1: B receives from another Chromatic nearby (its SHARE), no website
        const bool sh = Share_Available();
        const char *R[kRows] = { "RECEIVE  A  GAME", "SEND  ONE  FROM", "SHINYRELAY.COM", sh ? "OR  SHARE  ONE  FROM" : "IT  REPLACES  THE", sh ? "A  CHROMATIC  NEARBY" : "GAME  AND  SAVE",
                                 "A:  RECEIVE  GAME", sh ? "B:  FROM A CHROMATIC" : "" };
        const bool W[kRows] = { true, false, false, false, false, true, true };
        Rows(pScreen, R, W);
        break;
    }
    }
    return kOSD_Result_Ok;
}

OSD_Result_t Download_OnButton(const Button_t Button, const ButtonState_t State, void *arg)
{
    (void)arg;
    if (State != kButtonState_Pressed) return kOSD_Result_Ok;
    if (_Ctx.ShareWhy) { if (Button == kButton_A || Button == kButton_B) _Ctx.ShareWhy = 0; return kOSD_Result_Ok; }
    if (Button == kButton_B && Share_Available())
    {
        NwCartView_t v;
        NW_CartView(&v);
        if (v.Kind == kNwCv_Menu) { _Ctx.ShareWhy = (uint8_t)Share_RecvStart(NULL); return kOSD_Result_Ok; }
    }
    if (Button == kButton_A) NW_CartButton(1);
    else if (Button == kButton_B) NW_CartButton(0);
    return kOSD_Result_Ok;
}

OSD_Result_t Download_OnTransition(void *arg)
{
    (void)arg;
    Share_HostLeft();                   // lane share1
    _Ctx.ShareWhy = 0;
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
