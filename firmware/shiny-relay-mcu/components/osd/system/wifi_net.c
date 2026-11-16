#include "wifi_net.h"

#include "cwl.h"
#include "cwl_net.h"
#include "wifi_qr.h"
#include "menu_mgr.h"
#include "osd_shared.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// SYSTEM > WI-FI: which network the Chromatic joins. Same panel and editor
// style as SYSTEM > ONLINE (the D-pad is captured while choosing).
//
//   main      network, state, address, saved count; A: choose
//   list      SET UP WITH PHONE, SAVED NETWORKS, SCAN AGAIN, then the scan
//             results (signal bars, lock = password); UP/DOWN (wraps), A: pick,
//             B: back
//   keyboard  password: D-pad over the keys, A: type, B: delete (on an empty
//             password: back); keys aA# (letters / capitals / digits and
//             symbols / more symbols), SPACE, SHOW / HIDE, DEL, OK
//   joining   JOINING <name>, then CONNECTED or why not (WRONG PASSWORD ...)
//   saved     the saved networks, newest first; A: JOIN / FORGET / BACK
//   phone     the setup network's name and password (A: its QR code), what
//             the phone did, time left; B: stop
//
// The password typed here lives in this file only until OK (then cwl_net
// owns it and saves it in NVS); it is never drawn unless SHOW is on.
enum {
    kX_px = 77,
    kY_px = 46,
    kRow_px = 9,
    kTextLen = 40,
    kNumLabels = 7,        // rows 0..5; row 6 only under the phone-setup QR
    kListRows = 5,
    kMaxItems = kCwlNet_MaxScan + 3,
    // 0.17.2: the Wi-Fi QR is drawn by wifi_qr.c (ECC L, version 3: 29
    // modules x 2 px + a 2 px margin = 62 px), centred in the panel
    // (interior x 74..147) under the SYSTEM bar (ends 38); row 6 (y 100,
    // glyphs 101..105, border 110) carries "B: BACK" under it.
    kQrSize_px = kWifiQr_Px,
    kQrX_px = 80,          // 80..141
    kQrY_px = kY_px - 7,   // 39..100
    kLockX_px = 90,      // after the signal bars; the name starts right of it
};

typedef enum { kV_Main, kV_List, kV_Keys, kV_Joining, kV_Saved, kV_Phone } View_t;
typedef enum { kItem_Ap, kItem_Saved, kItem_Phone, kItem_Rescan } ItemKind_t;
enum { kPh_Choose = 0, kPh_Qr, kPh_Type };

static const char *const kPages[4][3] = {
    {"abcdefghij", "klmnopqrst", "uvwxyz.-_@"},
    {"ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ.-_@"},
    {"1234567890", "!#$%&*+=?/", "()[]<>:;,'"},
    {"~^`{}|\\\"", "", ""},
};
static const char *const kFunc[] = {"aA#", "SPC", "SHOW", "DEL", "OK"};
enum { kNumFunc = 5, kFuncPage = 0, kFuncSpace, kFuncShow, kFuncDel, kFuncOk };

typedef struct {
    View_t View;
    bool Captured;
    lv_obj_t *pLabel[kNumLabels];
    char Text[kNumLabels][kTextLen];
    lv_style_t *pStyle[kNumLabels];
    lv_obj_t *pLock[kListRows];
    lv_obj_t *pQr;
    char QrData[80];
    // phone setup (0.17.2): a first screen to choose how, then its own screen
    int PhoneMode;                   // kPh_Choose / kPh_Qr / kPh_Type
    int PhoneSel;                    // on kPh_Choose: 0 QR, 1 type
    bool PhSeenJoining;
    CwlNetResult_t PhSeenResult;
    // list
    const CwlNetAp_t *Aps;           // cwl_net's own list (no copy: RAM is short)
    int NumAps;
    int Sel, Top;
    uint32_t SeenSeq;
    bool WantScan;
    // keyboard
    char Ssid[kCwlNet_SsidMax + 1];
    char Pw[kCwlNet_PassMax + 1];
    int Page, KRow, KCol;
    bool Show;
    // saved
    int SavedSel, SavedAct;   // action: 0 JOIN, 1 FORGET, 2 BACK
    bool Acting;
} WifiNet_t;

static WifiNet_t _Ctx;

// 5 x 6 padlock, 1 bit alpha, drawn in the label colour (img_recolor).
static const uint8_t kLockMap[] = {0x70, 0x88, 0x88, 0xF8, 0xD8, 0xF8};
static const lv_img_dsc_t kLockImg = {
    .header = {.cf = LV_IMG_CF_ALPHA_1BIT, .always_zero = 0, .reserved = 0, .w = 5, .h = 6},
    .data_size = sizeof(kLockMap),
    .data = kLockMap,
};

static OSD_Result_t OnCapture(const Button_t Button, const ButtonState_t State, void *arg);

// ---------------------------------------------------------------- drawing

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

static void Lock(lv_obj_t *pScreen, int Row, bool On)
{
    if (!On) {
        if (_Ctx.pLock[Row] != NULL) lv_obj_add_flag(_Ctx.pLock[Row], LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (_Ctx.pLock[Row] == NULL) {
        _Ctx.pLock[Row] = lv_img_create(pScreen);
        lv_img_set_src(_Ctx.pLock[Row], &kLockImg);
        lv_obj_set_style_img_recolor(_Ctx.pLock[Row], lv_color_white(), 0);
        lv_obj_set_style_img_recolor_opa(_Ctx.pLock[Row], LV_OPA_COVER, 0);
        lv_obj_align(_Ctx.pLock[Row], LV_ALIGN_TOP_LEFT, kLockX_px, kY_px + (Row + 1) * kRow_px + 1);
    }
    lv_obj_clear_flag(_Ctx.pLock[Row], LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_Ctx.pLock[Row]);
}

static void DeleteQr(void)
{
    if (_Ctx.pQr != NULL) {
        lv_qrcode_delete(_Ctx.pQr);
        _Ctx.pQr = NULL;
    }
    _Ctx.QrData[0] = '\0';
}

static void DeleteAll(void)
{
    for (int i = 0; i < kNumLabels; i++) {
        if (_Ctx.pLabel[i] != NULL) {
            lv_obj_del(_Ctx.pLabel[i]);
            _Ctx.pLabel[i] = NULL;
        }
    }
    for (int i = 0; i < kListRows; i++) {
        if (_Ctx.pLock[i] != NULL) {
            lv_obj_del(_Ctx.pLock[i]);
            _Ctx.pLock[i] = NULL;
        }
    }
    DeleteQr();
}

static void HideLocks(lv_obj_t *pScreen)
{
    for (int i = 0; i < kListRows; i++) Lock(pScreen, i, false);
}

static const char *Bars(int8_t Rssi)
{
    return Rssi > -55 ? "||||" : Rssi > -67 ? "|||." : Rssi > -75 ? "||.." : "|...";
}

// ---------------------------------------------------------------- state

static void SetView(View_t v)
{
    _Ctx.View = v;
    const bool Capture = v != kV_Main;
    if (Capture != _Ctx.Captured) {
        MenuMgr_SetCapture(Capture ? OnCapture : NULL);
        _Ctx.Captured = Capture;
    }
    DeleteQr();
}

static void ClearPassword(void)
{
    memset(_Ctx.Pw, 0, sizeof(_Ctx.Pw));
    _Ctx.Show = false;
}

// 0.17.3: the three actions come first, so SET UP WITH PHONE shows without
// scrolling past every network in range; the scanned networks follow.
static const ItemKind_t kTopItems[] = {kItem_Phone, kItem_Saved, kItem_Rescan};
enum { kNumTop = sizeof(kTopItems) / sizeof(kTopItems[0]) };

static int NumItems(void) { return kNumTop + (_Ctx.Aps != NULL ? _Ctx.NumAps : 0); }

static ItemKind_t ItemKind(int i) { return i < kNumTop ? kTopItems[i] : kItem_Ap; }

static const CwlNetAp_t *ItemAp(int i) { return &_Ctx.Aps[i - kNumTop]; }

static void StartJoin(const char *Pass)
{
    if (CwlNet_Join(_Ctx.Ssid, Pass)) SetView(kV_Joining);
    ClearPassword();
}

// ---------------------------------------------------------------- views

static void DrawMain(lv_obj_t *pScreen)
{
    char t[kTextLen], s[kCwlNet_SsidMax + 1];
    CWL_GetSsidText(s, sizeof(s));
    snprintf(t, sizeof(t), " %.14s", s[0] ? s : "NO  NETWORK");
    Line(pScreen, 0, true, t);
    snprintf(t, sizeof(t), " %s", CwlNet_Op() == kCwlNetOp_Joining ? "JOINING" : CWL_WifiStateText());
    Line(pScreen, 1, false, t);
    char ip[20];
    CWL_GetIpText(ip, sizeof(ip));
    snprintf(t, sizeof(t), " %s", ip);
    Line(pScreen, 2, false, t);
    snprintf(t, sizeof(t), " SAVED  %d  OF  %d", CwlNet_SavedCount(), kCwlNet_MaxSaved);
    Line(pScreen, 3, false, t);
    const CwlNetResult_t r = CwlNet_LastResult();
    if (r != kCwlNetRes_None && r != kCwlNetRes_Ok) snprintf(t, sizeof(t), " LAST:  %s", CwlNet_ResultText(r));
    else t[0] = '\0';
    Line(pScreen, 4, false, t);
    Line(pScreen, 5, false, " A:  CHOOSE  NETWORK");
}

static void DrawList(lv_obj_t *pScreen)
{
    if (_Ctx.WantScan && CwlNet_Op() == kCwlNetOp_Idle) {
        _Ctx.WantScan = false;
        (void)CwlNet_ScanStart();
    }
    if (CwlNet_ScanSeq() != _Ctx.SeenSeq && CwlNet_Op() == kCwlNetOp_Idle) {
        _Ctx.SeenSeq = CwlNet_ScanSeq();
        _Ctx.Aps = CwlNet_ScanList(&_Ctx.NumAps);
        if (_Ctx.Sel >= NumItems()) _Ctx.Sel = 0;
    }
    const bool Scanning = CwlNet_Op() == kCwlNetOp_Scanning || _Ctx.WantScan;
    char t[kTextLen];
    snprintf(t, sizeof(t), " %s", Scanning ? "SCANNING..." : "CHOOSE  A  NETWORK");
    Line(pScreen, 0, true, t);
    if (_Ctx.Sel < _Ctx.Top) _Ctx.Top = _Ctx.Sel;
    if (_Ctx.Sel >= _Ctx.Top + kListRows) _Ctx.Top = _Ctx.Sel - kListRows + 1;
    for (int r = 0; r < kListRows; r++) {
        const int i = _Ctx.Top + r;
        const char *mark = i == _Ctx.Sel ? ">" : " ";
        bool lock = false;
        if (i >= NumItems() || (Scanning && ItemKind(i) == kItem_Ap)) {
            t[0] = '\0';
        } else if (ItemKind(i) == kItem_Ap) {
            const CwlNetAp_t *a = ItemAp(i);
            // The name starts at x 97 whichever mark leads the row: fingfai
            // '>' is 4 px, ' ' 1 px, '|' 2 px, so ">" + 8 spaces and " " + 11
            // spaces both put it right of the padlock (x 90..94). One spacing
            // for both drew the lock over the name's first letter (host render).
            snprintf(t, sizeof(t), "%s%s%s%.10s%s", mark, Bars(a->Rssi), i == _Ctx.Sel ? "        " : "           ",
                     a->Ssid, a->Saved ? "*" : "");
            lock = a->Secure;
        } else if (ItemKind(i) == kItem_Saved) {
            snprintf(t, sizeof(t), "%sSAVED  NETWORKS", mark);
        } else if (ItemKind(i) == kItem_Phone) {
            snprintf(t, sizeof(t), "%sSET  UP  WITH  PHONE", mark);
        } else {
            snprintf(t, sizeof(t), "%sSCAN  AGAIN", mark);
        }
        Line(pScreen, 1 + r, i == _Ctx.Sel, t);
        Lock(pScreen, r, lock);
    }
}

static int RowLen(int Row)
{
    if (Row < 3) return (int)strlen(kPages[_Ctx.Page][Row]);
    return kNumFunc;
}

static void DrawKeys(lv_obj_t *pScreen)
{
    HideLocks(pScreen);
    char t[kTextLen];
    snprintf(t, sizeof(t), " %.24s", _Ctx.Ssid);
    Line(pScreen, 0, false, t);
    // the password: the last 16 characters, as dots unless SHOW
    const size_t n = strlen(_Ctx.Pw);
    const size_t from = n > 16 ? n - 16 : 0;
    char pw[24];
    size_t k = 0;
    for (size_t i = from; i < n && k < 16; i++) pw[k++] = _Ctx.Show ? _Ctx.Pw[i] : '*';
    pw[k] = '\0';
    snprintf(t, sizeof(t), " PW  %s%s_", from ? "<" : "", pw);
    Line(pScreen, 1, true, t);
    for (int r = 0; r < 4; r++) {
        char row[kTextLen];
        size_t o = 0;
        row[o++] = ' ';
        const int len = RowLen(r);
        for (int c = 0; c < len && o + 10 < sizeof(row); c++) {
            const bool sel = r == _Ctx.KRow && c == _Ctx.KCol;
            if (r < 3) {
                row[o++] = sel ? '[' : ' ';
                row[o++] = kPages[_Ctx.Page][r][c];
                row[o++] = sel ? ']' : ' ';
            } else {
                const char *f = (c == kFuncShow && _Ctx.Show) ? "HIDE" : kFunc[c];
                row[o++] = sel ? '[' : ' ';
                for (const char *p = f; *p && o + 4 < sizeof(row); p++) row[o++] = *p;
                row[o++] = sel ? ']' : ' ';
            }
        }
        row[o] = '\0';
        Line(pScreen, 2 + r, r == _Ctx.KRow, row);
    }
}

static void DrawJoining(lv_obj_t *pScreen)
{
    HideLocks(pScreen);
    char t[kTextLen];
    const bool Busy = CwlNet_Op() == kCwlNetOp_Joining;
    snprintf(t, sizeof(t), " %s", Busy ? "JOINING" : CwlNet_ResultText(CwlNet_LastResult()));
    Line(pScreen, 0, true, t);
    snprintf(t, sizeof(t), " %.24s", CwlNet_LastSsid());
    Line(pScreen, 1, false, t);
    char ip[20] = "";
    if (!Busy && CwlNet_LastResult() == kCwlNetRes_Ok) {
        Line(pScreen, 2, false, " SAVED.  THE  LINK  USES  IT");
        CWL_GetIpText(ip, sizeof(ip));
    } else {
        Line(pScreen, 2, false, Busy ? " UP  TO  30  SECONDS" : "");
    }
    snprintf(t, sizeof(t), " %s", ip);
    Line(pScreen, 3, false, t);
    Line(pScreen, 4, false, "");
    Line(pScreen, 5, false, Busy ? "" : (CwlNet_LastResult() == kCwlNetRes_Ok ? " A:  DONE" : " A:  TRY  AGAIN    B:  BACK"));
}

static void DrawSaved(lv_obj_t *pScreen)
{
    HideLocks(pScreen);
    char t[kTextLen];
    const int n = CwlNet_SavedCount();
    if (_Ctx.SavedSel >= n + 1) _Ctx.SavedSel = n;   // n = the BACK row
    Line(pScreen, 0, true, " SAVED  NETWORKS");
    for (int r = 0; r < 4; r++) {
        const int i = (_Ctx.SavedSel > 3 ? _Ctx.SavedSel - 3 : 0) + r;
        char s[kCwlNet_SsidMax + 1];
        const char *mark = i == _Ctx.SavedSel ? ">" : " ";
        if (i < n && CwlNet_SavedSsid(i, s, sizeof(s))) snprintf(t, sizeof(t), "%s%.24s", mark, s);
        else if (i == n) snprintf(t, sizeof(t), "%sBACK", mark);
        else t[0] = '\0';
        Line(pScreen, 1 + r, i == _Ctx.SavedSel, t);
    }
    if (_Ctx.Acting) {
        static const char *const kAct[] = {"JOIN", "FORGET", "BACK"};
        snprintf(t, sizeof(t), " <  %s  >    A:  OK", kAct[_Ctx.SavedAct]);
        Line(pScreen, 5, true, t);
    } else {
        Line(pScreen, 5, false, " A:  CHOOSE    B:  BACK");
    }
}

// The QR canvas, drawn by wifi_qr.c (lv_qrcode_create only sets up the
// 1-bit canvas and its palette: index 0 dark, 1 light).
static void DrawQr(lv_obj_t *pScreen, const char *Text)
{
    if (_Ctx.pQr == NULL) {
        _Ctx.pQr = lv_qrcode_create(pScreen, kQrSize_px, lv_color_black(), lv_color_white());
        lv_obj_align(_Ctx.pQr, LV_ALIGN_TOP_LEFT, kQrX_px, kQrY_px);
        lv_obj_move_foreground(_Ctx.pQr);
        _Ctx.QrData[0] = '\0';
    }
    if (strcmp(Text, _Ctx.QrData) == 0) return;
    snprintf(_Ctx.QrData, sizeof(_Ctx.QrData), "%s", Text);
    lv_img_dsc_t *img = lv_canvas_get_img(_Ctx.pQr);
    uint8_t *buf = (uint8_t *)img->data + 8;   // after the 2-colour palette
    (void)WifiQr_Render(_Ctx.QrData, buf, kQrSize_px, (kQrSize_px + 7) / 8);
    lv_obj_invalidate(_Ctx.pQr);
}

static void PhoneStatus(const CwlNetPortal_t *p, char *t, size_t n)
{
    if (p->Joining) snprintf(t, n, " TRYING  %.10s", p->JoinSsid);
    else if (p->Result != kCwlNetRes_None) snprintf(t, n, " %s", CwlNet_ResultText(p->Result));
    else if (p->Clients) snprintf(t, n, " A  PHONE  JOINED");
    else snprintf(t, n, " READY  FOR  A  PHONE");
}

// SET UP WITH PHONE (0.17.2): a first screen to choose SCAN A QR CODE or
// TYPE IT IN, each with its own screen and B: BACK to the choice; B on the
// choice stops the setup network. What the phone does (joined, trying, the
// result) shows on the choice screen, which the other two return to when it
// changes. Widths in fingfai, x 77 + n, panel edge 148: all rows <= 147.
static void DrawPhone(lv_obj_t *pScreen)
{
    HideLocks(pScreen);
    CwlNetPortal_t p;
    CwlNet_PortalState(&p);
    char t[kTextLen + 40];
    if (!p.Active) {
        DeleteQr();
        const bool Starting = CwlNet_Op() == kCwlNetOp_Portal;
        Line(pScreen, 0, true, Starting ? " STARTING..." : " PHONE  SETUP  CLOSED");
        Line(pScreen, 1, false, "");
        if (!Starting && p.Result == kCwlNetRes_Ok) snprintf(t, sizeof(t), " JOINED  %.18s", p.JoinSsid);
        else t[0] = '\0';
        Line(pScreen, 2, false, t);
        Line(pScreen, 3, false, "");
        Line(pScreen, 4, false, "");
        Line(pScreen, 5, false, Starting ? "" : " B:  BACK");
        Line(pScreen, 6, false, "");
        return;
    }
    if (p.Joining != _Ctx.PhSeenJoining || p.Result != _Ctx.PhSeenResult) {
        _Ctx.PhSeenJoining = p.Joining;
        _Ctx.PhSeenResult = p.Result;
        _Ctx.PhoneMode = kPh_Choose;   // the phone sent a network: show what happens
    }
    const unsigned long m = (unsigned long)(p.SecondsLeft / 60), sec = (unsigned long)(p.SecondsLeft % 60);
    if (_Ctx.PhoneMode == kPh_Qr) {
        for (int i = 0; i < 6; i++) Line(pScreen, i, false, "");
        snprintf(t, sizeof(t), " B:  BACK    %lu:%02lu", m, sec);
        Line(pScreen, 6, false, t);
        snprintf(t, sizeof(t), "WIFI:T:WPA;S:%s;P:%s;;", p.Ssid, p.Pass);
        DrawQr(pScreen, t);
        return;
    }
    DeleteQr();
    Line(pScreen, 6, false, "");
    if (_Ctx.PhoneMode == kPh_Type) {
        Line(pScreen, 0, false, " ON  THE  PHONE,  JOIN");
        snprintf(t, sizeof(t), " %s", p.Ssid);
        Line(pScreen, 1, true, t);
        Line(pScreen, 2, false, " PASSWORD  (CAPS)");
        snprintf(t, sizeof(t), " %s", p.Pass);
        Line(pScreen, 3, true, t);
        Line(pScreen, 4, false, " THEN OPEN 192.168.4.1");
        snprintf(t, sizeof(t), " B:  BACK    %lu:%02lu", m, sec);
        Line(pScreen, 5, false, t);
        return;
    }
    Line(pScreen, 0, false, " PHONE  SETUP");
    Line(pScreen, 1, _Ctx.PhoneSel == 0, _Ctx.PhoneSel == 0 ? ">SCAN  A  QR  CODE" : " SCAN  A  QR  CODE");
    Line(pScreen, 2, _Ctx.PhoneSel == 1, _Ctx.PhoneSel == 1 ? ">TYPE  IT  IN" : " TYPE  IT  IN");
    Line(pScreen, 3, false, "");
    PhoneStatus(&p, t, sizeof(t));
    Line(pScreen, 4, false, t);
    snprintf(t, sizeof(t), " A:  OK    B:  STOP  %lu:%02lu", m, sec);
    Line(pScreen, 5, false, t);
}

OSD_Result_t WifiNet_Draw(void *arg)
{
    if (arg == NULL) return kOSD_Result_Err_NullDataPtr;
    lv_obj_t *const pScreen = (lv_obj_t *const) arg;
    if (_Ctx.View != kV_Phone) Line(pScreen, 6, false, "");   // row 6 is the phone QR's
    switch (_Ctx.View) {
    case kV_Main: HideLocks(pScreen); DrawMain(pScreen); break;
    case kV_List: DrawList(pScreen); break;
    case kV_Keys: DrawKeys(pScreen); break;
    case kV_Joining: DrawJoining(pScreen); break;
    case kV_Saved: DrawSaved(pScreen); break;
    case kV_Phone: DrawPhone(pScreen); break;
    }
    return kOSD_Result_Ok;
}

// ---------------------------------------------------------------- buttons

static void OpenList(void)
{
    _Ctx.Sel = _Ctx.Top = 0;
    _Ctx.WantScan = true;
    SetView(kV_List);
}

static void PickItem(void)
{
    if (_Ctx.Sel >= NumItems()) return;
    switch (ItemKind(_Ctx.Sel)) {
    case kItem_Ap: {
        const CwlNetAp_t *a = ItemAp(_Ctx.Sel);
        snprintf(_Ctx.Ssid, sizeof(_Ctx.Ssid), "%s", a->Ssid);
        ClearPassword();
        if (a->Saved) StartJoin(NULL);                 // its saved password
        else if (!a->Secure) StartJoin("");
        else { _Ctx.Page = 0; _Ctx.KRow = 0; _Ctx.KCol = 0; SetView(kV_Keys); }
        break;
    }
    case kItem_Saved: _Ctx.SavedSel = 0; _Ctx.Acting = false; SetView(kV_Saved); break;
    case kItem_Phone:
        if (CwlNet_PortalStart() || CwlNet_Op() == kCwlNetOp_Portal) {
            _Ctx.PhoneMode = kPh_Choose;
            _Ctx.PhoneSel = 0;
            _Ctx.PhSeenJoining = false;
            _Ctx.PhSeenResult = kCwlNetRes_None;
            SetView(kV_Phone);
        }
        break;
    case kItem_Rescan: _Ctx.WantScan = true; break;
    }
}

static void TypeKey(void)
{
    const size_t n = strlen(_Ctx.Pw);
    if (_Ctx.KRow < 3) {
        const char *row = kPages[_Ctx.Page][_Ctx.KRow];
        if (_Ctx.KCol < (int)strlen(row) && n < kCwlNet_PassMax) { _Ctx.Pw[n] = row[_Ctx.KCol]; _Ctx.Pw[n + 1] = '\0'; }
        return;
    }
    switch (_Ctx.KCol) {
    case kFuncPage:
        _Ctx.Page = (_Ctx.Page + 1) % 4;
        break;
    case kFuncSpace:
        if (n < kCwlNet_PassMax) { _Ctx.Pw[n] = ' '; _Ctx.Pw[n + 1] = '\0'; }
        break;
    case kFuncShow:
        _Ctx.Show = !_Ctx.Show;
        break;
    case kFuncDel:
        if (n) _Ctx.Pw[n - 1] = '\0';
        break;
    case kFuncOk:
        if (n >= 8 || n == 0) StartJoin(_Ctx.Pw);      // WPA needs 8+; empty: try it open
        break;
    }
}

static void ClampCol(void)
{
    const int len = RowLen(_Ctx.KRow);
    if (len == 0) _Ctx.KCol = 0;
    else if (_Ctx.KCol >= len) _Ctx.KCol = len - 1;
}

static OSD_Result_t OnCapture(const Button_t Button, const ButtonState_t State, void *arg)
{
    (void)arg;
    if (State != kButtonState_Pressed) return kOSD_Result_Ok;
    switch (_Ctx.View) {
    case kV_List: {
        const int n = NumItems();
        if (Button == kButton_Up) _Ctx.Sel = (_Ctx.Sel + n - 1) % n;
        else if (Button == kButton_Down) _Ctx.Sel = (_Ctx.Sel + 1) % n;
        else if (Button == kButton_A) PickItem();
        else if (Button == kButton_B) SetView(kV_Main);
        break;
    }
    case kV_Keys:
        if (Button == kButton_Up) {
            do { _Ctx.KRow = (_Ctx.KRow + 3) % 4; } while (RowLen(_Ctx.KRow) == 0);
            ClampCol();
        } else if (Button == kButton_Down) {
            do { _Ctx.KRow = (_Ctx.KRow + 1) % 4; } while (RowLen(_Ctx.KRow) == 0);
            ClampCol();
        } else if (Button == kButton_Left) {
            const int len = RowLen(_Ctx.KRow);
            _Ctx.KCol = (_Ctx.KCol + len - 1) % len;
        } else if (Button == kButton_Right) {
            _Ctx.KCol = (_Ctx.KCol + 1) % RowLen(_Ctx.KRow);
        } else if (Button == kButton_A) {
            TypeKey();
            if (RowLen(_Ctx.KRow) == 0) { _Ctx.KRow = 3; }
            ClampCol();
        } else if (Button == kButton_B) {
            const size_t n = strlen(_Ctx.Pw);
            if (n) _Ctx.Pw[n - 1] = '\0';
            else { ClearPassword(); SetView(kV_List); }
        }
        break;
    case kV_Joining:
        if (CwlNet_Op() == kCwlNetOp_Joining) break;
        if (CwlNet_LastResult() == kCwlNetRes_Ok) { if (Button == kButton_A || Button == kButton_B) SetView(kV_Main); }
        else if (Button == kButton_A) { ClearPassword(); _Ctx.Page = 0; _Ctx.KRow = 0; _Ctx.KCol = 0; SetView(kV_Keys); }
        else if (Button == kButton_B) SetView(kV_List);
        break;
    case kV_Saved: {
        const int n = CwlNet_SavedCount();
        if (_Ctx.Acting) {
            if (Button == kButton_Left) _Ctx.SavedAct = (_Ctx.SavedAct + 2) % 3;
            else if (Button == kButton_Right) _Ctx.SavedAct = (_Ctx.SavedAct + 1) % 3;
            else if (Button == kButton_B) _Ctx.Acting = false;
            else if (Button == kButton_A) {
                _Ctx.Acting = false;
                if (_Ctx.SavedAct == 0 && CwlNet_SavedSsid(_Ctx.SavedSel, _Ctx.Ssid, sizeof(_Ctx.Ssid))) StartJoin(NULL);
                else if (_Ctx.SavedAct == 1) CwlNet_Forget(_Ctx.SavedSel);
            }
            break;
        }
        if (Button == kButton_Up) _Ctx.SavedSel = (_Ctx.SavedSel + n) % (n + 1);
        else if (Button == kButton_Down) _Ctx.SavedSel = (_Ctx.SavedSel + 1) % (n + 1);
        else if (Button == kButton_B) SetView(kV_List);
        else if (Button == kButton_A) {
            if (_Ctx.SavedSel >= n) SetView(kV_List);
            else { _Ctx.Acting = true; _Ctx.SavedAct = 0; }
        }
        break;
    }
    case kV_Phone: {
        CwlNetPortal_t p;
        CwlNet_PortalState(&p);
        if (!p.Active) {
            if (Button == kButton_B) { CwlNet_PortalStop(); SetView(kV_List); }
        } else if (_Ctx.PhoneMode != kPh_Choose) {
            if (Button == kButton_B) { _Ctx.PhoneMode = kPh_Choose; DeleteQr(); }
        } else if (Button == kButton_Up || Button == kButton_Down) {
            _Ctx.PhoneSel = 1 - _Ctx.PhoneSel;
        } else if (Button == kButton_A) {
            _Ctx.PhoneMode = _Ctx.PhoneSel == 0 ? kPh_Qr : kPh_Type;
        } else if (Button == kButton_B) {
            CwlNet_PortalStop();   // the screen shows CLOSED; B again: back to the list
        }
        break;
    }
    default:
        break;
    }
    return kOSD_Result_Ok;
}

OSD_Result_t WifiNet_OnButton(const Button_t Button, const ButtonState_t State, void *arg)
{
    (void)arg;
    if (State == kButtonState_Pressed && Button == kButton_A && _Ctx.View == kV_Main) OpenList();
    return kOSD_Result_Ok;
}

OSD_Result_t WifiNet_OnTransition(void *arg)
{
    (void)arg;
    ClearPassword();
    if (_Ctx.View != kV_Main) SetView(kV_Main);   // the phone setup keeps running (it times out)
    DeleteAll();
    return kOSD_Result_Ok;
}
