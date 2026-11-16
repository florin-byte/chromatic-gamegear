#include "online.h"

#include "cwl.h"
#include "cwl_online.h"
#include "cwl_relay.h"
#include "menu_mgr.h"
#include "osd_shared.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// SYSTEM > ONLINE: Shiny Link internet play (shiny_gen_3
// notes/CHROMATIC_RELAY.md). Pair with a Shiny Gen account (a QR of
// shinygen.ai/link?code=..., plus the code), then link games go through
// relay.shinyrelay.com (P2P when the routers allow) whenever WI-FI LINK is on.
//
//   not paired  A: pair
//   pairing     the code (A: QR of shinygen.ai/link?code=...), B: cancel
//   paired      status + room; A: edit (the D-pad is captured while editing:
//               UP/DOWN row, LEFT/RIGHT value, A on CODE: enter a friend's
//               room code, A A on UNPAIR, B: done)
enum {
    kX_px = 77,
    kY_px = 46,          // the panel below the tab title runs y 44..108: six 9 px lines
    kRow_px = 9,
    kTextLen = 24,
    kQrSize_px = 62,     // version 3 (29 modules) at 2 px + 2 px light margin
    kNumLabels = 9,
};

typedef enum {
    kView_None,
    kView_Unpaired,
    kView_Starting,
    kView_Qr,
    kView_Failed,
    kView_Paired,
} View_t;

typedef enum {
    kRow_Room,
    kRow_Code,
    kRow_Unpair,
    kNumRows,
} Row_t;

typedef struct {
    View_t View;
    lv_obj_t *pLabel[kNumLabels];
    char Text[kNumLabels][kTextLen];
    lv_obj_t *pQr;
    char QrData[96];
    // editing (paired view)
    bool Editing;
    Row_t Row;
    bool CodeEdit;
    char Code[kSl_CodeLen + 1];
    int Cur;
    bool UnpairArmed;
    bool ShowQr;         // pairing: the QR fills the panel instead of the code text
    lv_style_t *pStyle[kNumLabels];   // style each label has (restyle only on change:
                                      // every restyle/raise invalidates and redraws)
} Online_t;

static Online_t _Ctx;

static const char *const kRoomName[kCwlNumRoom] = {"APP", "MINE", "HOST", "JOIN"};

static OSD_Result_t OnCapture(const Button_t Button, const ButtonState_t State, void *arg);

static void DeleteAll(void)
{
    for (int i = 0; i < kNumLabels; i++) {
        if (_Ctx.pLabel[i] != NULL) {
            lv_obj_del(_Ctx.pLabel[i]);
            _Ctx.pLabel[i] = NULL;
        }
    }
    if (_Ctx.pQr != NULL) {
        lv_qrcode_delete(_Ctx.pQr);
        _Ctx.pQr = NULL;
    }
    _Ctx.QrData[0] = '\0';
}

// Label i at row y (px from the top of the panel); white, grey or large white.
static void LineStyled(lv_obj_t *pScreen, int i, int y, lv_style_t *pStyle, const char *Text)
{
    if (_Ctx.pLabel[i] == NULL) {
        _Ctx.pLabel[i] = lv_label_create(pScreen);
        lv_obj_align(_Ctx.pLabel[i], LV_ALIGN_TOP_LEFT, kX_px, kY_px + y);
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

static void Line(lv_obj_t *pScreen, int i, int y, bool White, const char *Text)
{
    LineStyled(pScreen, i, y, White ? OSD_GetStyleTextWhite() : OSD_GetStyleTextGrey(), Text);
}

static View_t WantedView(void)
{
    const CwlPairState_t p = CwlOnline_PairState();
    if (p == kCwlPair_Starting) return kView_Starting;
    if (p == kCwlPair_Waiting) return kView_Qr;
    if (p == kCwlPair_Failed) return kView_Failed;
    return CwlOnline_Paired() ? kView_Paired : kView_Unpaired;
}

static void SetEditing(bool On)
{
    _Ctx.Editing = On;
    _Ctx.CodeEdit = false;
    _Ctx.UnpairArmed = false;
    MenuMgr_SetCapture(On ? OnCapture : NULL);
}

static void RowText(Row_t r, char *Buf, size_t Len)
{
    const char *mark = (_Ctx.Editing && _Ctx.Row == r) ? ">" : " ";
    switch (r) {
    case kRow_Room:
        snprintf(Buf, Len, "%sROOM   %s", mark, kRoomName[CwlOnline_Room() % kCwlNumRoom]);
        break;
    case kRow_Code:
        if (_Ctx.CodeEdit) {
            char c[16] = "";
            int n = 0;
            for (int i = 0; i < kSl_CodeLen; i++) {
                if (i == _Ctx.Cur) c[n++] = '[';
                c[n++] = _Ctx.Code[i];
                if (i == _Ctx.Cur) c[n++] = ']';
            }
            c[n] = '\0';
            snprintf(Buf, Len, "%sCODE %s", mark, c);
        } else {
            const char *code = CwlOnline_JoinCode();
            snprintf(Buf, Len, "%sCODE   %s", mark, code[0] ? code : "------");
        }
        break;
    case kRow_Unpair:
        snprintf(Buf, Len, "%s%s", mark, _Ctx.UnpairArmed ? "A  AGAIN:  UNPAIR" : "UNPAIR");
        break;
    default:
        Buf[0] = '\0';
    }
}

OSD_Result_t Online_Draw(void *arg)
{
    if (arg == NULL) {
        return kOSD_Result_Err_NullDataPtr;
    }
    lv_obj_t *const pScreen = (lv_obj_t *const)arg;

    const View_t v = WantedView();
    static bool QrWas = false, EditWas = false;
    if (v != _Ctx.View || QrWas != _Ctx.ShowQr || EditWas != _Ctx.Editing) {
        QrWas = _Ctx.ShowQr;
        EditWas = _Ctx.Editing;
        DeleteAll();
        if (v != kView_Paired && _Ctx.Editing) SetEditing(false);
        _Ctx.View = v;
    }

    char t[kTextLen];
    switch (v) {
    case kView_Unpaired:
        Line(pScreen, 0, 0, true, "NOT  PAIRED");
        Line(pScreen, 1, kRow_px * 1 + 2, false, "PLAY  LINK  GAMES");
        Line(pScreen, 2, kRow_px * 2 + 2, false, "OVER  THE  INTERNET");
        Line(pScreen, 3, kRow_px * 3 + 2, false, "WITH  SHINY  GEN.");
        Line(pScreen, 4, kRow_px * 5, true, "A:  PAIR");
        break;
    case kView_Starting:
        Line(pScreen, 0, 0, true, "GETTING  A  CODE...");
        Line(pScreen, 1, kRow_px, false, CWL_IsOnline() ? "" : "JOINING  WI-FI");
        Line(pScreen, 2, kRow_px * 5, false, "B:  CANCEL");
        break;
    case kView_Qr: {
        char code[16], url[96];
        CwlOnline_PairCode(code, sizeof(code), url, sizeof(url));
        if (_Ctx.ShowQr) {
            // shinygen.ai/link?code=... (version 3, 29 modules at 2 px + margin)
            if (_Ctx.pQr == NULL) {
                _Ctx.pQr = lv_qrcode_create(pScreen, kQrSize_px, lv_color_black(), lv_color_white());
                lv_obj_align(_Ctx.pQr, LV_ALIGN_TOP_LEFT, kX_px - 2, kY_px - 3);
                lv_obj_move_foreground(_Ctx.pQr);
            }
            if (strcmp(url, _Ctx.QrData) != 0) {
                snprintf(_Ctx.QrData, sizeof(_Ctx.QrData), "%s", url);
                (void)lv_qrcode_update(_Ctx.pQr, _Ctx.QrData, strlen(_Ctx.QrData));
            }
        } else {
            Line(pScreen, 0, 0, false, "ON  A  PHONE  OPEN");
            Line(pScreen, 1, kRow_px, true, "SHINYGEN.AI/LINK");
            Line(pScreen, 2, kRow_px * 2, false, "AND  ENTER");
            LineStyled(pScreen, 3, kRow_px * 3 - 1, OSD_GetStyleTextWhite_L(), code);
            Line(pScreen, 4, kRow_px * 5, false, "A:  QR    B:  CANCEL");
        }
        break;
    }
    case kView_Failed:
        Line(pScreen, 0, 0, true, "PAIRING  FAILED");
        Line(pScreen, 1, kRow_px, false, CwlOnline_PairError());
        Line(pScreen, 2, kRow_px * 5, true, "A:  TRY  AGAIN");
        break;
    case kView_Paired: {
        if (!_Ctx.Editing) {
            Line(pScreen, 0, 0, true, CWL_RelayStatusText());
            CWL_RelayRoomText(t, sizeof(t));
            Line(pScreen, 1, kRow_px, false, t);
            RowText(kRow_Room, t, sizeof(t));
            Line(pScreen, 2, kRow_px * 2 + 4, false, t);
            // How link traffic travels is the LINK CABLE mode (0.17.0 retired "via").
            snprintf(t, sizeof(t), " LINK   %s", CWL_ModeText(CWL_GetMode()));
            Line(pScreen, 3, kRow_px * 3 + 4, false, t);
            Line(pScreen, 4, kRow_px * 5, false, "A:  SETTINGS");
        } else {
            for (int r = 0; r < kNumRows; r++) {
                RowText((Row_t)r, t, sizeof(t));
                Line(pScreen, r, kRow_px * r, _Ctx.Row == r, t);
            }
            Line(pScreen, 5, kRow_px * 5, false, _Ctx.CodeEdit ? "^V  <>  A:OK  B:BACK" : "<>  CHANGE  B:  DONE");
        }
        break;
    }
    default:
        break;
    }
    return kOSD_Result_Ok;
}

static void CycleRow(Row_t r, int Dir)
{
    switch (r) {
    case kRow_Room: {
        int n = ((int)CwlOnline_Room() + Dir + kCwlNumRoom) % kCwlNumRoom;
        if (n == kCwlRoom_Join && !Sl_ValidCode(CwlOnline_JoinCode())) {
            // A friend's room needs its code first.
            _Ctx.Row = kRow_Code;
            _Ctx.CodeEdit = true;
            _Ctx.Cur = 0;
            memset(_Ctx.Code, kSl_CodeAlphabet[0], kSl_CodeLen);
            _Ctx.Code[kSl_CodeLen] = '\0';
            return;
        }
        CwlOnline_SetRoom((CwlRoom_t)n, CwlOnline_JoinCode());
        break;
    }
    default:
        break;
    }
}

static void CodeChar(int Dir)
{
    const int n = (int)strlen(kSl_CodeAlphabet);
    const char *p = strchr(kSl_CodeAlphabet, _Ctx.Code[_Ctx.Cur]);
    int i = p ? (int)(p - kSl_CodeAlphabet) : 0;
    i = (i + Dir + n) % n;
    _Ctx.Code[_Ctx.Cur] = kSl_CodeAlphabet[i];
}

static OSD_Result_t OnCapture(const Button_t Button, const ButtonState_t State, void *arg)
{
    (void)arg;
    if (State != kButtonState_Pressed) return kOSD_Result_Ok;
    if (_Ctx.CodeEdit) {
        switch (Button) {
        case kButton_Up: CodeChar(+1); break;
        case kButton_Down: CodeChar(-1); break;
        case kButton_Left: _Ctx.Cur = (_Ctx.Cur + kSl_CodeLen - 1) % kSl_CodeLen; break;
        case kButton_Right: _Ctx.Cur = (_Ctx.Cur + 1) % kSl_CodeLen; break;
        case kButton_A:
            if (Sl_ValidCode(_Ctx.Code)) CwlOnline_SetRoom(kCwlRoom_Join, _Ctx.Code);
            _Ctx.CodeEdit = false;
            _Ctx.Row = kRow_Room;
            break;
        case kButton_B: _Ctx.CodeEdit = false; break;
        default: break;
        }
        return kOSD_Result_Ok;
    }
    if (Button != kButton_A) _Ctx.UnpairArmed = false;
    switch (Button) {
    case kButton_Up: _Ctx.Row = (Row_t)((_Ctx.Row + kNumRows - 1) % kNumRows); break;
    case kButton_Down: _Ctx.Row = (Row_t)((_Ctx.Row + 1) % kNumRows); break;
    case kButton_Left: CycleRow(_Ctx.Row, -1); break;
    case kButton_Right: CycleRow(_Ctx.Row, +1); break;
    case kButton_A:
        if (_Ctx.Row == kRow_Code) {
            _Ctx.CodeEdit = true;
            _Ctx.Cur = 0;
            const char *c = CwlOnline_JoinCode();
            if (Sl_ValidCode(c)) {
                memcpy(_Ctx.Code, c, kSl_CodeLen);
            } else {
                memset(_Ctx.Code, kSl_CodeAlphabet[0], kSl_CodeLen);
            }
            _Ctx.Code[kSl_CodeLen] = '\0';
        } else if (_Ctx.Row == kRow_Unpair) {
            if (_Ctx.UnpairArmed) {
                SetEditing(false);
                CwlOnline_Unpair();
            } else {
                _Ctx.UnpairArmed = true;
            }
        } else {
            CycleRow(_Ctx.Row, +1);
        }
        break;
    case kButton_B: SetEditing(false); break;
    default: break;
    }
    return kOSD_Result_Ok;
}

OSD_Result_t Online_OnButton(const Button_t Button, const ButtonState_t State, void *arg)
{
    (void)arg;
    if (State != kButtonState_Pressed) return kOSD_Result_Ok;
    switch (WantedView()) {
    case kView_Unpaired:
        if (Button == kButton_A) CwlOnline_PairStart();
        break;
    case kView_Starting:
        if (Button == kButton_B) CwlOnline_PairCancel();
        break;
    case kView_Qr:
        if (Button == kButton_A) _Ctx.ShowQr = !_Ctx.ShowQr;
        else if (Button == kButton_B) {
            if (_Ctx.ShowQr) _Ctx.ShowQr = false;
            else CwlOnline_PairCancel();
        }
        break;
    case kView_Failed:
        if (Button == kButton_A) CwlOnline_PairStart();
        else if (Button == kButton_B) CwlOnline_PairCancel();
        break;
    case kView_Paired:
        if (Button == kButton_A) {
            _Ctx.Row = kRow_Room;
            SetEditing(true);
        }
        break;
    default:
        break;
    }
    return kOSD_Result_Ok;
}

OSD_Result_t Online_OnTransition(void *arg)
{
    (void)arg;
    if (_Ctx.Editing) SetEditing(false);
    DeleteAll();
    _Ctx.View = kView_None;
    _Ctx.ShowQr = false;
    return kOSD_Result_Ok;
}
