#include "share.h"

#include <stdio.h>
#include <string.h>

// SHARE / RECEIVE FROM A CHROMATIC (lane share1): see share.h. The words follow ui110/DESIGN.md: CARTRIDGE, ON CHROMATIC,
// CARD, PLAY NOW; no FPGA, flash or library on the screen. Same layout as SYSTEM > PLAY (seven rows of 23 characters).

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#if defined(SHARE_HOST_TEST) || defined(CONFIG_NW_CARTFLASH_WIFI)
#define SHARE_ON 1
#endif

#ifdef SHARE_ON
#include "cf_share.h"
#include "cf_share_dev.h"
#ifndef SHARE_HOST_TEST
#include "menu_mgr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
extern bool OSD_IsVisible(void);
static uint32_t NowMs(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }
static bool Visible(void) { return OSD_IsVisible(); }
#else
void MenuMgr_SetCapture(fnOSDButtonCb_t f);
uint32_t ShareHost_NowMs(void);
bool ShareHost_Visible(void);
static uint32_t NowMs(void) { return ShareHost_NowMs(); }
static bool Visible(void) { return ShareHost_Visible(); }
#endif

static struct {
    fnOSDButtonCb_t Back;
    bool Captured;
    // m110s: a result nobody dismissed (A: OK) is not shown again when a page comes back. Away: the page that drew it went
    // (Share_HostLeft), the menu was hidden, or it was not drawn for a while; OnMs: how long this result has been on screen
    bool Away;
    uint32_t LastMs, OnMs;
} _Sh = { .Away = true };

// bytes as the pages say them: "1.2  MB" / "640  KB"
static void Size(char *b, size_t n, uint32_t bytes)
{
    if (bytes >= 1024u * 1024u) snprintf(b, n, "%lu.%lu  MB", (unsigned long)(bytes >> 20), (unsigned long)((bytes & 0xFFFFFu) * 10u >> 20));
    else snprintf(b, n, "%lu  KB", (unsigned long)((bytes + 1023u) >> 10));
}

static void Set(ShareRows_t *o, int i, bool White, uint8_t Mode, const char *Text)
{
    snprintf(o->T[i], sizeof o->T[i], "%s", Text ? Text : "");
    if (White) o->White |= (uint8_t)(1u << i);
    o->Mode[i] = Mode;
}

// the rows of a view (also the host render test's)
void Share_RowsFor(const cf_share_view_t *v, ShareRows_t *o);
void Share_RowsFor(const cf_share_view_t *v, ShareRows_t *o)
{
    memset(o, 0, sizeof *o);
    char b[kShareRowLen], c[kShareRowLen], s[16];
    switch (v->kind)
    {
    case CF_SHV_A_CODE:
    {
        // the code's digits apart, the one being changed in brackets; then WITH SAVE when the game has one
        int n = 0;
        n += snprintf(b + n, sizeof b - (size_t)n, "CODE ");
        for (int i = 0; i < 4; i++) n += snprintf(b + n, sizeof b - (size_t)n, v->cur == i ? " [%c]" : "  %c ", v->code[i]);
        Set(o, 0, true, 0, "SHARE");
        Set(o, 1, false, 2, v->title);
        // lane m110u: where the other one's code is, then what to do here
        Set(o, 2, false, 0, "OTHER:  DOWNLOAD  >  B");
        Set(o, 3, false, 0, "TYPE  ITS  CODE  HERE");
        Set(o, 4, v->cur < 4, 0, b);
        if (v->read_pct >= 0) { snprintf(c, sizeof c, "READING  CART  %d%%", v->read_pct); Set(o, 5, false, 0, c); }
        else if (v->save_avail) { snprintf(c, sizeof c, v->cur == 4 ? "WITH  SAVE  [%s]" : "WITH  SAVE   %s", v->with_save ? "YES" : "NO"); Set(o, 5, v->cur == 4, 0, c); }
        Set(o, 6, true, 0, "A:  SEND   B:  BACK");
        break;
    }
    case CF_SHV_A_READ:
        snprintf(b, sizeof b, "%d%%", v->read_pct < 0 ? 100 : v->read_pct);
        Set(o, 0, true, 0, "READING  THE");
        Set(o, 1, true, 0, "CARTRIDGE");
        Set(o, 2, true, 0, b);
        Set(o, 3, false, 0, "NOTHING  IS  WRITTEN");
        Set(o, 4, false, 0, "TO  IT");
        Set(o, 6, true, 0, "B:  STOP");
        break;
    case CF_SHV_A_FIND:
        snprintf(b, sizeof b, "CODE  %s", v->code);
        Set(o, 0, true, 0, "LOOKING  FOR  THE");
        Set(o, 1, true, 0, "OTHER  CHROMATIC");
        Set(o, 2, false, 0, b);
        Set(o, 3, false, 0, "KEEP  THEM  CLOSE");
        Set(o, 6, true, 0, "B:  STOP");
        break;
    case CF_SHV_A_ASKED:
        Set(o, 0, true, 0, "FOUND");
        Set(o, 1, true, 0, v->peer);
        Set(o, 2, false, 0, "IT  ASKS  WHERE  TO");
        Set(o, 3, false, 0, "KEEP  THE  GAME");
        Set(o, 6, true, 0, "B:  STOP");
        break;
    case CF_SHV_A_SEND:
    case CF_SHV_B_RECV:
    {
        const bool a = v->kind == CF_SHV_A_SEND;
        snprintf(b, sizeof b, "%d%%", v->pct);
        Set(o, 0, true, 0, a ? "SENDING" : "RECEIVING");
        Set(o, 1, false, 2, v->title);
        Set(o, 2, true, 0, b);
        if (v->paused)
        {
            snprintf(c, sizeof c, "WAITING  %d  S", v->paused);
            Set(o, 3, true, 0, "OUT  OF  REACH");
            Set(o, 4, false, 0, c);
        }
        else
        {
            snprintf(c, sizeof c, "%lu  KB/S", (unsigned long)v->kbps);
            Set(o, 3, false, 0, v->kbps ? c : "");
            Set(o, 4, false, 0, a ? "KEEP  THEM  CLOSE" : v->dest == CF_SH_DEST_LIB ? "ON  CHROMATIC" : v->dest == CF_SH_DEST_CARD ? "TO  THE  CARD" : "TO  PLAY  NOW");
        }
        Set(o, 6, true, 0, "B:  STOP");
        break;
    }
    case CF_SHV_A_END:
        Set(o, 0, true, 0, "ALL  SENT");
        Set(o, 1, false, 2, v->title);
        Set(o, 2, false, 0, "THE  OTHER  ONE");
        Set(o, 3, false, 0, "CHECKS  AND  KEEPS  IT");
        break;
    case CF_SHV_A_RESULT:
    case CF_SHV_B_RESULT:
        Set(o, 0, true, 0, v->ok ? (v->kind == CF_SHV_A_RESULT ? "SENT" : "RECEIVED") : (v->kind == CF_SHV_A_RESULT ? "NOT  SENT" : "NOT  RECEIVED"));
        Set(o, 1, false, 2, v->title);
        Set(o, 2, false, 0, v->r1);
        Set(o, 3, false, 0, v->r2);
        // lane m110u: a game received with its save still says how to play it
        if (v->ok && v->kind == CF_SHV_B_RESULT && !strcmp(v->r2, "WITH  ITS  SAVE")) Set(o, 4, false, 0, "PLAY  IT  FROM  GAMES");
        Set(o, 6, true, 0, "A:  OK");
        break;
    case CF_SHV_B_START:
        Set(o, 0, true, 0, "RECEIVE  FROM  A");
        Set(o, 1, true, 0, "CHROMATIC");
        Set(o, 2, false, 0, "GETTING  READY");
        Set(o, 6, true, 0, "B:  CANCEL");
        break;
    case CF_SHV_B_CODE:
    {
        snprintf(b, sizeof b, "%c   %c   %c   %c", v->code[0], v->code[1], v->code[2], v->code[3]);
        Set(o, 0, false, 0, "YOUR  CODE");
        Set(o, 1, true, 0, b);
        Set(o, 2, false, 0, "ON  THE  OTHER  ONE:");
        Set(o, 3, false, 0, "SHARE  A  GAME,  THEN");
        Set(o, 4, false, 0, "TYPE  THIS  CODE");
        Set(o, 6, true, 0, "B:  CANCEL");
        break;
    }
    case CF_SHV_B_OFFER:
    {
        static const char *const kPlace[3] = { "ON  CHROMATIC", "CARD", "PLAY  NOW" };
        Size(s, sizeof s, v->rom_size ? v->rom_size : v->size);
        snprintf(b, sizeof b, "%s%s", s, v->save_avail ? ",  WITH  SAVE" : "");
        Set(o, 0, true, 0, v->peer);
        Set(o, 1, false, 2, v->title);
        Set(o, 2, false, 0, b);
        for (int d = 0; d < 3; d++)
        {
            if (v->place_ok[d]) snprintf(c, sizeof c, "%s%s", v->sel == d ? ">" : " ", kPlace[d]);
            else if (!v->place_why[d] || !v->place_why[d][0]) snprintf(c, sizeof c, " %s", kPlace[d]);
            else snprintf(c, sizeof c, d == 0 ? " %s: %s" : " %s:  %s", kPlace[d], v->place_why[d]);   // (ON CHROMATIC: FULL fits with one space)
            Set(o, 3 + d, v->place_ok[d] && v->sel == d, 0, c);
        }
        Set(o, 6, true, 0, "A:  TAKE  IT  B:  NO");
        break;
    }
    case CF_SHV_B_KEEP:
        Set(o, 0, true, 0, "ALL  CAME:  CHECKED");
        Set(o, 1, false, 2, v->title);
        Set(o, 2, false, 0, "KEEPING  IT");
        Set(o, 3, false, 0, "DO  NOT  TURN  OFF");
        break;
    case CF_SHV_B_AFTER:
        snprintf(b, sizeof b, "%d%%", v->pct);
        Set(o, 0, true, 0, v->dest == CF_SH_DEST_CARD ? "PUTTING  IT  ON" : "STARTING");
        Set(o, 1, v->dest == CF_SH_DEST_CARD, v->dest == CF_SH_DEST_CARD ? 0 : 2, v->dest == CF_SH_DEST_CARD ? "THE  CARD" : v->title);
        Set(o, 2, true, 0, b);
        Set(o, 3, false, 0, "DO  NOT  TURN  OFF");
        break;
    default:
        break;
    }
}

static OSD_Result_t OnCapture(const Button_t Button, const ButtonState_t State, void *arg)
{
    (void)arg;
    if (State != kButtonState_Pressed) return kOSD_Result_Ok;
    int b = 0;
    switch (Button)
    {
    case kButton_A: b = CF_SHB_A; break;
    case kButton_B: b = CF_SHB_B; break;
    case kButton_Up: b = CF_SHB_UP; break;
    case kButton_Down: b = CF_SHB_DOWN; break;
#ifndef SHARE_HOST_TEST
    case kButton_Left: b = CF_SHB_LEFT; break;
    case kButton_Right: b = CF_SHB_RIGHT; break;
#endif
    default: break;
    }
    if (b) CfShare_Button(b);
    return kOSD_Result_Ok;
}

static void Capture(fnOSDButtonCb_t Back)
{
    _Sh.Back = Back;
    _Sh.Captured = true;
    MenuMgr_SetCapture(OnCapture);
}

bool Share_Available(void) { return true; }
static int Why(int e) { return e == 0 ? kShare_Ok : e == -2 ? kShare_NoMemory : kShare_Busy; }
int Share_SendStart(ShareSrc_t Src, uint32_t Uid, const char *Path, const char *Title, fnOSDButtonCb_t Back)
{
    static const int kSrc[3] = { CF_SH_SRC_CART, CF_SH_SRC_LIB, CF_SH_SRC_CARD };
    const int e = Why(CfShare_SendStart(kSrc[Src], Uid, Path, Title));
    if (e == kShare_Ok) Capture(Back);
    return e;
}
int Share_RecvStart(fnOSDButtonCb_t Back)
{
    const int e = Why(CfShare_RecvStart());
    if (e == kShare_Ok) Capture(Back);
    return e;
}
// a finished share's result that is not shown again when a page comes back: one already on screen a second or more, or a
// PLAY NOW that plays (the game started at once: the menu closed over it, and the game is the result)
static bool Stale(const cf_share_view_t *v, uint32_t now)
{
    if (v->kind != CF_SHV_A_RESULT && v->kind != CF_SHV_B_RESULT) return false;
    if (v->kind == CF_SHV_B_RESULT && v->ok && v->dest == CF_SH_DEST_PLAY) return true;
    (void)now;
    return _Sh.OnMs >= 1000u;
}
bool Share_Rows(ShareRows_t *Out)
{
    const uint32_t now = NowMs();
    const bool vis = Visible();
    const uint32_t dt = now - _Sh.LastMs;
    const bool entry = _Sh.Away || dt > 5000u;   // (a gap: drawn by another page meanwhile; long enough that a busy moment is not one)
    _Sh.Away = !vis;
    _Sh.LastMs = now;
    if (CfShare_Active())
    {
        cf_share_view_t v;
        CfShare_View(&v);
        const bool res = v.kind == CF_SHV_A_RESULT || v.kind == CF_SHV_B_RESULT;
        if (!res) _Sh.OnMs = 0;
        if (res && vis && entry && Stale(&v, now)) CfShare_Close();   // (m110f: the next DOWNLOAD showed the old result, B did not receive)
        else
        {
            if (res && vis && !entry) _Sh.OnMs += dt;
            if (!_Sh.Captured) Capture(NULL);   // (shown again on SYSTEM > DOWNLOAD after the page that started it went away)
            Share_RowsFor(&v, Out);
            return true;
        }
    }
    _Sh.OnMs = 0;
    // the result was seen (A: OK): the buttons back to the page that started it
    if (_Sh.Captured) { _Sh.Captured = false; MenuMgr_SetCapture(_Sh.Back); _Sh.Back = NULL; }
    return false;
}
void Share_HostLeft(void)
{
    _Sh.Away = true;
    if (_Sh.Captured) { _Sh.Captured = false; _Sh.Back = NULL; MenuMgr_SetCapture(NULL); }
}
#else
bool Share_Available(void) { return false; }
int Share_SendStart(ShareSrc_t Src, uint32_t Uid, const char *Path, const char *Title, fnOSDButtonCb_t Back) { (void)Src; (void)Uid; (void)Path; (void)Title; (void)Back; return kShare_Busy; }
int Share_RecvStart(fnOSDButtonCb_t Back) { (void)Back; return kShare_Busy; }
bool Share_Rows(ShareRows_t *Out) { (void)Out; return false; }
void Share_HostLeft(void) {}
#endif
