#include "network.h"

#include "cwl_online.h"
#include "cwl_relay.h"
#include "menu_mgr.h"
#include "nw_dev.h"
#include "play_help.h"
#include "esp_log.h"
#include "osd_shared.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

LV_IMG_DECLARE(img_toggle_on);
LV_IMG_DECLARE(img_toggle_off);

// SYSTEM > NETWORK: the network window service (games reach the Shiny Gen
// gateway through FF58-FF5F). Same layout as WI-FI LINK.
//
// While a Shiny Gen game with SHLINK v1.1 runs (lane chromshlink1), the item
// is that game's ROOM MENU instead: HOST (shows the code a friend types in
// the Shiny Gen app or on another Chromatic), JOIN CODE (six characters with
// the D-pad), MY DEVICES (the room this account's phone or other Chromatic is
// in, else a new one), LEAVE. A game's "play online" (lk_ask) opens the OSD
// right here. The service on/off switch is the same item when no such game runs.
//
// B on this page (either view) opens HOW TO PLAY ONLINE (0.17.7). Since 0.17.9 it is six
// short pages in a row (A or DOWN: next, UP: back a page, B: close, A on the last page:
// close), the last one pointing at shinyrelay.com/docs (play_help.c; the index of 0.17.7
// is still drawn if the generator ever makes more than one section). The room
// menu also asks the one-time question before a public party match
// (PUBLIC MATCH / YOUR NAME WILL BE VISIBLE; nw_dev.c, party_ask.h).
enum {
    kToggleBtnX_px = 77,
    kToggleBtnY_px = 55,

    kMsgStateOnX_px = kToggleBtnX_px + 14,
    kMsgStateOff_px = kToggleBtnX_px + 44,
    kMsgStateY_px   = kToggleBtnY_px + 12,

    kStatusX_px  = kToggleBtnX_px,
    kStatusY_px  = kToggleBtnY_px + 32,
    kDetailY_px  = kStatusY_px + 10,

    kTextLen = 24,

    kRoomX_px = 77,
    kRoomY_px = 46,
    kRow_px = 9,
    kRoomLabels = 7,   // the help pages use all seven rows
};

typedef enum { kRow_Host, kRow_Join, kRow_Mine, kRow_Leave, kNumRows } Row_t;

typedef struct Network {
    lv_obj_t* pMsgStateObj;
    lv_obj_t* pImgToggleOffObj;
    lv_obj_t* pImgToggleOnObj;
    lv_obj_t* pStatusObj;
    lv_obj_t* pDetailObj;
    bool DrawnOn;
    char Status[kTextLen];
    char Detail[kTextLen];
    // the room menu
    bool RoomView;
    lv_obj_t *pLabel[kRoomLabels];
    char Text[kRoomLabels][kTextLen];
    lv_style_t *pStyle[kRoomLabels];
    int Y[kRoomLabels];
    bool Editing, CodeEdit;
    bool Consent;              // the public-match question has the buttons
    int Help;                  // 0 off, 1 the index, 2 a page
    int HelpSec, HelpPage;
    bool HelpFromEditing;
    Row_t Row;
    char Code[kSl_CodeLen + 1];
    int Cur;
} Network_t;

static Network_t _Ctx;
static volatile bool _OpenWanted;   // the game asked for the room menu

static OSD_Result_t OnCapture(const Button_t Button, const ButtonState_t State, void *arg);
static OSD_Result_t OnHelpCapture(const Button_t Button, const ButtonState_t State, void *arg);

static void SetLabel(lv_obj_t* pObj, char* pCache, const char* pText)
{
    if (strncmp(pCache, pText, kTextLen) != 0)
    {
        strncpy(pCache, pText, kTextLen - 1);
        pCache[kTextLen - 1] = '\0';
        lv_label_set_text(pObj, pCache);
    }
}

static void DeleteToggle(void)
{
    lv_obj_t** ToDelete[] = {
        &_Ctx.pImgToggleOffObj,
        &_Ctx.pImgToggleOnObj,
        &_Ctx.pMsgStateObj,
        &_Ctx.pStatusObj,
        &_Ctx.pDetailObj,
    };
    for (size_t i = 0; i < ARRAY_SIZE(ToDelete); i++)
    {
        if (*ToDelete[i] != NULL)
        {
            lv_obj_del(*ToDelete[i]);
            *(ToDelete[i]) = NULL;
        }
    }
}

static void DeleteRoom(void)
{
    for (int i = 0; i < kRoomLabels; i++)
    {
        if (_Ctx.pLabel[i] != NULL)
        {
            lv_obj_del(_Ctx.pLabel[i]);
            _Ctx.pLabel[i] = NULL;
        }
    }
}

static void Line(lv_obj_t *pScreen, int i, int y, bool White, const char *Text)
{
    lv_style_t *pStyle = White ? OSD_GetStyleTextWhite() : OSD_GetStyleTextGrey();
    if (_Ctx.pLabel[i] == NULL)
    {
        _Ctx.pLabel[i] = lv_label_create(pScreen);
        lv_obj_align(_Ctx.pLabel[i], LV_ALIGN_TOP_LEFT, kRoomX_px, kRoomY_px + y);
        _Ctx.Y[i] = y;
        _Ctx.Text[i][0] = '\x01';
        _Ctx.pStyle[i] = NULL;
        lv_obj_move_foreground(_Ctx.pLabel[i]);
    }
    else if (_Ctx.Y[i] != y)
    {
        // the same row in another layout (room menu, help, question)
        lv_obj_align(_Ctx.pLabel[i], LV_ALIGN_TOP_LEFT, kRoomX_px, kRoomY_px + y);
        _Ctx.Y[i] = y;
    }
    if (_Ctx.pStyle[i] != pStyle)
    {
        if (_Ctx.pStyle[i] != NULL) lv_obj_remove_style(_Ctx.pLabel[i], _Ctx.pStyle[i], 0);
        lv_obj_add_style(_Ctx.pLabel[i], pStyle, 0);
        _Ctx.pStyle[i] = pStyle;
    }
    SetLabel(_Ctx.pLabel[i], _Ctx.Text[i], Text);
}

static void SetEditing(bool On)
{
    _Ctx.Editing = On;
    _Ctx.CodeEdit = false;
    _Ctx.Consent = false;
    MenuMgr_SetCapture(On ? OnCapture : NULL);
}

static void OpenHelp(void)
{
    _Ctx.HelpFromEditing = _Ctx.Editing;
    _Ctx.Editing = false;
    _Ctx.CodeEdit = false;
    // 0.17.9: one short run of pages (no index) when the help has a single section
    _Ctx.Help = g_PlayHelpNumSections > 1 ? 1 : 2;
    _Ctx.HelpSec = 0;
    _Ctx.HelpPage = 0;
    DeleteToggle();
    DeleteRoom();
    MenuMgr_SetCapture(OnHelpCapture);
}

static void CloseHelp(void)
{
    _Ctx.Help = 0;
    DeleteRoom();
    DeleteToggle();
    _Ctx.RoomView = false;   // Network_Draw sets the view up again
    MenuMgr_SetCapture(NULL);
}

static void DrawHelp(lv_obj_t *pScreen)
{
    char t[kTextLen];
    if (_Ctx.Help == 1)
    {
        Line(pScreen, 0, 0, true, " HOW  TO  PLAY  ONLINE");
        for (int i = 0; i < 5; i++)
        {
            if (i < g_PlayHelpNumSections)
            {
                snprintf(t, sizeof(t), "%s%s", i == _Ctx.HelpSec ? ">" : " ", g_PlayHelpSections[i].Name);
                Line(pScreen, 1 + i, kRow_px * (1 + i), i == _Ctx.HelpSec, t);
            }
            else
            {
                Line(pScreen, 1 + i, kRow_px * (1 + i), false, "");
            }
        }
        Line(pScreen, 6, kRow_px * 6, true, " A:  OPEN   B:  BACK");
        return;
    }
    const char *const *pg = g_PlayHelp[g_PlayHelpSections[_Ctx.HelpSec].First + _Ctx.HelpPage];
    for (int i = 0; i < kPlayHelpRows; i++)
    {
        const bool White = pg[i][0] == '!';
        Line(pScreen, i, kRow_px * i, White, White ? pg[i] + 1 : pg[i]);
    }
}

static OSD_Result_t OnHelpCapture(const Button_t Button, const ButtonState_t State, void *arg)
{
    (void)arg;
    if (State != kButtonState_Pressed) return kOSD_Result_Ok;
    const int n = g_PlayHelpNumSections;
    if (_Ctx.Help == 1)
    {
        switch (Button)
        {
        case kButton_Up: _Ctx.HelpSec = (_Ctx.HelpSec + n - 1) % n; break;
        case kButton_Down: _Ctx.HelpSec = (_Ctx.HelpSec + 1) % n; break;
        case kButton_A: _Ctx.Help = 2; _Ctx.HelpPage = 0; break;
        case kButton_B: CloseHelp(); break;
        default: break;
        }
        return kOSD_Result_Ok;
    }
    const int pages = g_PlayHelpSections[_Ctx.HelpSec].Pages;
    const bool Index = n > 1;   // with one section, leaving the pages closes the help
    switch (Button)
    {
    case kButton_A:
    case kButton_Down:
        if (_Ctx.HelpPage + 1 < pages) _Ctx.HelpPage++;
        else if (Index) _Ctx.Help = 1;
        else if (Button == kButton_A) CloseHelp();   // A on the last page; Down stays on it
        break;
    case kButton_Up:
        if (_Ctx.HelpPage > 0) _Ctx.HelpPage--;
        else if (Index) _Ctx.Help = 1;
        break;
    case kButton_B:
        if (Index) _Ctx.Help = 1;
        else CloseHelp();
        break;
    default: break;
    }
    return kOSD_Result_Ok;
}

// What the room is doing, in one line (and the code to give a friend).
static void RoomStatus(const NwRoomInfo_t *r, char *Status, char *Detail)
{
    Detail[0] = '\0';
    if (r->AskBusy)
    {
        snprintf(Status, kTextLen, "SAVING...");
    }
    else if (r->AskFailed)
    {
        snprintf(Status, kTextLen, "NO  ROOM");
        snprintf(Detail, kTextLen, "TRY  AGAIN  LATER");
    }
    else if (r->InRoom)
    {
        snprintf(Status, kTextLen, "%s  %s", r->Host ? "HOSTING" : "IN  ROOM", r->Code);
        snprintf(Detail, kTextLen, "PLAYERS  %u  OF  %u", (unsigned)r->Players, (unsigned)(r->MaxPeers ? r->MaxPeers : 2));
    }
    else if (r->State == 1 || r->State == 2)
    {
        snprintf(Status, kTextLen, "JOINING...");
    }
    else if (r->State == 4 && r->Ended[0])
    {
        snprintf(Status, kTextLen, "NO  ROOM");
        if (!strcmp(r->Ended, "no_room")) snprintf(Detail, kTextLen, "NO  SUCH  CODE");
        else if (!strcmp(r->Ended, "room_full")) snprintf(Detail, kTextLen, "THE  ROOM  IS  FULL");
        else if (!strcmp(r->Ended, "room_closed")) snprintf(Detail, kTextLen, "THE  ROOM  CLOSED");
        else if (!strcmp(r->Ended, "party_consent")) snprintf(Detail, kTextLen, "NEEDS  YOUR  OK");
        else snprintf(Detail, kTextLen, "%s", r->Ended);
    }
    else
    {
        snprintf(Status, kTextLen, CwlOnline_Paired() ? "NOT  IN  A  ROOM" : "PAIR  IN  ONLINE  FIRST");
    }
}

static void RowText(Row_t r, char *Buf)
{
    const char *mark = (_Ctx.Editing && _Ctx.Row == r) ? ">" : " ";
    switch (r)
    {
    case kRow_Host: snprintf(Buf, kTextLen, "%sHOST", mark); break;
    case kRow_Join:
        if (_Ctx.CodeEdit)
        {
            char c[16] = "";
            int n = 0;
            for (int i = 0; i < kSl_CodeLen; i++)
            {
                if (i == _Ctx.Cur) c[n++] = '[';
                c[n++] = _Ctx.Code[i];
                if (i == _Ctx.Cur) c[n++] = ']';
            }
            c[n] = '\0';
            snprintf(Buf, kTextLen, "%sJOIN %s", mark, c);
        }
        else
        {
            snprintf(Buf, kTextLen, "%sJOIN  CODE", mark);
        }
        break;
    case kRow_Mine: snprintf(Buf, kTextLen, "%sMY  DEVICES", mark); break;
    case kRow_Leave: snprintf(Buf, kTextLen, "%sLEAVE", mark); break;
    default: Buf[0] = '\0';
    }
}

static OSD_Result_t DrawRoom(lv_obj_t *pScreen)
{
    NwRoomInfo_t r;
    NW_GetRoomInfo(&r);
    char st[kTextLen], de[kTextLen], t[kTextLen];
    RoomStatus(&r, st, de);
    if (r.AskConsent)
    {
        // the one-time question before a public party match (PARTY_ROOMS.md 8.4)
        _OpenWanted = false;
        if (!_Ctx.Consent)
        {
            SetEditing(true);
            _Ctx.Consent = true;
        }
        Line(pScreen, 0, 0, true, "PUBLIC  MATCH");
        Line(pScreen, 1, kRow_px, false, "YOUR  NAME  WILL");
        Line(pScreen, 2, kRow_px * 2, false, "BE  VISIBLE");
        Line(pScreen, 3, kRow_px * 3, true, "A:  OK");
        Line(pScreen, 4, kRow_px * 4, true, "SELECT:");
        Line(pScreen, 5, kRow_px * 5, true, "GENERIC  NAME");
        Line(pScreen, 6, kRow_px * 6, true, "B:  BACK");
        return kOSD_Result_Ok;
    }
    if (_Ctx.Consent) SetEditing(false);   // answered (or the question went away)
    if (_OpenWanted)
    {
        _OpenWanted = false;
        _Ctx.Row = r.InRoom ? kRow_Leave : kRow_Host;
        SetEditing(true);
    }
    if (!_Ctx.Editing)
    {
        Line(pScreen, 0, 0, true, st);
        Line(pScreen, 1, kRow_px, false, de);
        Line(pScreen, 2, kRow_px * 3, false, "GAME  ROOM");
        Line(pScreen, 3, kRow_px * 4, false, "");
        Line(pScreen, 4, kRow_px * 5, true, "A:  HOST  OR  JOIN");
        Line(pScreen, 5, kRow_px * 6, false, "B:  HOW  TO  PLAY");
        Line(pScreen, 6, kRow_px * 6, false, "");
        return kOSD_Result_Ok;
    }
    Line(pScreen, 0, 0, true, st);
    for (int i = 0; i < kNumRows; i++)
    {
        RowText((Row_t)i, t);
        Line(pScreen, 1 + i, kRow_px * (1 + i) + 2, _Ctx.Row == i, t);
    }
    Line(pScreen, 5, kRow_px * 5 + 2, false, _Ctx.CodeEdit ? "^V  <>  A:OK  B:BACK" : (r.InRoom && r.Host ? de : "A:  PICK   B:  DONE"));
    Line(pScreen, 6, kRow_px * 6, false, "");
    return kOSD_Result_Ok;
}

OSD_Result_t Network_Draw(void* arg)
{
    if (arg == NULL)
    {
        return kOSD_Result_Err_NullDataPtr;
    }

    lv_obj_t *const pScreen = (lv_obj_t *const) arg;

    if (_Ctx.Help)
    {
        DrawHelp(pScreen);
        return kOSD_Result_Ok;
    }
    const bool Room = NW_LinkAttached() || _OpenWanted;
    if (Room != _Ctx.RoomView)
    {
        _Ctx.RoomView = Room;
        DeleteToggle();
        DeleteRoom();
        if (!Room && _Ctx.Editing) SetEditing(false);
    }
    if (Room)
    {
        return DrawRoom(pScreen);
    }

    const bool IsOn = NW_IsEnabled();

    // above the switch: the way to the help (0.17.7)
    Line(pScreen, 0, 0, false, "B:  HOW  TO  PLAY");

    if (IsOn != _Ctx.DrawnOn)
    {
        DeleteToggle();   // redraw the toggle in its new position
    }
    _Ctx.DrawnOn = IsOn;

    if (_Ctx.pMsgStateObj == NULL)
    {
        _Ctx.pMsgStateObj = lv_label_create(pScreen);
        lv_obj_add_style(_Ctx.pMsgStateObj, OSD_GetStyleTextBlack(), 0);
        lv_obj_set_align(_Ctx.pMsgStateObj, LV_ALIGN_TOP_LEFT);
    }

    if (!IsOn)
    {
        if (_Ctx.pImgToggleOffObj == NULL)
        {
            _Ctx.pImgToggleOffObj = lv_img_create(pScreen);
            lv_obj_align(_Ctx.pImgToggleOffObj, LV_ALIGN_TOP_LEFT, kToggleBtnX_px, kToggleBtnY_px);
            lv_img_set_src(_Ctx.pImgToggleOffObj, &img_toggle_off);

            lv_obj_set_pos(_Ctx.pMsgStateObj, kMsgStateOff_px, kMsgStateY_px);
            lv_label_set_text_static(_Ctx.pMsgStateObj, "OFF");
        }
    }
    else
    {
        if (_Ctx.pImgToggleOnObj == NULL)
        {
            _Ctx.pImgToggleOnObj = lv_img_create(pScreen);
            lv_obj_align(_Ctx.pImgToggleOnObj, LV_ALIGN_TOP_LEFT, kToggleBtnX_px, kToggleBtnY_px);
            lv_img_set_src(_Ctx.pImgToggleOnObj, &img_toggle_on);

            lv_obj_set_pos(_Ctx.pMsgStateObj, kMsgStateOnX_px, kMsgStateY_px);
            lv_label_set_text_static(_Ctx.pMsgStateObj, "ON");
        }
    }

    lv_obj_move_foreground(_Ctx.pMsgStateObj);

    // Status: what the link is doing now, and the address peers reach it at.
    if (_Ctx.pStatusObj == NULL)
    {
        _Ctx.pStatusObj = lv_label_create(pScreen);
        lv_obj_add_style(_Ctx.pStatusObj, OSD_GetStyleTextWhite(), 0);
        lv_obj_align(_Ctx.pStatusObj, LV_ALIGN_TOP_LEFT, kStatusX_px, kStatusY_px);
        _Ctx.Status[0] = '\x01';   // force the first text
    }
    if (_Ctx.pDetailObj == NULL)
    {
        _Ctx.pDetailObj = lv_label_create(pScreen);
        lv_obj_add_style(_Ctx.pDetailObj, OSD_GetStyleTextGrey(), 0);
        lv_obj_align(_Ctx.pDetailObj, LV_ALIGN_TOP_LEFT, kStatusX_px, kDetailY_px);
        _Ctx.Detail[0] = '\x01';
    }

    char Detail[kTextLen] = "";
    NW_GetDetailText(Detail, sizeof(Detail));
    SetLabel(_Ctx.pStatusObj, _Ctx.Status, NW_StatusText());
    SetLabel(_Ctx.pDetailObj, _Ctx.Detail, Detail);
    lv_obj_move_foreground(_Ctx.pStatusObj);
    lv_obj_move_foreground(_Ctx.pDetailObj);

    return kOSD_Result_Ok;
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
    if (_Ctx.Consent)
    {
        // A: OK with the account's name; SELECT: OK with a generic name; B: back (no call)
        if (Button == kButton_A) NW_PartyAnswer(1);
        else if (Button == kButton_Select) NW_PartyAnswer(2);
        else if (Button == kButton_B) NW_PartyAnswer(0);
        else return kOSD_Result_Ok;
        SetEditing(false);
        return kOSD_Result_Ok;
    }
    if (_Ctx.CodeEdit)
    {
        switch (Button)
        {
        case kButton_Up: CodeChar(+1); break;
        case kButton_Down: CodeChar(-1); break;
        case kButton_Left: _Ctx.Cur = (_Ctx.Cur + kSl_CodeLen - 1) % kSl_CodeLen; break;
        case kButton_Right: _Ctx.Cur = (_Ctx.Cur + 1) % kSl_CodeLen; break;
        case kButton_A:
            if (Sl_ValidCode(_Ctx.Code))
            {
                NW_RoomRequest(1, _Ctx.Code);
                SetEditing(false);
            }
            break;
        case kButton_B: _Ctx.CodeEdit = false; break;
        default: break;
        }
        return kOSD_Result_Ok;
    }
    switch (Button)
    {
    case kButton_Up: _Ctx.Row = (Row_t)((_Ctx.Row + kNumRows - 1) % kNumRows); break;
    case kButton_Down: _Ctx.Row = (Row_t)((_Ctx.Row + 1) % kNumRows); break;
    case kButton_A:
        switch (_Ctx.Row)
        {
        case kRow_Host: NW_RoomRequest(0, ""); SetEditing(false); break;
        case kRow_Mine: NW_RoomRequest(2, ""); SetEditing(false); break;
        case kRow_Leave: NW_RoomRequest(100, ""); SetEditing(false); break;
        case kRow_Join:
            _Ctx.CodeEdit = true;
            _Ctx.Cur = 0;
            memset(_Ctx.Code, kSl_CodeAlphabet[0], kSl_CodeLen);
            _Ctx.Code[kSl_CodeLen] = '\0';
            break;
        default: break;
        }
        break;
    case kButton_B: SetEditing(false); break;
    default: break;
    }
    return kOSD_Result_Ok;
}

OSD_Result_t Network_OnButton(const Button_t Button, const ButtonState_t State, void* arg)
{
    (void)arg;
    if (State != kButtonState_Pressed) return kOSD_Result_Ok;
    if (Button == kButton_B)
    {
        OpenHelp();
        return kOSD_Result_Ok;
    }
    if (_Ctx.RoomView)
    {
        if (Button == kButton_A)
        {
            _Ctx.Row = kRow_Host;
            SetEditing(true);
        }
        return kOSD_Result_Ok;
    }
    if (Button == kButton_A)
    {
        NW_SetEnabled(!NW_IsEnabled());
    }
    return kOSD_Result_Ok;
}

OSD_Result_t Network_OnTransition(void* arg)
{
    (void)arg;
    if (_Ctx.Editing) SetEditing(false);
    if (_Ctx.Help) { _Ctx.Help = 0; MenuMgr_SetCapture(NULL); }
    DeleteToggle();
    DeleteRoom();
    _Ctx.RoomView = false;
    return kOSD_Result_Ok;
}

void Network_RequestRoomMenu(void)
{
    _OpenWanted = true;
}
