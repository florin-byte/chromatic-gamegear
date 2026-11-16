#include "wifi_link.h"

#include "cwl.h"
#include "menu_mgr.h"
#include "osd_shared.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// SYSTEM > LINK CABLE (was WI-FI LINK): the link cable mode and the nearby
// partner. Same panel and editor style as SYSTEM > ONLINE.
//
//   view     MODE, what the link does, partner / address, PARTNER; A: change
//   editing  the D-pad is captured: UP/DOWN row, LEFT/RIGHT value
//            (MODE: AUTO NEARBY ONLINE OFF; PARTNER: ANY = pair with the next
//            side that answers, or one of the sides heard nearby: the pick
//            list), A or B: done (applies)
//
// Wi-Fi credentials stay console-set (`cwl wifi <ssid> <password>`).
enum {
    kX_px = 77,
    kY_px = 46,          // the panel below the tab title: six 9 px lines
    kRow_px = 9,
    kTextLen = 24,
    kNumLabels = 6,
    kMaxPick = 6,
};

typedef enum { kRow_Mode, kRow_Partner, kNumRows } Row_t;

typedef struct {
    lv_obj_t *pLabel[kNumLabels];
    char Text[kNumLabels][kTextLen];
    lv_style_t *pStyle[kNumLabels];
    bool Editing;
    Row_t Row;
    CwlMode_t Mode;                  // being edited
    int Pick;                        // 0 = ANY, 1.. = Picks[Pick - 1]
    int NumPicks;
    CwlNearby_t Picks[kMaxPick];
} WifiLink_t;

static WifiLink_t _Ctx;

static OSD_Result_t OnCapture(const Button_t Button, const ButtonState_t State, void *arg);

static void LineStyled(lv_obj_t *pScreen, int i, lv_style_t *pStyle, const char *Text)
{
    if (_Ctx.pLabel[i] == NULL) {
        _Ctx.pLabel[i] = lv_label_create(pScreen);
        lv_obj_align(_Ctx.pLabel[i], LV_ALIGN_TOP_LEFT, kX_px, kY_px + i * kRow_px);
        _Ctx.Text[i][0] = '\x01';
        _Ctx.pStyle[i] = NULL;
        lv_obj_move_foreground(_Ctx.pLabel[i]);
    }
    if (_Ctx.pStyle[i] != pStyle) {
        if (_Ctx.pStyle[i] != NULL) lv_obj_remove_style(_Ctx.pLabel[i], _Ctx.pStyle[i], 0);
        lv_obj_add_style(_Ctx.pLabel[i], pStyle, 0);
        _Ctx.pStyle[i] = pStyle;
    }
    if (strncmp(_Ctx.Text[i], Text, kTextLen) != 0) {
        strncpy(_Ctx.Text[i], Text, kTextLen - 1);
        _Ctx.Text[i][kTextLen - 1] = '\0';
        lv_label_set_text(_Ctx.pLabel[i], _Ctx.Text[i]);
    }
}

static void Line(lv_obj_t *pScreen, int i, bool White, const char *Text)
{
    LineStyled(pScreen, i, White ? OSD_GetStyleTextWhite() : OSD_GetStyleTextGrey(), Text);
}

static void DeleteAll(void)
{
    for (int i = 0; i < kNumLabels; i++) {
        if (_Ctx.pLabel[i] != NULL) {
            lv_obj_del(_Ctx.pLabel[i]);
            _Ctx.pLabel[i] = NULL;
        }
    }
}

// The pick list: the remembered partner first (even when not heard), then
// every side heard nearby.
static void LoadPicks(void)
{
    _Ctx.NumPicks = 0;
    CwlNearby_t p;
    const bool HavePartner = CWL_GetPartner(&p);
    if (HavePartner) _Ctx.Picks[_Ctx.NumPicks++] = p;
    CwlNearby_t l[kMaxPick];
    const int n = CWL_GetNearby(l, kMaxPick);
    for (int i = 0; i < n && _Ctx.NumPicks < kMaxPick; i++) {
        if (HavePartner && memcmp(l[i].Mac, p.Mac, 6) == 0) continue;
        _Ctx.Picks[_Ctx.NumPicks++] = l[i];
    }
    _Ctx.Pick = HavePartner ? 1 : 0;
}

static void SetEditing(bool On)
{
    if (On) {
        _Ctx.Mode = CWL_GetMode();
        _Ctx.Row = kRow_Mode;
        LoadPicks();
    }
    _Ctx.Editing = On;
    MenuMgr_SetCapture(On ? OnCapture : NULL);
}

static void Apply(void)
{
    if (_Ctx.Mode != CWL_GetMode()) CWL_SetMode(_Ctx.Mode);
    CwlNearby_t cur;
    const bool Had = CWL_GetPartner(&cur);
    if (_Ctx.Pick == 0) {
        if (Had) CWL_SetPartner(NULL);
    } else {
        const CwlNearby_t *p = &_Ctx.Picks[_Ctx.Pick - 1];
        if (!Had || memcmp(cur.Mac, p->Mac, 6) != 0) CWL_SetPartner(p->Mac);
    }
}

static void PartnerText(char *Buf, size_t Len, const char *Mark)
{
    if (_Ctx.Editing) {
        const char *n = _Ctx.Pick == 0 ? "ANY" : _Ctx.Picks[_Ctx.Pick - 1].Name;
        snprintf(Buf, Len, "%sPARTNER  %s", Mark, n);
        return;
    }
    CwlNearby_t p;
    if (CWL_GetPartner(&p)) snprintf(Buf, Len, "%sPARTNER  %s", Mark, p.Name);
    else snprintf(Buf, Len, "%sPARTNER  ANY", Mark);
}

OSD_Result_t WifiLink_Draw(void* arg)
{
    if (arg == NULL) return kOSD_Result_Err_NullDataPtr;
    lv_obj_t *const pScreen = (lv_obj_t *const) arg;
    char t[kTextLen + 8];

    const CwlMode_t Mode = _Ctx.Editing ? _Ctx.Mode : CWL_GetMode();
    const bool MarkMode = _Ctx.Editing && _Ctx.Row == kRow_Mode;
    snprintf(t, sizeof(t), "%sMODE  %s%s%s", MarkMode ? ">" : " ", MarkMode ? "< " : "", CWL_ModeText(Mode),
             MarkMode ? " >" : "");
    LineStyled(pScreen, 0, OSD_GetStyleTextWhite(), t);

    if (_Ctx.Editing) {
        Line(pScreen, 1, false, "");
        Line(pScreen, 2, false, "");
        const bool MarkP = _Ctx.Row == kRow_Partner;
        char p[kTextLen];
        PartnerText(p, sizeof(p), MarkP ? ">" : " ");
        Line(pScreen, 3, MarkP, p);
        snprintf(t, sizeof(t), " %d  NEARBY", _Ctx.NumPicks);
        Line(pScreen, 4, false, Mode == kCwlMode_Online || Mode == kCwlMode_Off ? "" : t);
        Line(pScreen, 5, false, " A:  DONE");
        return kOSD_Result_Ok;
    }

    const CwlStatus_t Status = CWL_GetStatus();
    char Detail[kTextLen - 2] = "";
    if (Status == kCwlStatus_Asleep) snprintf(Detail, sizeof(Detail), "WAKES  ON  LINK");
    else if (Status == kCwlStatus_NoWifi) snprintf(Detail, sizeof(Detail), "USE  CONSOLE");
    else if (Status != kCwlStatus_Off) CWL_GetDetailText(Detail, sizeof(Detail));
    snprintf(t, kTextLen, " %s", Status == kCwlStatus_Off ? "" : CWL_StatusText(Status));
    Line(pScreen, 1, true, t);
    snprintf(t, kTextLen, " %s", Detail);
    Line(pScreen, 2, false, t);
    char p[kTextLen] = "";
    if (Mode == kCwlMode_Auto || Mode == kCwlMode_Nearby) PartnerText(p, sizeof(p), " ");
    Line(pScreen, 3, false, p);
    Line(pScreen, 4, false, "");
    Line(pScreen, 5, false, " A:  CHANGE");
    return kOSD_Result_Ok;
}

static OSD_Result_t OnCapture(const Button_t Button, const ButtonState_t State, void *arg)
{
    (void)arg;
    if (State != kButtonState_Pressed) return kOSD_Result_Ok;
    switch (Button) {
    case kButton_Up:
    case kButton_Down:
        _Ctx.Row = _Ctx.Row == kRow_Mode ? kRow_Partner : kRow_Mode;
        if (_Ctx.Row == kRow_Partner) {
            const int keep = _Ctx.Pick;
            LoadPicks();       // the list may have grown
            if (keep <= _Ctx.NumPicks) _Ctx.Pick = keep;
        }
        break;
    case kButton_Left:
    case kButton_Right: {
        const int d = Button == kButton_Right ? 1 : -1;
        if (_Ctx.Row == kRow_Mode) {
            _Ctx.Mode = (CwlMode_t)((_Ctx.Mode + kCwlNumMode + d) % kCwlNumMode);
        } else {
            const int n = _Ctx.NumPicks + 1;
            _Ctx.Pick = (_Ctx.Pick + n + d) % n;
        }
        break;
    }
    case kButton_A:
    case kButton_B:
        Apply();
        SetEditing(false);
        break;
    default:
        break;
    }
    return kOSD_Result_Ok;
}

OSD_Result_t WifiLink_OnButton(const Button_t Button, const ButtonState_t State, void* arg)
{
    (void)arg;
    if (State == kButtonState_Pressed && Button == kButton_A) SetEditing(true);
    return kOSD_Result_Ok;
}

OSD_Result_t WifiLink_OnTransition(void* arg)
{
    (void)arg;
    if (_Ctx.Editing) SetEditing(false);   // leaving without A/B: nothing applied
    DeleteAll();
    return kOSD_Result_Ok;
}

// ---------------------------------------------------------------- toast

// "LINKED  CHROMATIC 1A2B" for a few seconds after a nearby pairing, drawn
// over the game like the battery icon.
typedef struct {
    lv_obj_t *pLabel;
    char Text[kTextLen];
} Toast_t;
static Toast_t _Toast;

static OSD_Result_t Toast_Draw(void *arg)
{
    if (arg == NULL) return kOSD_Result_Err_NullDataPtr;
    lv_obj_t *const pScreen = (lv_obj_t *const) arg;
    char t[kTextLen];
    const bool Show = CWL_GetToast(t, sizeof(t));
    if (!Show) {
        if (_Toast.pLabel != NULL) {
            lv_obj_del(_Toast.pLabel);
            _Toast.pLabel = NULL;
        }
        return kOSD_Result_Ok;
    }
    if (_Toast.pLabel == NULL) {
        _Toast.pLabel = lv_label_create(pScreen);
        lv_obj_add_style(_Toast.pLabel, OSD_GetStyleTextWhite(), 0);
        lv_obj_set_style_bg_color(_Toast.pLabel, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(_Toast.pLabel, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(_Toast.pLabel, 2, 0);
        lv_obj_align(_Toast.pLabel, LV_ALIGN_TOP_MID, 0, 2);
        _Toast.Text[0] = '\x01';
    }
    if (strncmp(_Toast.Text, t, kTextLen) != 0) {
        strncpy(_Toast.Text, t, kTextLen - 1);
        _Toast.Text[kTextLen - 1] = '\0';
        lv_label_set_text(_Toast.pLabel, _Toast.Text);
    }
    lv_obj_move_foreground(_Toast.pLabel);
    return kOSD_Result_Ok;
}

OSD_Result_t LinkToast_Init(OSD_Widget_t *const pWidget)
{
    if (pWidget == NULL) return kOSD_Result_Err_NullDataPtr;
    pWidget->fnDraw = Toast_Draw;
    return kOSD_Result_Ok;
}
