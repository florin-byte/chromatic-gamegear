#include "getgames.h"

#include "menu_mgr.h"
#include "nw_dev.h"
#include "osd_shared.h"
#include "sd.h"
#include "lvgl.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// SYSTEM > GET GAMES (lane ui110dev; lane mygames1: MY GAMES and the four places. Shiny Relay 1.1.0, ../ui110/DESIGN.md):
// games got on the Chromatic itself, FREE GAMES (our free catalog) or MY GAMES (the paired account's own library on
// shinyrelay.com). The page runs over the install machine's receive mode with no code on the screen (cartflash
// cf_install.c catalog mode): the pages and the order are calls of the unit's own (POST /link_device/catalog, /order,
// shiny_game_data free_catalog.rs; MY GAMES: /link_device/my_games, /saves, my_games.rs, with the unit's pairing token);
// the order binds the game to this unit's code and the unit then downloads it exactly as a site send.
//   main     what it is; A: LOOK                                                                       (not captured)
//   which    FREE GAMES or MY GAMES (MY GAMES needs the unit paired: SYSTEM > ONLINE)
//   list     the games, five rows a page (title; the selected one's size and line at the bottom); Up Down, Left Right a
//            page; A: GET, B: back (receive mode ends)                                                  (captured)
//   where    CARD, ON CHROMATIC, PLAY NOW, CARTRIDGE, each greyed with its reason when it cannot take this game; the
//            first pick is CARD when a card is in and ready, else ON CHROMATIC
//   saves    (MY GAMES, a game with saves) NO SAVE or one of them
//   cart     the cartridge's rule: backed up to the card first when a card is in, else NO BACKUP: WRITE ANYWAY? (B = no);
//            the backup is the card's job (receive mode ends for it, then starts again for the order)
//   asking / getting   the order, then the download's own screens (percent, battery, result)
//   fail     two rows of why; A: TRY AGAIN, B: BACK
enum {
    kTextLen = 24,
    kX_px = 77,
    kY_px = 46,
    kRow_px = 9,
    kRows = 7,
    kPage = 5,
    kPanelW_px = 72,
    kBuf = 720,
    kBody = 170,                       // (cf_install.h CF_SIDE_BODY: 176)
    kSaves = 8,                        // (shiny_gbnet_db MY_SAVES_PER_GAME)
};
#define kPlayMax 0x400000u             // what plays with no cartridge (and so what ON CHROMATIC keeps)
#define kCartMax 0x1F0000u             // the cartridge's writable 1.94 MB
#define kCardSlack (256u * 1024u)

typedef enum { kQ_Main, kQ_Start, kQ_List, kQ_Where, kQ_Play, kQ_Order, kQ_Install, kQ_Fail,
               kQ_Which, kQ_Pair, kQ_Saves, kQ_Cart, kQ_PreBackup, kQ_Backup } View_t;
// lane m110u: the order SHARE and the site use too (ON CHROMATIC, CARD, PLAY NOW, then CARTRIDGE: it replaces what is on it)
enum { kP_Lib, kP_Card, kP_Now, kP_Cart, kP_N };
enum { kA_None, kA_Page, kA_Saves, kA_Order };
typedef struct { char id[33]; uint32_t bytes; char title[25]; char line[49]; uint8_t saves; } Ent_t;
typedef struct {
    Ent_t e[kPage];
    int n, page, pages, total;
} Page_t;
typedef struct { char id[9]; uint32_t bytes; char name[25]; } Sv_t;
typedef struct { Sv_t s[kSaves]; int n; } Saves_t;

typedef struct {
    lv_obj_t *pLabel[kRows];
    uint8_t White, Styled, Mode[kRows];
    uint8_t View, Captured, Where, Play, Loading, Ordered, Seen;   // Seen: receive mode has started since A
    uint8_t Mine;                      // the list is MY GAMES
    uint8_t Placed;                    // this receive session has sent its order (the install's screens are its own)
    uint8_t Ask;                       // kA_*: what the side call running / wanted is for
    uint8_t Resume;                    // a view to come back to when the page opens again (the backup's cart restart)
    uint8_t WhichSel;
    int Sel;                           // on the page
    int SaveSel;                       // saves: 0 = NO SAVE, else that save
    int WantPage;                      // the page asked for (-1 none)
    uint32_t JobFrom;                  // kQ_PreBackup: when the wait for receive mode's end began
    uint32_t JobAt;                    // ... when the backup was last asked (a busy unit: asked again)
    char *Buf;                         // the call's answer (heap while the page is open)
    Page_t *Pg;                        // the page shown (heap)
    Saves_t *Sv;                       // the game's saves (heap, MY GAMES)
    Ent_t Pick;                        // the game asked for
    char Why[48];                      // kQ_Fail: "ROW1|ROW2"
    uint32_t WaitFrom;                 // kQ_Install: when the wait for the game's offer began (lv ticks; 0 not waiting)
} Gg_t;

static Gg_t _Ctx = { .WantPage = -1 };
// milliseconds since boot (the OSD's LVGL tick is not kept: lv_tick_get() stays 0 on the unit)
#ifdef GAMES_HOST
static uint32_t NowMs(void) { return lv_tick_get(); }
#else
static uint32_t NowMs(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }
#endif
static OSD_Result_t OnCapture(const Button_t Button, const ButtonState_t State, void *arg);

static void LineM(lv_obj_t *pScreen, int i, bool White, const char *Text, uint8_t Mode)
{
    const uint8_t bit = (uint8_t)(1u << i);
    if (_Ctx.pLabel[i] == NULL)
    {
        _Ctx.pLabel[i] = lv_label_create(pScreen);
        lv_obj_align(_Ctx.pLabel[i], LV_ALIGN_TOP_LEFT, kX_px, kY_px + kRow_px * i);
        _Ctx.Styled &= (uint8_t)~bit;
        _Ctx.Mode[i] = 0;
        lv_obj_move_foreground(_Ctx.pLabel[i]);
    }
    if (_Ctx.Mode[i] != Mode)
    {
        if (Mode == 0) { lv_label_set_long_mode(_Ctx.pLabel[i], LV_LABEL_LONG_WRAP); lv_obj_set_size(_Ctx.pLabel[i], LV_SIZE_CONTENT, LV_SIZE_CONTENT); }
        else { lv_obj_set_size(_Ctx.pLabel[i], kPanelW_px, kRow_px); lv_label_set_long_mode(_Ctx.pLabel[i], Mode == 1 ? LV_LABEL_LONG_DOT : LV_LABEL_LONG_SCROLL_CIRCULAR); }
        _Ctx.Mode[i] = Mode;
    }
    if (!(_Ctx.Styled & bit) || !(_Ctx.White & bit) != !White)
    {
        if (_Ctx.Styled & bit) lv_obj_remove_style(_Ctx.pLabel[i], (_Ctx.White & bit) ? OSD_GetStyleTextWhite() : OSD_GetStyleTextGrey(), 0);
        lv_obj_add_style(_Ctx.pLabel[i], White ? OSD_GetStyleTextWhite() : OSD_GetStyleTextGrey(), 0);
        _Ctx.Styled |= bit;
        _Ctx.White = White ? (uint8_t)(_Ctx.White | bit) : (uint8_t)(_Ctx.White & ~bit);
    }
    char t[112];
    snprintf(t, sizeof t, "%s", Text);
    const char *cur = lv_label_get_text(_Ctx.pLabel[i]);
    if (cur == NULL || strcmp(cur, t) != 0) lv_label_set_text(_Ctx.pLabel[i], t);
}
static void Rows(lv_obj_t *pScreen, const char *const R[kRows], const bool W[kRows])
{
    for (int i = 0; i < kRows; i++) LineM(pScreen, i, W[i], R[i] ? R[i] : "", 0);
}
static void DeleteAll(void)
{
    for (int i = 0; i < kRows; i++) if (_Ctx.pLabel[i]) { lv_obj_del(_Ctx.pLabel[i]); _Ctx.pLabel[i] = NULL; }
    _Ctx.Styled = 0;
}
static void SetView(View_t v)
{
    _Ctx.View = (uint8_t)v;
    const bool Capture = v != kQ_Main;
    if (Capture != (bool)_Ctx.Captured)
    {
        MenuMgr_SetCapture(Capture ? OnCapture : NULL);
        _Ctx.Captured = Capture;
    }
}
static void FreeAll(void)
{
    free(_Ctx.Buf); _Ctx.Buf = NULL;
    free(_Ctx.Pg); _Ctx.Pg = NULL;
    free(_Ctx.Sv); _Ctx.Sv = NULL;
}
static void Spaced(char *b, size_t n, const char *t)
{
    size_t k = 0;
    for (; *t && k + 2 < n; t++) { b[k++] = *t; if (*t == ' ' && t[1] != ' ' && (k < 2 || b[k - 2] != ' ')) b[k++] = ' '; }
    b[k] = 0;
}
static void Bytes(char *b, size_t n, uint32_t bytes)
{
    if (bytes >= 1024u * 1024u && !(bytes & 0xFFFFFu)) snprintf(b, n, "%lu  MB", (unsigned long)(bytes >> 20));
    else if (bytes >= 1024u * 1024u) snprintf(b, n, "%lu.%lu  MB", (unsigned long)(bytes >> 20), (unsigned long)((bytes & 0xFFFFFu) * 10u >> 20));
    else snprintf(b, n, "%lu  KB", (unsigned long)(bytes >> 10));
}
static void Fail(const char *rows) { snprintf(_Ctx.Why, sizeof _Ctx.Why, "%s", rows && rows[0] ? rows : "SERVER  BUSY|TRY  AGAIN  LATER"); _Ctx.Ask = kA_None; NW_GgOwnJob(0); SetView(kQ_Fail); }

// ---- the pages' text: free_catalog.rs "SGC1|p|pages|n\n" then "id|bytes|TITLE|LINE\n"; my_games.rs "SGM1|p|pages|n\n" then
// "id|bytes|TITLE|N SAVES|n\n"; a game's saves "SGS1|n\n" then "sid|bytes|NAME\n"
static int Field(const char **p, char *out, size_t n, char end)
{
    size_t k = 0;
    while (**p && **p != end && **p != '\n') { if (k + 1 < n) out[k++] = **p; (*p)++; }
    out[k] = 0;
    if (**p == end) { (*p)++; return 0; }
    return **p == '\n' && end == '\n' ? ((*p)++, 0) : -1;
}
static int IdOk(const char *id)
{
    if (!id[0]) return 0;
    for (const char *q = id; *q; q++) if (!((*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9') || *q == '_' || *q == '-')) return 0;   /* (it goes into the order's body) */
    return 1;
}
int GetGames_ParsePage(const char *t, void *pg_)
{
    Page_t *pg = pg_;
    memset(pg, 0, sizeof *pg);
    char f[64];
    const char *p = t;
    const int mine = !strncmp(p, "SGM1|", 5);
    if (strncmp(p, "SGC1|", 5) && !mine) return -1;
    p += 5;
    if (Field(&p, f, sizeof f, '|')) return -1;
    pg->page = atoi(f);
    if (Field(&p, f, sizeof f, '|')) return -1;
    pg->pages = atoi(f);
    if (Field(&p, f, sizeof f, '\n')) return -1;
    pg->total = atoi(f);
    while (*p && pg->n < kPage) {
        Ent_t *e = &pg->e[pg->n];
        if (Field(&p, e->id, sizeof e->id, '|') || Field(&p, f, sizeof f, '|') || Field(&p, e->title, sizeof e->title, '|') || Field(&p, e->line, sizeof e->line, mine ? '|' : '\n')) return -1;
        e->bytes = (uint32_t)strtoul(f, NULL, 10);
        if (mine) { if (Field(&p, f, sizeof f, '\n')) return -1; const int k = atoi(f); e->saves = (uint8_t)(k < 0 ? 0 : k > kSaves ? kSaves : k); }
        if (!IdOk(e->id) || !e->title[0]) return -1;
        pg->n++;
    }
    return 0;
}
static int ParseSaves(const char *t, Saves_t *sv)
{
    memset(sv, 0, sizeof *sv);
    char f[24];
    const char *p = t;
    if (strncmp(p, "SGS1|", 5)) return -1;
    p += 5;
    if (Field(&p, f, sizeof f, '\n')) return -1;
    while (*p && sv->n < kSaves) {
        Sv_t *s = &sv->s[sv->n];
        if (Field(&p, s->id, sizeof s->id, '|') || Field(&p, f, sizeof f, '|') || Field(&p, s->name, sizeof s->name, '\n')) return -1;
        s->bytes = (uint32_t)strtoul(f, NULL, 10);
        if (!IdOk(s->id)) return -1;
        if (!s->name[0]) snprintf(s->name, sizeof s->name, "SAVE");
        sv->n++;
    }
    return 0;
}
// the answer's "why" ("ROW1|ROW2"), else the HTTP code's words
static void WhyOf(int code, const char *ans, char *out, size_t n)
{
    const char *w = ans ? strstr(ans, "\"why\":\"") : NULL;
    if (w) {
        w += 7;
        size_t k = 0;
        while (*w && *w != '"' && k + 1 < n) { const char c = *w++; out[k++] = (c >= ' ' && c <= 'Z') || c == '|' ? c : ' '; }
        out[k] = 0;
        if (k) return;
    }
    snprintf(out, n, "%s", code < 0 ? "NO  CONNECTION|TRY  AGAIN" : code == 401 ? "NOT  REGISTERED|TRY  AGAIN  LATER" : code == 429 ? "TOO  MANY  TRIES|WAIT  A  MINUTE" :
             code == 404 ? "UPDATE  YOUR|CHROMATIC  FIRST" : "SERVER  BUSY|TRY  AGAIN  LATER");
}

// ---- the places (DESIGN.md: each greyed with its reason when the unit cannot take this game)
static const char *LibWhyNot(void)
{
    const int n = NW_LibCount(NULL, NULL, NULL);
    if (n == -1) return "CANNOT  KEEP  GAMES";
    if (n >= 16) return "16  GAMES  KEPT";
    return NULL;
}
static const char *CardWhyNot(void)
{
    if (Sd_Lent()) return NULL;                                // (lent for receive mode's heap: the download mounts it again)
    if (Sd_State() == kSdSt_None) return "NO  CARD  IN";
    if (Sd_State() == kSdSt_ReadOnly) return "THE  CARD  IS  LOCKED";
    if (!Sd_Writable()) return "CARD  NOT  READY";
    return NULL;
}
static const char *PlaceWhyNot(int p, uint32_t bytes)
{
    switch (p) {
    case kP_Card: {
        const char *w = CardWhyNot();
        if (w) return w;
        sd_view_t v;
        Sd_View(&v);
        if (!Sd_Lent() && v.free_mb && (uint64_t)v.free_mb * 1048576u < (uint64_t)bytes + kCardSlack) return "THE  CARD  IS  FULL";
        return NULL;
    }
    case kP_Lib: { const char *w = LibWhyNot(); if (w) return w; return bytes > kPlayMax ? "TOO  BIG:  4  MB  AT  MOST" : NULL; }
    case kP_Now: return bytes > kPlayMax ? "TOO  BIG:  4  MB  AT  MOST" : NULL;
    case kP_Cart:
        if (!Sd_CartIn()) return "NO  CARTRIDGE  IN";
        return bytes > kCartMax ? "TOO  BIG:  1.94  MB  AT  MOST" : NULL;
    default: return "";
    }
}
static const char *const kPlaceWord[kP_N] = { "ON  CHROMATIC", "CARD", "PLAY  NOW", "CARTRIDGE" };
static const char *const kPlaceTo[kP_N] = { "lib", "sd", "now", "cart" };
// lane m110u: what the place picked means, in one row under the question (a beginner reads only this)
static const char *const kPlaceHint[kP_N] = { "STAYS  IN  CHROMATIC", "STAYS  ON  THE  CARD", "PLAYS  NOW,  NOT  KEPT", "REPLACES  CART  GAME" };
// the first pick: CARD when a card is in and ready (and it fits), else ON CHROMATIC, else the first that can take it (-1: none)
static int DefaultPlace(uint32_t bytes)
{
    if (!PlaceWhyNot(kP_Card, bytes)) return kP_Card;
    for (int p = 0; p < kP_N; p++) if (p != kP_Card && !PlaceWhyNot(p, bytes)) return p;
    return -1;
}

static void AskPage(int p)
{
    if (!_Ctx.Buf && !(_Ctx.Buf = malloc(kBuf))) { Fail("NOT  ENOUGH  MEMORY|TRY  AGAIN"); return; }
    _Ctx.WantPage = p;
    _Ctx.Ask = kA_Page;
    _Ctx.Loading = 1;
}
static void AskOrder(void)
{
    if (!_Ctx.Buf && !(_Ctx.Buf = malloc(kBuf))) { Fail("NOT  ENOUGH  MEMORY|TRY  AGAIN"); return; }
    _Ctx.Ordered = 0;
    _Ctx.Ask = kA_Order;
    _Ctx.Loading = 1;
    SetView(kQ_Order);
}
static void AskSaves(void)
{
    if (!_Ctx.Buf && !(_Ctx.Buf = malloc(kBuf))) { Fail("NOT  ENOUGH  MEMORY|TRY  AGAIN"); return; }
    if (!_Ctx.Sv && !(_Ctx.Sv = malloc(sizeof *_Ctx.Sv))) { Fail("NOT  ENOUGH  MEMORY|TRY  AGAIN"); return; }
    _Ctx.Sv->n = -1;                                           // (not here yet)
    _Ctx.SaveSel = 0;
    _Ctx.Ordered = 0;
    _Ctx.Ask = kA_Saves;
    _Ctx.Loading = 1;
    SetView(kQ_Saves);
}
// the step after WHERE: the saves (MY GAMES, a game that has some), the cartridge's rule, or the order
static void AfterWhere(void)
{
    if (_Ctx.Mine && _Ctx.Pick.saves > 0) { AskSaves(); return; }
    if (_Ctx.Sv) _Ctx.Sv->n = 0;
    _Ctx.SaveSel = 0;
    if (_Ctx.Where == kP_Cart) { SetView(kQ_Cart); return; }
    AskOrder();
}
static void AfterSaves(void)
{
    if (_Ctx.Where == kP_Cart) { SetView(kQ_Cart); return; }
    AskOrder();
}

// the calls go out from the draw (one at a time, when the machine has its code)
static void Calls(void)
{
    int code = 0;
    const int d = NW_GgSideDone(&code);
    if (d == 0) return;                                        // (one is running)
    if (d == -1 && code == -1) { _Ctx.Loading = 0; Fail("NO  CONNECTION|TRY  AGAIN"); return; }   // (receive mode went before it)
    if (d == 1)
    {
        _Ctx.Loading = 0;
        if (_Ctx.Ask == kA_Order)
        {
            _Ctx.Ask = kA_None;
            if (code == 200) { SetView(kQ_Install); return; }
            char w[48];
            WhyOf(code, _Ctx.Buf, w, sizeof w);
            Fail(w);
            return;
        }
        if (code != 200) { char w[48]; WhyOf(code, _Ctx.Buf, w, sizeof w); Fail(w); return; }
        if (_Ctx.Ask == kA_Saves)
        {
            _Ctx.Ask = kA_None;
            if (!_Ctx.Sv || ParseSaves(_Ctx.Buf, _Ctx.Sv)) { Fail("SERVER  ERROR|TRY  AGAIN  LATER"); return; }
            if (_Ctx.Sv->n == 0) AfterSaves();                // (they were deleted meanwhile: no save to pick)
            return;
        }
        _Ctx.Ask = kA_None;
        if (!_Ctx.Pg && !(_Ctx.Pg = malloc(sizeof *_Ctx.Pg))) { Fail("NOT  ENOUGH  MEMORY|TRY  AGAIN"); return; }
        if (GetGames_ParsePage(_Ctx.Buf, _Ctx.Pg)) { Fail("SERVER  ERROR|TRY  AGAIN  LATER"); return; }
        if (_Ctx.Pg->n == 0 && _Ctx.Pg->total > 0 && _Ctx.Pg->page > 0) { AskPage(0); return; }   // (the list got shorter: back to the start)
        if (_Ctx.Sel >= _Ctx.Pg->n) _Ctx.Sel = _Ctx.Pg->n ? _Ctx.Pg->n - 1 : 0;
        if (_Ctx.Sel < 0) _Ctx.Sel = 0;
        if (_Ctx.View == kQ_Start) SetView(kQ_List);
        return;
    }
    if (!NW_GgReady()) return;                                 // (no code yet: GETTING READY)
    if (_Ctx.Ask == kA_Order && _Ctx.View == kQ_Order && _Ctx.Loading && !_Ctx.Ordered)
    {
        char body[kBody];
        const Sv_t *s = _Ctx.Sv && _Ctx.SaveSel > 0 && _Ctx.SaveSel <= _Ctx.Sv->n ? &_Ctx.Sv->s[_Ctx.SaveSel - 1] : NULL;
        if (_Ctx.Mine) snprintf(body, sizeof body, "{\"code\":\"%%s\",\"id\":\"%s\",\"to\":\"%s\",\"play\":false,\"mine\":true%s%s%s}", _Ctx.Pick.id, kPlaceTo[_Ctx.Where],
                                s ? ",\"save\":\"" : "", s ? s->id : "", s ? "\"" : "");
        else snprintf(body, sizeof body, "{\"code\":\"%%s\",\"id\":\"%s\",\"to\":\"%s\",\"play\":false}", _Ctx.Pick.id, kPlaceTo[_Ctx.Where]);
        if (NW_GgSide("/link_device/catalog/order", body, _Ctx.Buf, kBuf) == 0) { _Ctx.Ordered = 1; _Ctx.Placed = 1; }
        return;
    }
    if (_Ctx.Ask == kA_Saves && _Ctx.Loading && !_Ctx.Ordered && _Ctx.Buf)
    {
        char body[64];
        snprintf(body, sizeof body, "{\"id\":\"%s\",\"code\":\"%%s\"}", _Ctx.Pick.id);
        if (NW_GgSide("/link_device/my_games/saves", body, _Ctx.Buf, kBuf) == 0) _Ctx.Ordered = 1;
        return;
    }
    if (_Ctx.Ask == kA_Page && _Ctx.WantPage >= 0 && _Ctx.Buf)
    {
        char body[48];
        snprintf(body, sizeof body, "{\"p\":%d,\"code\":\"%%s\"}", _Ctx.WantPage);
        if (NW_GgSide(_Ctx.Mine ? "/link_device/my_games" : "/link_device/catalog", body, _Ctx.Buf, kBuf) == 0) _Ctx.WantPage = -1;
    }
}

// the cartridge's backup to the card (the card's job; receive mode ended for it), then receive mode again for the order
static void BackupTick(void)
{
    if (_Ctx.View == kQ_PreBackup)
    {
        NwCartView_t v;
        NW_CartView(&v);
        if (v.Kind != kNwCv_Menu) return;                      // (receive mode is still ending)
        if (NowMs() - _Ctx.JobAt < 3000u) return;              // (a moment after the last try)
        NW_GgOwnJob(1);
        if (Sd_AskJob(kSdJob_Backup, NULL, 0, 0, "THE  CARTRIDGE") == 0) { SetView(kQ_Backup); return; }
        NW_GgOwnJob(0);
        if (NowMs() - _Ctx.JobFrom > 15000u) Fail("NOT  BACKED  UP|CHROMATIC  IS  BUSY");
        return;
    }
    sd_view_t v;
    Sd_View(&v);
    if (v.op != kSdOp_None || !v.result[0]) return;
    char r[48];
    snprintf(r, sizeof r, "%s", v.result);
    Sd_ClearResult();
    // the unit still busy (receive mode, the last install's staged copy being erased): asked again for a minute
    if (strstr(r, "CHROMATIC  IS  BUSY") && NowMs() - _Ctx.JobFrom < 60000u) { _Ctx.JobAt = NowMs(); SetView(kQ_PreBackup); return; }
    NW_GgOwnJob(0);
    if (strncmp(r, "CART  BACKED  UP", 16)) { Fail(r); return; }
    // backed up: receive mode again (its code is the order's), then the order
    _Ctx.Seen = 0; _Ctx.Placed = 0;
    NW_GgStart();
    AskOrder();
}

OSD_Result_t GetGames_Draw(void *arg)
{
    if (arg == NULL) return kOSD_Result_Err_NullDataPtr;
    lv_obj_t *const pScreen = (lv_obj_t *const)arg;
    char b1[112] = "", b2[112] = "", b3[48] = "", b4[112] = "";
    NwCartView_t v;
    NW_CartView(&v);
    if (v.Kind == -1)
    {
        const char *R[kRows] = { "GET  GAMES", "THIS  CHROMATIC", "HAS  NO  WI-FI", "", "", "", "" };
        const bool W[kRows] = { true, false, false, false, false, false, false };
        Rows(pScreen, R, W);
        return kOSD_Result_Ok;
    }
    if (_Ctx.View == kQ_Main && _Ctx.Resume) { const View_t r = (View_t)_Ctx.Resume; _Ctx.Resume = 0; SetView(r); }
    const bool off = _Ctx.View == kQ_PreBackup || _Ctx.View == kQ_Backup || _Ctx.View == kQ_Cart || _Ctx.View == kQ_Pair;   // (receive mode ends for these, or does not matter)
    // receive mode is GET GAMES's: its failures and the download's screens follow the machine. A job's screens (its
    // arrival, the download, its result) are this session's only once it has ordered: right after A on a result, the
    // finished job's result (its report still going out) is still what the machine shows (lane m110x)
    const bool jobs = v.Kind == kNwCv_Arrived || v.Kind == kNwCv_Installing || v.Kind == kNwCv_Result;
    if (v.Kind != kNwCv_Menu && (!jobs || _Ctx.Placed)) _Ctx.Seen = 1;
    if (!off && _Ctx.View != kQ_Main && _Ctx.View != kQ_Fail && _Ctx.View != kQ_Install && _Ctx.Seen)
    {
        if (v.Kind == kNwCv_Fail) { Fail(v.Note); }
        else if (v.Kind == kNwCv_Expired) { Fail("THE  CONNECTION|ENDED,  TRY  AGAIN"); }
        else if ((jobs && _Ctx.Placed) || v.Kind == kNwCv_Battery || v.Kind == kNwCv_Unfinished) SetView(kQ_Install);
        else if (v.Kind == kNwCv_Menu && _Ctx.View != kQ_Order) Fail("THE  CONNECTION|ENDED,  TRY  AGAIN");
    }
    if (_Ctx.View == kQ_PreBackup || _Ctx.View == kQ_Backup) BackupTick();
    else if (_Ctx.View != kQ_Main && _Ctx.View != kQ_Cart && _Ctx.View != kQ_Pair) Calls();

    switch ((View_t)_Ctx.View)
    {
    case kQ_Main:
    default:
    {
        // a GET GAMES install that is still going (the page left, then opened again): its screens
        if (NW_GgActive() && (v.Kind == kNwCv_Installing || v.Kind == kNwCv_Arrived || v.Kind == kNwCv_Battery || v.Kind == kNwCv_Result)) { SetView(kQ_Install); return GetGames_Draw(arg); }
        const char *R[kRows] = { "GET  GAMES", "FREE  GAMES  AND", "YOUR  OWN  GAMES,", "STRAIGHT  TO  THIS", "CHROMATIC  BY", "WI-FI", "A:  LOOK" };
        const bool W[kRows] = { true, false, false, false, false, false, true };
        Rows(pScreen, R, W);
        break;
    }
    case kQ_Which:
    {
        const bool paired = NW_Paired() != 0;
        LineM(pScreen, 0, true, "GET  GAMES", 0);
        LineM(pScreen, 1, _Ctx.WhichSel == 0, _Ctx.WhichSel == 0 ? ">FREE  GAMES" : "    FREE  GAMES", 0);
        snprintf(b1, sizeof b1, "%sMY  GAMES", _Ctx.WhichSel == 1 ? ">" : "    ");
        LineM(pScreen, 2, _Ctx.WhichSel == 1 && paired, b1, 0);
        LineM(pScreen, 3, false, "", 0);
        LineM(pScreen, 4, false, _Ctx.WhichSel == 1 ? (paired ? "YOUR  OWN  GAMES" : "PAIR  FIRST  IN") : "OUR  FREE  GAMES", 0);
        LineM(pScreen, 5, false, _Ctx.WhichSel == 1 ? (paired ? "ON  SHINYRELAY.COM" : "SYSTEM  >  ONLINE") : "", 0);
        LineM(pScreen, 6, true, "A:  OPEN   B:  BACK", 0);
        break;
    }
    case kQ_Pair:
    {
        const char *R[kRows] = { "MY  GAMES", "PAIR  IT  WITH  YOUR", "SHINY  GEN  ACCOUNT", "FIRST,  IN", "SYSTEM  >  ONLINE", "", "B:  BACK" };
        const bool W[kRows] = { true, false, false, false, true, false, true };
        Rows(pScreen, R, W);
        break;
    }
    case kQ_Start:
    {
        const char *R[kRows] = { _Ctx.Mine ? "MY  GAMES" : "GET  GAMES", v.Kind == kNwCv_Getting ? v.Title : "LOOKING...", "", "", "", "", "B:  BACK" };
        const bool W[kRows] = { true, false, false, false, false, false, true };
        Rows(pScreen, R, W);
        break;
    }
    case kQ_List:
    {
        const Page_t *pg = _Ctx.Pg;
        if (!pg) { SetView(kQ_Start); break; }
        if (pg->total == 0) {
            const char *Rf[kRows] = { "GET  GAMES", "NO  FREE  GAMES", "RIGHT  NOW", "", "", "", "B:  BACK" };
            const char *Rm[kRows] = { "MY  GAMES", "NO  GAMES  YET", "PUT  YOUR  OWN", "GAMES  IN  MY  GAMES", "ON  SHINYRELAY.COM", "", "B:  BACK" };
            const bool W[kRows] = { true, false, false, false, false, false, true };
            Rows(pScreen, _Ctx.Mine ? Rm : Rf, W);
            break;
        }
        snprintf(b1, sizeof b1, "%s  %d/%d", _Ctx.Mine ? "MY  GAMES" : "FREE  GAMES", pg->page * kPage + _Ctx.Sel + 1, pg->total);
        LineM(pScreen, 0, true, b1, 0);
        for (int r = 0; r < kPage; r++)
        {
            if (r >= pg->n) { LineM(pScreen, 1 + r, false, "", 0); continue; }
            const bool sel = r == _Ctx.Sel;
            Spaced(b3, sizeof b3, pg->e[r].title);
            snprintf(b2, sizeof b2, "%s%s", sel ? ">" : "    ", b3);
            LineM(pScreen, 1 + r, sel, b2, sel ? 2 : 1);
        }
        if (_Ctx.Loading) LineM(pScreen, 6, false, "LOADING...", 0);
        else if (_Ctx.Sel < pg->n)
        {
            char sz[16], ln[64];
            Bytes(sz, sizeof sz, pg->e[_Ctx.Sel].bytes);
            Spaced(ln, sizeof ln, pg->e[_Ctx.Sel].line);
            snprintf(b2, sizeof b2, "A:  GET  %s%s%s", sz, ln[0] ? ":  " : "", ln);   // (row 6 scrolls)
            LineM(pScreen, 6, true, b2, 2);
        }
        break;
    }
    case kQ_Where:
    {
        Spaced(b3, sizeof b3, _Ctx.Pick.title);
        LineM(pScreen, 0, true, "WHERE  DOES  IT  GO?", 0);
        // lane m110u: the place picked, said in plain words (the game's name is on the page before and the one after)
        // 1.1.3 (owner 2026-10-26): a place that cannot take it is greyed with its name only, and the cursor goes onto it
        // too: the row under the question then says why in full (it scrolls), so a reason is never cut off at the edge
        const char *whyNot = PlaceWhyNot(_Ctx.Where, _Ctx.Pick.bytes);
        if (whyNot) snprintf(b4, sizeof b4, "%s:  %s", kPlaceWord[_Ctx.Where], whyNot);
        LineM(pScreen, 1, false, whyNot ? b4 : kPlaceHint[_Ctx.Where], whyNot ? 2 : 0);
        int any = 0;
        for (int p = 0; p < kP_N; p++)
        {
            const char *no = PlaceWhyNot(p, _Ctx.Pick.bytes);
            snprintf(b1, sizeof b1, "%s%s", _Ctx.Where == p ? ">" : "    ", kPlaceWord[p]);
            if (!no) any = 1;
            LineM(pScreen, 2 + p, !no && _Ctx.Where == p, b1, 0);
        }
        LineM(pScreen, 6, true, any ? "A:  NEXT   B:  BACK" : "B:  BACK", 0);
        break;
    }
    case kQ_Saves:
    {
        Spaced(b3, sizeof b3, _Ctx.Pick.title);
        LineM(pScreen, 0, true, "WHICH  SAVE?", 0);
        if (!_Ctx.Sv || _Ctx.Sv->n < 0) {
            LineM(pScreen, 1, false, b3, 2);
            for (int r = 2; r < 6; r++) LineM(pScreen, r, false, r == 2 ? "LOOKING..." : "", 0);
            LineM(pScreen, 6, true, "B:  BACK", 0);
            break;
        }
        // NO SAVE, then the saves: five rows, the window kept round the one picked
        const int n = 1 + _Ctx.Sv->n;
        int top = _Ctx.SaveSel - 2;
        if (top > n - kPage) top = n - kPage;
        if (top < 0) top = 0;
        for (int r = 0; r < kPage; r++)
        {
            const int i = top + r;
            if (i >= n) { LineM(pScreen, 1 + r, false, "", 0); continue; }
            const bool sel = i == _Ctx.SaveSel;
            if (i == 0) snprintf(b2, sizeof b2, "%sNO  SAVE", sel ? ">" : "    ");
            else { Spaced(b3, sizeof b3, _Ctx.Sv->s[i - 1].name); snprintf(b2, sizeof b2, "%s%s", sel ? ">" : "    ", b3); }
            LineM(pScreen, 1 + r, sel, b2, sel ? 2 : 1);
        }
        LineM(pScreen, 6, true, _Ctx.Where == kP_Cart ? "A:  NEXT   B:  BACK" : "A:  GET   B:  BACK", 0);
        break;
    }
    case kQ_Cart:
    {
        Spaced(b3, sizeof b3, _Ctx.Pick.title);
        if (!CardWhyNot())
        {
            const char *R[kRows] = { "WRITE  IT  TO  THE", "CARTRIDGE?", b3, "THE  CARTRIDGE  GAME", "IS  BACKED  UP  FIRST.", "A:  WRITE", "B:  NO" };
            const bool W[kRows] = { true, true, false, false, false, true, true };
            for (int i = 0; i < kRows; i++) LineM(pScreen, i, W[i], R[i], i == 2 ? 2 : 0);
        }
        else
        {
            const char *R[kRows] = { "NO  BACKUP:", "WRITE  ANYWAY?", "NO  CARD  IS  IN,  SO", "THE  CARTRIDGE  GAME", "AND  SAVE  ARE  LOST.", "A:  WRITE  ANYWAY", "B:  NO" };
            const bool W[kRows] = { true, true, false, false, false, true, true };
            Rows(pScreen, R, W);
        }
        break;
    }
    case kQ_PreBackup:
    case kQ_Backup:
    {
        sd_view_t sv;
        Sd_View(&sv);
        if (_Ctx.View == kQ_Backup && sv.pct >= 0) snprintf(b1, sizeof b1, "%d%%", sv.pct);
        const char *R[kRows] = { "BACKING  UP", "THE  CARTRIDGE  GAME", "TO  THE  CARD  FIRST", b1, "DO  NOT  TAKE  OUT", "THE  CARTRIDGE", "" };
        const bool W[kRows] = { true, false, false, true, false, false, false };
        Rows(pScreen, R, W);
        break;
    }
    case kQ_Play:                                              // (no longer asked: the four places say it; kept for the old order of views)
        SetView(kQ_Where);
        break;
    case kQ_Order:
    {
        Spaced(b3, sizeof b3, _Ctx.Pick.title);
        const char *R[kRows] = { "GETTING  READY", b3, "", "", "", "", "" };
        const bool W[kRows] = { true, false, false, false, false, false, false };
        for (int i = 0; i < kRows; i++) LineM(pScreen, i, W[i], R[i], i == 1 ? 2 : 0);
        break;
    }
    case kQ_Install:
    {
        Spaced(b3, sizeof b3, v.Title[0] ? v.Title : _Ctx.Pick.title);
        const char *bar = strchr(v.Note, '|');
        char r1[40] = "", r2[40] = "";
        snprintf(r1, sizeof r1, "%.*s", bar ? (int)(bar - v.Note) : (int)strlen(v.Note), v.Note);
        snprintf(r2, sizeof r2, "%s", bar ? bar + 1 : "");
        switch (v.Kind)
        {
        case kNwCv_Battery:
        {
            const char *R[kRows] = { "BATTERY  LOW", b3, "PUT  IT  ON  THE", "CHARGER  TO  GO  ON", "", "A:  GO  ON  ANYWAY", "B:  STOP" };
            const bool W[kRows] = { true, false, false, false, false, true, true };
            Rows(pScreen, R, W);
            break;
        }
        case kNwCv_Result:
        {
            const char *R[kRows] = { v.Title, r1, r2, "", "", "A:  OK", "" };
            const bool W[kRows] = { true, false, false, false, false, true, false };
            Rows(pScreen, R, W);
            break;
        }
        case kNwCv_Unfinished:
        {
            const char *R[kRows] = { "NOT  FINISHED", b3, v.Note, "", "", "A:  GO  ON", "B:  STOP" };
            const bool W[kRows] = { true, false, false, false, false, true, true };
            Rows(pScreen, R, W);
            break;
        }
        case kNwCv_Fail: Fail(v.Note); break;
        case kNwCv_Menu: case kNwCv_Expired: SetView(kQ_Main); break;
        default:
        {
            // the code waits for its offer (a second or two), the header, the download
            if (v.Kind == kNwCv_Code)
            {
                const uint32_t now = NowMs();
                if (!_Ctx.WaitFrom) _Ctx.WaitFrom = now ? now : 1;
                else if (now - _Ctx.WaitFrom > 45000u) { _Ctx.WaitFrom = 0; Fail("THE  GAME  DID  NOT|COME,  TRY  AGAIN"); break; }
            }
            else _Ctx.WaitFrom = 0;
            // 1.1.2 (owner QA 2026-10-26): a game over 2 MB kept ON CHROMATIC is written into it as it comes, and the screen
            // goes dark while that runs: said before it does (~22 KB/s of the packed game, about 70 % of its size)
            char dk[kTextLen] = "";
            const bool dark = _Ctx.Where == kP_Lib && _Ctx.Pick.bytes > 0x200000u;
            if (dark) snprintf(dk, sizeof dk, "FOR  ABOUT  %u  MIN", (unsigned)(((uint64_t)_Ctx.Pick.bytes * 7u / 10u / 22528u + 59u) / 60u));
            const char *R[kRows] = { _Ctx.Where == kP_Cart ? "WRITING  IT" : "GETTING  IT", b3, v.Kind == kNwCv_Installing ? v.Note : "",
                                     dark ? "THE  SCREEN  GOES  DARK" : "", dark ? dk : "DO  NOT  TURN  OFF", dark ? "DO  NOT  TURN  OFF" : "", "" };
            const bool W[kRows] = { true, false, true, false, false, false, false };
            for (int i = 0; i < kRows; i++) LineM(pScreen, i, W[i], R[i], i == 1 ? 2 : 0);
            break;
        }
        }
        break;
    }
    case kQ_Fail:
    {
        const char *bar = strchr(_Ctx.Why, '|');
        snprintf(b1, sizeof b1, "%.*s", bar ? (int)(bar - _Ctx.Why) : (int)strlen(_Ctx.Why), _Ctx.Why);
        snprintf(b2, sizeof b2, "%s", bar ? bar + 1 : "");
        const char *R[kRows] = { b1, b2, "", "", "", "A:  TRY  AGAIN", "B:  BACK" };
        const bool W[kRows] = { true, false, false, false, false, true, true };
        Rows(pScreen, R, W);
        break;
    }
    }
    return kOSD_Result_Ok;
}

// receive mode for GET GAMES (no code on the screen), then FREE GAMES or MY GAMES
static void Start(void)
{
    if (!_Ctx.Buf && !(_Ctx.Buf = malloc(kBuf))) { Fail("NOT  ENOUGH  MEMORY|TRY  AGAIN"); return; }
    _Ctx.Sel = 0;
    _Ctx.Seen = 0;
    _Ctx.Placed = 0;
    _Ctx.Ask = kA_None;
    _Ctx.WhichSel = 0;
    NW_GgStart();
    SetView(kQ_Which);
}
static void OpenList(int mine)
{
    _Ctx.Mine = (uint8_t)(mine != 0);
    _Ctx.Sel = 0;
    free(_Ctx.Pg); _Ctx.Pg = NULL;
    AskPage(0);
    SetView(kQ_Start);
}
static void Leave(void)
{
    // receive mode was GET GAMES's and nothing is on its way: it ends (B gives the code back, the link comes back)
    NwCartView_t v;
    NW_CartView(&v);
    if (NW_GgActive() && (v.Kind == kNwCv_Getting || v.Kind == kNwCv_Code)) NW_CartButton(0);
    _Ctx.WantPage = -1; _Ctx.Loading = 0; _Ctx.Ask = kA_None;
    FreeAll();
    SetView(kQ_Main);
}

static OSD_Result_t OnCapture(const Button_t Button, const ButtonState_t State, void *arg)
{
    (void)arg;
    if (State != kButtonState_Pressed) return kOSD_Result_Ok;
    switch ((View_t)_Ctx.View)
    {
    case kQ_Which:
        if (Button == kButton_B) Leave();
        else if (Button == kButton_Up || Button == kButton_Down) _Ctx.WhichSel = (uint8_t)!_Ctx.WhichSel;
        else if (Button == kButton_A) { if (_Ctx.WhichSel == 1 && !NW_Paired()) SetView(kQ_Pair); else OpenList(_Ctx.WhichSel); }
        break;
    case kQ_Pair:
        if (Button == kButton_B || Button == kButton_A) SetView(kQ_Which);
        break;
    case kQ_Start:
        if (Button == kButton_B) Leave();
        break;
    case kQ_List:
    {
        const Page_t *pg = _Ctx.Pg;
        if (Button == kButton_B) { _Ctx.Loading = 0; _Ctx.Ask = kA_None; SetView(kQ_Which); break; }
        if (!pg || _Ctx.Loading || pg->total == 0) break;
        if (Button == kButton_Down)
        {
            if (_Ctx.Sel + 1 < pg->n) _Ctx.Sel++;
            else { _Ctx.Sel = 0; AskPage(pg->page + 1 < pg->pages ? pg->page + 1 : 0); }
        }
        else if (Button == kButton_Up)
        {
            if (_Ctx.Sel > 0) _Ctx.Sel--;
            else { _Ctx.Sel = kPage - 1; AskPage(pg->page > 0 ? pg->page - 1 : pg->pages - 1); }
        }
        else if (Button == kButton_Right && pg->pages > 1) { _Ctx.Sel = 0; AskPage(pg->page + 1 < pg->pages ? pg->page + 1 : 0); }
        else if (Button == kButton_Left && pg->pages > 1) { _Ctx.Sel = 0; AskPage(pg->page > 0 ? pg->page - 1 : pg->pages - 1); }
        else if (Button == kButton_A && _Ctx.Sel < pg->n)
        {
            _Ctx.Pick = pg->e[_Ctx.Sel];
            const int p = DefaultPlace(_Ctx.Pick.bytes);
            _Ctx.Where = (uint8_t)(p < 0 ? 0 : p);
            SetView(kQ_Where);
        }
        break;
    }
    case kQ_Where:
    {
        if (Button == kButton_B) { SetView(kQ_List); break; }
        if (Button == kButton_Up || Button == kButton_Down)
        {
            const int d = Button == kButton_Down ? 1 : kP_N - 1;
            _Ctx.Where = (uint8_t)((_Ctx.Where + d) % kP_N);   /* 1.1.3: onto a greyed place too (its reason in the row above) */
        }
        else if (Button == kButton_A && !PlaceWhyNot(_Ctx.Where, _Ctx.Pick.bytes)) AfterWhere();
        break;
    }
    case kQ_Saves:
        if (Button == kButton_B) { _Ctx.Loading = 0; _Ctx.Ask = kA_None; SetView(kQ_Where); break; }
        if (!_Ctx.Sv || _Ctx.Sv->n < 0) break;
        if (Button == kButton_Down) _Ctx.SaveSel = (_Ctx.SaveSel + 1) % (1 + _Ctx.Sv->n);
        else if (Button == kButton_Up) _Ctx.SaveSel = (_Ctx.SaveSel + _Ctx.Sv->n) % (1 + _Ctx.Sv->n);
        else if (Button == kButton_A) AfterSaves();
        break;
    case kQ_Cart:
        if (Button == kButton_B) SetView(kQ_Where);              // (B = no: nothing is written)
        else if (Button == kButton_A)
        {
            if (!CardWhyNot())
            {
                // the cartridge to the card first: receive mode ends for the card's job, then starts again for the order
                NwCartView_t v;
                NW_CartView(&v);
                if (NW_GgActive() && (v.Kind == kNwCv_Getting || v.Kind == kNwCv_Code)) NW_CartButton(0);
                _Ctx.JobFrom = NowMs(); _Ctx.JobAt = 0;
                SetView(kQ_PreBackup);
            }
            else AskOrder();                                     // (A: WRITE ANYWAY)
        }
        break;
    case kQ_Install:
    {
        NwCartView_t v;
        NW_CartView(&v);
        if (v.Kind == kNwCv_Result && (Button == kButton_A || Button == kButton_B))
        {
            // done: the list again (a new receive mode for it: that closes the result too), or B: back
            if (Button == kButton_A) { _Ctx.WantPage = -1; const int mine = _Ctx.Mine; Start(); OpenList(mine); }
            else { NW_CartButton(0); Leave(); }
        }
        else if (v.Kind == kNwCv_Battery || v.Kind == kNwCv_Unfinished) NW_CartButton(Button == kButton_A ? 1 : 0);
        else if (Button == kButton_B && (v.Kind == kNwCv_Code || v.Kind == kNwCv_Getting || v.Kind == kNwCv_Menu)) Leave();   // (nothing on its way yet)
        break;
    }
    case kQ_Fail:
        if (Button == kButton_A) { NwCartView_t v; NW_CartView(&v); if (v.Kind == kNwCv_Fail || v.Kind == kNwCv_Expired) NW_CartButton(0); const int mine = _Ctx.Mine; Start(); OpenList(mine); }
        else if (Button == kButton_B) Leave();
        break;
    case kQ_PreBackup:
    case kQ_Backup:
    case kQ_Order:
    case kQ_Play:
    case kQ_Main:
    default:
        break;
    }
    return kOSD_Result_Ok;
}

OSD_Result_t GetGames_OnButton(const Button_t Button, const ButtonState_t State, void *arg)
{
    (void)arg;
    if (State != kButtonState_Pressed) return kOSD_Result_Ok;
    if (Button == kButton_A && _Ctx.View == kQ_Main) Start();
    return kOSD_Result_Ok;
}

OSD_Result_t GetGames_OnTransition(void *arg)
{
    (void)arg;
    // an install, an order or the cartridge's backup goes on (the backup restarts the cartridge, which closes the menu):
    // the page comes back to it when it opens again; anything else leaves receive mode
    if (_Ctx.View == kQ_PreBackup || _Ctx.View == kQ_Backup || _Ctx.View == kQ_Order) { _Ctx.Resume = _Ctx.View; SetView(kQ_Main); }
    else if (_Ctx.View != kQ_Install) Leave();
    else { SetView(kQ_Main); }
    DeleteAll();
    return kOSD_Result_Ok;
}

void GetGames_Bench(char *out, size_t n)
{
    size_t k = 0;
    for (int i = 0; i < kRows && k < n; i++)
    {
        const char *t = _Ctx.pLabel[i] ? lv_label_get_text(_Ctx.pLabel[i]) : "";
        k += (size_t)snprintf(out + k, n - k, "%s%s%s", i ? "|" : "", (_Ctx.White & (1u << i)) ? "!" : "", t ? t : "");
    }
}
void GetGames_BenchButton(int b)
{
    const Button_t map[6] = { kButton_A, kButton_B, kButton_Up, kButton_Down, kButton_Left, kButton_Right };
    if (b < 0 || b > 5) return;
    if (_Ctx.Captured) (void)OnCapture(map[b], kButtonState_Pressed, NULL);
    else (void)GetGames_OnButton(map[b], kButtonState_Pressed, NULL);
}

#ifndef GAMES_HOST
// bench console: qpage [show|a|b|up|down|left|right] [MS], then the seven rows as drawn ("!" = white)
static int cmd_qpage(int argc, char **argv)
{
    static const char *const kB[6] = { "a", "b", "up", "down", "left", "right" };
    if (argc >= 2 && !strcmp(argv[1], "show")) NW_OpenPage("GET  GAMES");
    for (int i = 0; argc >= 2 && i < 6; i++) if (!strcmp(argv[1], kB[i])) GetGames_BenchButton(i);
    vTaskDelay(pdMS_TO_TICKS(argc >= 3 ? atoi(argv[2]) : 400));
    char rows[300];
    GetGames_Bench(rows, sizeof rows);
    printf("qpage: e=0 view %u rows %s\n", _Ctx.View, rows);
    return 0;
}
void GetGames_Register(void)
{
    const esp_console_cmd_t cmd = { .command = "qpage", .help = "SYSTEM > GET GAMES (bench): qpage [show|a|b|up|down|left|right] [MS]", .func = &cmd_qpage };
    (void)esp_console_cmd_register(&cmd);
}
#endif
