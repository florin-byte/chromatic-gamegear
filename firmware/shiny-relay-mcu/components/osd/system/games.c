#include "games.h"
#include "cartflash/cf_fota.h"

#include "menu_mgr.h"
#include "nw_dev.h"
#include "osd.h"
#include "../osd_default.h"
#ifndef GAMES_HOST
#include "cartflash/cf_rc_dev.h"
void PwrMgr_KeepAwakeFor(uint32_t Duration_ms);   // main/pwrmgr.c (a component cannot include main)
#else
int CfDev_FpgaSlot(void);
int CfDev_FpgaSlotPending(void);
int CfDev_FpgaSwitchesLeft(void);
int CfDev_FpgaSwitch(int slot);
int CfDev_FpgaOther(void);
int CfDev_FpgaNext(void);
#define CF_CORE_NOT_READY (-100)
uint32_t CfRc_StopDone(void);
void PwrMgr_KeepAwakeFor(uint32_t Duration_ms);
uint32_t Button_PhysicalEventCount(void);
#endif
#include "osd_shared.h"
#include "tab_shared.h"
#include "play.h"
#include "sd.h"
#include "share.h"
#include "lvgl.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// SYSTEM > GAMES (lane ui110dev, Shiny Relay 1.1.0, ../ui110/DESIGN.md): every game this Chromatic can play in ONE list,
// with where it lives: the CARTRIDGE in the slot, the games kept ON CHROMATIC (the FPGA flash's library: nw/cartflash
// cf_lib.c) and the games on the CARD (the microSD card: sdcard/). It merges the card's old GAMES page and the old PLAY
// page (play.c draws only a game loading / its result now).
//   main     the places and their counts (and a game playing); A: OPEN
//   list     the free space on top (one line, it scrolls), then the places and their games, five rows; Up Down one,
//            Left Right five; A: the game, B: back                                                    (captured)
//   game     its name, where it lives, then its ACTIONS (one table, kActs: only those that make sense and fit)
//   confirm  DELETE / WRITE TO CARTRIDGE asked once more (B keeps it)
//   busy     a job of the card's task (sd.c kSdOp_Job and the old ops), its percent; B leaves the page, the job goes on
//   result   how it ended (A: OK)
// Never on screen: FPGA, flash, library, compressed, version numbers (DESIGN.md "The words").
// The card is touched by the card's task and through SdLib_Get (never waits behind a long job); the library through
// the RAM cart task's copy (NW_LibGet). Same layout as the other SYSTEM pages (x 77, y 46, 9 px a row, seven rows).
//
// fr1: the GAMES tab (GamesTab_*) draws the same page; from its main view A (or SELECT anywhere in the menu) opens the
// library over the whole screen (kV_Lib, 12 rows of about 36 characters): the card's folders as they are on the card,
// A-Z, or the games played last (SELECT), A plays, START opens the game's actions, B goes up. The game, confirm, busy and
// result views opened from there are drawn over the whole screen too; SYSTEM > GAMES keeps the panel's list.
enum {
    kTextLen = 24,
    kX_px = 77,
    kY_px = 46,
    kRow_px = 9,
    kRows = 7,
    kListRows = 5,
    kPanelW_px = 72,
    // fr1: the whole screen (160 x 144): a title, 12 rows, the buttons
    kFullListRows = 12,
    kFullRows = kFullListRows + 2,
    kFullX_px = 8,
    kFullW_px = 144,
    kFullTitleY_px = 4,
    kFullListY_px = 18,
    kFullHintY_px = 133,
    kMaxRows = kFullRows,
};

typedef enum { kV_Main, kV_List, kV_Game, kV_Confirm, kV_Busy, kV_Result, kV_Lib } View_t;
// where a game lives (bits: an action says where it applies)
enum { kW_Cart = 1, kW_Lib = 2, kW_Card = 4 };
// the list's entries: a place's header, a game, or a place's only row when it has none (not selectable)
enum { kE_Hdr, kE_Game, kE_Note };
enum { kC_Delete, kC_Write, kC_WriteNoBackup, kC_PlayLib, kC_Switch };

typedef struct {
    uint8_t where;                     // kW_*
    char    title[SDC_NAME_LEN];
    uint32_t size, save;               // the game's bytes; its save (the library's)
    uint32_t uid;                      // ON CHROMATIC: the library's uid (0: the 1.1.0 kept game on a unit with no library)
    uint8_t flags;                     // CARD: SDC_F_*
    uint8_t cgb;
    char    path[SDC_PATH_MAX];        // CARD: the file
} Game_t;

// ---- the row actions: ONE table. A row shows the actions whose `where` has its place and whose ok() says yes, in this
// order. Lane share1 adds SHARE here (one line + its two functions); nothing else on the page needs to change.
typedef struct {
    const char *label;
    uint8_t where;
    bool (*ok)(const Game_t *g);
    void (*run)(const Game_t *g);
} Act_t;
static bool OkPlay(const Game_t *g);
static void RunPlay(const Game_t *g);
#ifndef GAMES_HOST
// Observe stop completion independently of page drawing and esp_timer callbacks.
static void StopAliveTask(void *arg)
{
    const uint32_t previous = (uint32_t)(uintptr_t)arg;
    const TickType_t started = xTaskGetTickCount();
    while (CfRc_StopDone() == previous)
    {
        if (xTaskGetTickCount() - started >= pdMS_TO_TICKS(10000))
        {
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    TickType_t last = xTaskGetTickCount();
    for (int i = 1; i <= 15; i++)
    {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(1000));
        printf("[%u ms] diag: alive %d\n", (unsigned)esp_log_timestamp(), i);
    }
    vTaskDelete(NULL);
}
#endif
static bool OkStop(const Game_t *g);
static void RunStop(const Game_t *g);
static bool OkBackup(const Game_t *g);
static void RunBackup(const Game_t *g);
static bool OkKeep(const Game_t *g);
static void RunKeep(const Game_t *g);
static bool OkToCard(const Game_t *g);
static void RunToCard(const Game_t *g);
static bool OkToCart(const Game_t *g);
static void RunToCart(const Game_t *g);
static bool OkDelete(const Game_t *g);
static void RunDelete(const Game_t *g);
static bool OkShare(const Game_t *g);
static void RunShare(const Game_t *g);
static bool OkSaveCloud(const Game_t *g);
static void RunSaveCloud(const Game_t *g);
static const Act_t kActs[] = {
    { "PLAY",                  kW_Cart | kW_Lib | kW_Card, OkPlay,   RunPlay   },
    { "STOP  PLAYING",         kW_Lib | kW_Card,           OkStop,   RunStop   },
    { "BACK  UP  TO  CARD",    kW_Cart,                    OkBackup, RunBackup },
    { "KEEP  ON  CHROMATIC",   kW_Cart | kW_Card,          OkKeep,   RunKeep   },
    { "COPY  TO  CARD",        kW_Lib,                     OkToCard, RunToCard },
    { "WRITE  TO  CARTRIDGE",  kW_Lib | kW_Card,           OkToCart, RunToCart },
    { "SHARE",                 kW_Cart | kW_Lib | kW_Card, OkShare,  RunShare  },   // lane share1: to another Chromatic nearby
    { "SAVE  TO  MY  GAMES",   kW_Lib,                     OkSaveCloud, RunSaveCloud },   // lane savecloud1: its newest save up to the account
    { "DELETE",                kW_Lib | kW_Card,           OkDelete, RunDelete },
};
enum { kActN = sizeof kActs / sizeof kActs[0], kActRows = kRows - 2 };

// fr1: the library (kV_Lib): where it is (kept while the menu is closed and between visits), and the rows on screen
enum { kLv_Folders, kLv_AZ, kLv_Recent, kLv_N };            // SELECT goes round
enum { kLoc_Top, kLoc_Lib, kLoc_Card };                      // the places, ON CHROMATIC, a folder of the card
enum { kR_None, kR_Note, kR_Cart, kR_PlaceLib, kR_PlaceCard, kR_Folder, kR_LibGame, kR_CardGame };
typedef struct {
    uint8_t view, loc;
    uint32_t folder;                   // kLoc_Card: the folder's id (0 the card's top)
    int sel, top;
} LibPos_t;
typedef struct {
    // what the rows below were read for (read again when any of it changes)
    uint8_t valid, view, loc;
    uint32_t folder, gen;
    int top, n, card, lib;
    int count;                         // entries here
    sdc_folder_t f;                    // kLoc_Card: the folder (its runs)
    uint8_t kind[kFullListRows];
    uint32_t ref[kFullListRows];       // a folder id, a record's number (LIBDIR / LIBRARY), an ON CHROMATIC game, a recent one
    uint32_t cnt[kFullListRows];       // a folder's games
    char text[kFullListRows][SDC_NAME_LEN];
    char path[96];                     // the title: where this is
    char rpath[SDC_PATH_MAX];          // kLv_Recent: the selected game's file (read when it is opened)
    uint8_t selectPlaying;            // fr3i: only on entry, never on later redraws
    uint8_t asked;                     // fr2: the card asked once (when the library opened), not every 3 s
} LibRows_t;

typedef struct {
    lv_obj_t *pLabel[kMaxRows];
    uint16_t White, Styled;
    uint8_t Mode[kMaxRows];            // a row: 0 as is, 1 cut with dots at the panel's edge, 2 scrolling
    uint8_t Full;                      // fr1: the rows are drawn over the whole screen (the library and what it opened)
    uint8_t InTab;                     // fr1: drawn by the GAMES tab (else SYSTEM > GAMES)
    uint8_t OpenReq;                   // fr1: SELECT in the menu: the library opens at the next draw
    uint8_t Back;                      // fr1: where B / OK goes from a game, a result: kV_Lib or kV_List
    lv_obj_t *pBg, *pLine1, *pLine2;   // fr1: the whole screen's background and its two lines
    LibRows_t *pLib;                   // fr1: heap while the library shows
    uint8_t View, Captured, PlayView, Held, Confirm, Act, ActTop;
    int Sel, Top;                      // the list: the selected entry, the first row shown (entry numbers)
    uint8_t SelWhere;                  // 1.1.4: the selected game itself (its place, its number there): entry numbers move when
    int SelI;                          // a place is read again (the card every 3 s, the library), the game does not
    int CoreState;                    // -2 pending, -1 unknown, 0 GB, 1 GG
    uint8_t Away;                      // 1.1.6: that game not there for now (the nearest one shown, the game still the one wanted)
    int8_t Pend;                       // 1.1.6: Up / Down pressed while its place was read (applied once it is read)
    uint8_t Known;                     // 1.1.6: the card's game count last read (KnownCard) on card generation KnownGen: shown
    int KnownCard;                     // while the card is read again, so its games stay in the list (no READING row, no jump)
    uint32_t KnownGen;
    uint32_t CacheGen;                 // the card generation the card rows were read on
    int CacheFrom, CacheCount;
    char CardRows[kListRows][SDC_NAME_LEN];
    Game_t *pGame;                     // the game opened (heap while the game / confirm / busy views need it)
    uint32_t LastAsk;
    uint8_t ReadAsked;                 // 1.1.6: this visit's read of the card's games was taken by the card's task
    char Res[64];                      // a result this page made itself ("ROW1|ROW2", or "ROW1|ROW2|ROW3")
    uint8_t BusyOwn;                   // busy: the job is the card task's (1), a library delete (2) or a stop before a delete (3)
    uint8_t StopPending;              // wait for the explicit stop-end signal
    uint32_t StopAsked, StopDone;
    uint32_t StopFrom;                 // 1.1.3: when that stop was asked
    uint32_t LibRetry;                 // m110t: when ON CHROMATIC was last asked again after a read that found no memory
} Games_t;

static bool _CorePage;
static Games_t _Ctx = { .CacheFrom = -1, .Back = kV_List };
static LibPos_t _Pos;                  // fr1: where the library is (static: kept between visits)
// milliseconds since boot (the OSD's LVGL tick is not kept: lv_tick_get() stays 0 on the unit)
#ifdef GAMES_HOST
static uint32_t NowMs(void) { return lv_tick_get(); }
#else
static uint32_t NowMs(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }
#endif
// The settle callback must run even when the hidden page is no longer drawn.
static esp_timer_handle_t _StopTimer;
static bool _StopTimerPending;
static uint32_t _StopPhysicalEvents;
static int64_t _StopArmed_us;
static void StopReopenTimer(void *arg)
{
    (void)arg;
    if (!__atomic_exchange_n(&_StopTimerPending, false, __ATOMIC_ACQ_REL)) return;
    if (Button_PhysicalEventCount() != _StopPhysicalEvents)
        printf("[%u ms] osd: reopen skipped (physical button)\n", (unsigned)esp_log_timestamp());
    // ESP timer time is advanced across light sleep; FreeRTOS ticks are not.
    else if (esp_timer_get_time() - _StopArmed_us > 1500 * 1000)
        printf("[%u ms] osd: reopen skipped (late)\n", (unsigned)esp_log_timestamp());
    else if (!OSD_IsVisible())
    {
        printf("[%u ms] osd: reopen now\n", (unsigned)esp_log_timestamp());
        OSD_ReopenCurrent();
    }
}
static void CancelStopReopen(void)
{
    __atomic_store_n(&_StopTimerPending, false, __ATOMIC_RELEASE);
    if (_StopTimer) (void)esp_timer_stop(_StopTimer);
}
static OSD_Result_t OnCapture(const Button_t Button, const ButtonState_t State, void *arg);
static void Move(int dir, int step);

// fr1: a row's place: the panel's seven rows, or over the whole screen a title (row 0), the list's rows (1 ..
// kFullListRows) and the buttons (the last row)
static void RowXY(int i, int *x, int *y)
{
    if (!_Ctx.Full) { *x = kX_px; *y = kY_px + kRow_px * i; return; }
    *x = kFullX_px;
    if (i == 0) *y = kFullTitleY_px;
    else if (i >= kFullRows - 1) *y = kFullHintY_px;
    else *y = kFullListY_px + kRow_px * (i - 1);
}
static void LineM(lv_obj_t *pScreen, int i, bool White, const char *Text, uint8_t Mode)
{
    const uint16_t bit = (uint16_t)(1u << i);
    if (_Ctx.pLabel[i] == NULL)
    {
        int x, y;
        RowXY(i, &x, &y);
        _Ctx.pLabel[i] = lv_label_create(pScreen);
        lv_obj_align(_Ctx.pLabel[i], LV_ALIGN_TOP_LEFT, x, y);
        _Ctx.Styled &= (uint16_t)~bit;
        _Ctx.Mode[i] = 0;
        lv_obj_move_foreground(_Ctx.pLabel[i]);
    }
    if (_Ctx.Mode[i] != Mode)
    {
        if (Mode == 0) { lv_label_set_long_mode(_Ctx.pLabel[i], LV_LABEL_LONG_WRAP); lv_obj_set_size(_Ctx.pLabel[i], LV_SIZE_CONTENT, LV_SIZE_CONTENT); }
        else { lv_obj_set_size(_Ctx.pLabel[i], _Ctx.Full ? kFullW_px : kPanelW_px, kRow_px); lv_label_set_long_mode(_Ctx.pLabel[i], Mode == 1 ? LV_LABEL_LONG_DOT : LV_LABEL_LONG_SCROLL_CIRCULAR); }
        _Ctx.Mode[i] = Mode;
    }
    if (!(_Ctx.Styled & bit) || !(_Ctx.White & bit) != !White)
    {
        if (_Ctx.Styled & bit) lv_obj_remove_style(_Ctx.pLabel[i], (_Ctx.White & bit) ? OSD_GetStyleTextWhite() : OSD_GetStyleTextGrey(), 0);
        lv_obj_add_style(_Ctx.pLabel[i], White ? OSD_GetStyleTextWhite() : OSD_GetStyleTextGrey(), 0);
        _Ctx.Styled |= bit;
        _Ctx.White = White ? (uint16_t)(_Ctx.White | bit) : (uint16_t)(_Ctx.White & ~bit);
    }
    char t[64];   // (a scrolling row may be long: its label cuts or scrolls it)
    snprintf(t, sizeof t, "%s", Text);
    const char *cur = lv_label_get_text(_Ctx.pLabel[i]);
    if (cur == NULL || strcmp(cur, t) != 0) lv_label_set_text(_Ctx.pLabel[i], t);
}
static void Line(lv_obj_t *pScreen, int i, bool White, const char *Text) { LineM(pScreen, i, White, Text, 0); }
static void Rows(lv_obj_t *pScreen, const char *const R[kRows], const bool W[kRows])
{
    for (int i = 0; i < kRows; i++) Line(pScreen, i, W[i], R[i] ? R[i] : "");
}

static void DeleteAll(void)
{
    for (int i = 0; i < kMaxRows; i++)
    {
        if (_Ctx.pLabel[i] != NULL) { lv_obj_del(_Ctx.pLabel[i]); _Ctx.pLabel[i] = NULL; }
    }
    _Ctx.Styled = 0;
}

// fr1: the whole screen's background (opaque: the tab's picture and the game do not show through) and its lines
#define kGamesAccent 0x30E070
static lv_obj_t *Bar(lv_obj_t *pScreen, int x, int y, int w, int h, uint32_t rgb)
{
    lv_obj_t *o = lv_obj_create(pScreen);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(rgb), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_move_foreground(o);
    return o;
}
static void SetFull(lv_obj_t *pScreen, bool Full)
{
    if ((bool)_Ctx.Full == Full && (!Full || _Ctx.pBg)) return;
    DeleteAll();                                       // (the rows are made again at their new places, over the background)
    if (_Ctx.pBg) { lv_obj_del(_Ctx.pBg); _Ctx.pBg = NULL; }
    if (_Ctx.pLine1) { lv_obj_del(_Ctx.pLine1); _Ctx.pLine1 = NULL; }
    if (_Ctx.pLine2) { lv_obj_del(_Ctx.pLine2); _Ctx.pLine2 = NULL; }
    _Ctx.Full = Full;
    if (Full && pScreen)
    {
        _Ctx.pBg = Bar(pScreen, 0, 0, kOSD_Width_px, kOSD_Height_px, 0x000000);
        _Ctx.pLine1 = Bar(pScreen, kFullX_px, kFullListY_px - 4, kFullW_px, 1, kGamesAccent);
        _Ctx.pLine2 = Bar(pScreen, kFullX_px, kFullHintY_px - 3, kFullW_px, 1, kGamesAccent);
    }
}

static void SetView(View_t v)
{
    if (v != kV_Game && v != kV_Confirm && v != kV_Busy && _Ctx.pGame) { free(_Ctx.pGame); _Ctx.pGame = NULL; }
    if (v != kV_Lib && v != kV_Game && v != kV_Confirm && v != kV_Busy && v != kV_Result && _Ctx.pLib) { free(_Ctx.pLib); _Ctx.pLib = NULL; }
    if (v == kV_Main || v == kV_List) _Ctx.Back = kV_List;   // (fr1: a view opened from the library goes back to it)
    _Ctx.View = (uint8_t)v;
    const bool Capture = v != kV_Main;
    if (Capture != (bool)_Ctx.Captured)
    {
        MenuMgr_SetCapture(Capture ? OnCapture : NULL);
        _Ctx.Captured = Capture;
    }
}

// ---- sizes as the page says them: "1.2  MB" / "640  KB" / "3.1  GB"
static void Bytes(char *b, size_t n, uint32_t bytes)
{
    if (bytes >= 1024u * 1024u) snprintf(b, n, "%lu.%lu  MB", (unsigned long)(bytes >> 20), (unsigned long)((bytes & 0xFFFFFu) * 10u >> 20));
    else snprintf(b, n, "%lu  KB", (unsigned long)(bytes >> 10));
}
// a game's size on its place's row: "512  KB" / "2  MB" / "1.5  MB"
static void GameBytes(char *b, size_t n, uint32_t bytes)
{
    if (bytes >= 1024u * 1024u && !(bytes & 0xFFFFFu)) snprintf(b, n, "%lu  MB", (unsigned long)(bytes >> 20));
    else Bytes(b, n, bytes);
}
static void Mbytes(char *b, size_t n, uint32_t mb)
{
    if (mb >= 1024) snprintf(b, n, "%lu.%lu  GB", (unsigned long)(mb / 1024), (unsigned long)(mb % 1024 * 10 / 1024));
    else snprintf(b, n, "%lu  MB", (unsigned long)mb);
}

// ---- what is where (each draw; nothing here waits on the card or the FPGA)
typedef struct {
    int cart;                          // 1 a cartridge in the slot
    int lib;                           // ON CHROMATIC games; -1 this unit keeps none (no library), -2 being read
    int kept;                          // no library: the 1.1.0 game kept in the unit (1) or none (0)
    int card;                          // CARD games; -1 no card that reads, -2 being read, -3 core pending
    uint32_t lib_free;
    sd_view_t sd;
    NwPlayView_t pv;
    char kept_title[17];
} Where_t;
static void Look(Where_t *w)
{
    memset(w, 0, sizeof *w);
    w->cart = Sd_CartIn();
    uint32_t to = 0, gen = 0;
    w->lib = NW_LibCount(&w->lib_free, &to, &gen);
    NW_PlayView(&w->pv);
    if (w->lib == -1 && (w->pv.Kind == kNwPv_Ready || (w->pv.Kind == kNwPv_Playing && w->pv.Title[0])))
    {
        w->kept = w->pv.Kind == kNwPv_Ready;
        snprintf(w->kept_title, sizeof w->kept_title, "%s", w->pv.Title);
    }
    Sd_View(&w->sd);
    if (w->sd.st == kSdSt_Ready || w->sd.st == kSdSt_ReadOnly)
    {
        if (w->sd.games >= 0) { w->card = w->sd.games; _Ctx.Known = 1; _Ctx.KnownCard = w->sd.games; _Ctx.KnownGen = Sd_Generation(); }
        else if (_Ctx.Known && _Ctx.KnownGen == Sd_Generation()) w->card = _Ctx.KnownCard;   // (1.1.6: read again: as it was)
        else w->card = -2;
    }
    else w->card = -1;
    const int core = CfDev_FpgaSlotPending() ? -2 : CfDev_FpgaSlot();
    if (_Ctx.CoreState != core) {
        _Ctx.CoreState = core;
        _Ctx.CacheFrom = -1;
        if (_Ctx.pLib) _Ctx.pLib->valid = 0;
    }
    if (core == -2) w->card = -3;
    if (CfDev_FpgaSlot() == 1) { w->cart = 0; w->lib = -1; w->kept = 0; }
}
static int LibGames(const Where_t *w) { return w->lib >= 0 ? w->lib : w->kept; }
static bool LibShown(const Where_t *w) { return w->lib != -1 || w->kept; }

// the list as entries: [CARTRIDGE, the cart] [ON CHROMATIC, its games or a note] [CARD, its games or a note]
typedef struct { uint8_t kind, where; int i; } Ent_t;
static int Count(const Where_t *w)
{
    int n = 0;
    if (w->cart) n += 2;
    if (LibShown(w)) n += 1 + (LibGames(w) > 0 ? LibGames(w) : 1);
    n += 1 + (w->card > 0 ? w->card : 1);
    return n;
}
static Ent_t EntAt(const Where_t *w, int k)
{
    Ent_t e = { kE_Note, 0, 0 };
    if (w->cart) { if (k < 2) { e.kind = k == 0 ? kE_Hdr : kE_Game; e.where = kW_Cart; return e; } k -= 2; }
    if (LibShown(w))
    {
        const int g = LibGames(w), rows = 1 + (g > 0 ? g : 1);
        if (k < rows) { e.where = kW_Lib; e.kind = k == 0 ? kE_Hdr : g > 0 ? kE_Game : kE_Note; e.i = k - 1; return e; }
        k -= rows;
    }
    e.where = kW_Card;
    e.kind = k == 0 ? kE_Hdr : w->card > 0 ? kE_Game : kE_Note;
    e.i = k - 1;
    return e;
}
static int Selectable(const Where_t *w, int k) { return k >= 0 && k < Count(w) && EntAt(w, k).kind == kE_Game; }
// 1.1.4 (a player, 2026-10-27: "menu keeps jumping back up to cartridge when trying to scroll sd card games"): the list
// was followed by entry number only. While the card is read again (every 3 s while the list shows: AskLook) or lent for
// the network's memory, and while the library is read, that place shows one note row instead of its games, so the
// selected card game "went" and the cursor moved to the nearest game left, up in CARTRIDGE, and stayed there. Now the
// selection is the game (its place and its number there): found again at each draw, and kept, not moved, while its
// place is only being read; moved to the nearest game only when the game is really gone (a card pulled, a delete).
// 1.1.6 (the same player on 1.1.5: "the menu jumping bug still exists"; unit 2, 1.1.5, seen on its screen): (1) a press
// while the card was read again went from the list as it was then, with no card games: Down went round to the top, the
// CARTRIDGE (the read takes longer than the page waits for it, 30 ms, on a card with many games); (2) a card lent for the
// network's memory (or mounted again) has no state for a moment: "gone", and the nearest game, the CARTRIDGE on a unit
// with nothing ON CHROMATIC, was remembered for good. Now a press while the game's place is read waits for the read; the
// nearest game is only shown while the game is not there (Away), and the cursor goes back to it when it is; only the
// player's own Up / Down changes the game wanted. A card lent is a card being read.
static void Remember(const Where_t *w)
{
    _Ctx.Away = 0;
    if (!Selectable(w, _Ctx.Sel)) { _Ctx.SelWhere = 0; return; }
    const Ent_t e = EntAt(w, _Ctx.Sel);
    _Ctx.SelWhere = e.where; _Ctx.SelI = e.i;
}
static int FindSel(const Where_t *w)
{
    if (!_Ctx.SelWhere) return -1;
    const int n = Count(w);
    for (int k = 0; k < n; k++) { const Ent_t e = EntAt(w, k); if (e.kind == kE_Game && e.where == _Ctx.SelWhere && e.i == _Ctx.SelI) return k; }
    return -1;
}
static bool PlaceReading(const Where_t *w, uint8_t where)
{
    // (mounted again with no state yet: reading too, unless the game had gone already: a card out is asked every 3 s)
    if (where == kW_Card) return w->card == -2 || Sd_Lent() || ((w->sd.op == kSdOp_Mount || w->sd.op == kSdOp_Refresh || w->sd.op == kSdOp_Look) && (w->card >= 0 || !_Ctx.Away));
    if (where == kW_Lib) return w->lib == -2 || w->lib == -3;
    return false;
}

// the card's rows of the window, read again when the window or the card changed
static void FillCard(const Where_t *w, int from)
{
    if (_Ctx.CacheFrom == from && _Ctx.CacheGen == Sd_Generation() && _Ctx.CacheCount == w->card) return;
    char rows[kListRows][SDC_NAME_LEN];             // (1.1.6: all the rows or none: a busy card mid-way kept half a window)
    for (int i = 0; i < kListRows; i++)
    {
        const int at = from + i;
        if (at >= w->card) { rows[i][0] = 0; continue; }
        sdc_rec_t r;
        const int e = SdLib_Get((uint32_t)at, &r);
        if (e == -2) return;                       // busy: the next draw tries again (the rows read before still shown)
        if (e) { snprintf(rows[i], sizeof rows[i], "?"); continue; }
        snprintf(rows[i], sizeof rows[i], "%s", r.title);
    }
    memcpy(_Ctx.CardRows, rows, sizeof rows);
    _Ctx.CacheFrom = from; _Ctx.CacheGen = Sd_Generation(); _Ctx.CacheCount = w->card;
}
// a game's name as the page draws it: each space doubled (the OSD font's space is one pixel: "SHINY  KART")
static void Spaced(char *b, size_t n, const char *t)
{
    size_t k = 0;
    for (; *t && k + 2 < n; t++) { b[k++] = *t; if (*t == ' ' && t[1] != ' ' && (k < 2 || b[k - 2] != ' ')) b[k++] = ' '; }
    b[k] = 0;
}
static void CartTitle(char *b, size_t n)
{
    NW_CartTitle(b, n);
    if (!b[0]) snprintf(b, n, "CARTRIDGE  GAME");   // (its title is known once a cart job has read it)
}
// an entry's words (a game: its name; a header: the place; a note: why the place has none)
static void EntText(const Where_t *w, const Ent_t *e, char *b, size_t n)
{
    b[0] = 0;
    if (e->kind == kE_Hdr) { snprintf(b, n, "%s", e->where == kW_Cart ? "CARTRIDGE:" : e->where == kW_Lib ? "ON  CHROMATIC:" : "CARD:"); return; }
    if (e->kind == kE_Note)
    {
        if (e->where == kW_Lib) snprintf(b, n, w->lib == -2 ? "    READING..." : w->lib == -3 ? "    STILL  LOOKING..." : "    NO  GAMES  YET");
        else if (w->card == -3) snprintf(b, n, "    READING  THE  CORE");
        else if (w->card == -2) snprintf(b, n, w->sd.op == kSdOp_Mount && w->sd.pct >= 0 ? "    READING  %d%%" : "    READING...", w->sd.pct);
        else if (w->card == 0) snprintf(b, n, "    NO  GAMES  YET");
        else snprintf(b, n, "    %s", w->sd.st == kSdSt_None ? "NO  CARD  IN" : Sd_StateWord());
        return;
    }
    if (e->where == kW_Cart) { char t[40]; CartTitle(t, sizeof t); Spaced(b, n, t); return; }
    if (e->where == kW_Lib)
    {
        NwLibGame_t g;
        if (w->lib >= 0) { if (NW_LibGet(e->i, &g) == 0) Spaced(b, n, g.Title); else snprintf(b, n, "?"); }
        else Spaced(b, n, w->kept_title);
        return;
    }
    const int r = e->i - _Ctx.CacheFrom;
    Spaced(b, n, (r >= 0 && r < kListRows) ? _Ctx.CardRows[r] : "");
}

// the game opened: a copy (the list may change under it)
static int OpenEnt(const Where_t *w, int k)
{
    const Ent_t e = EntAt(w, k);
    if (e.kind != kE_Game) return -1;
    Game_t *g = _Ctx.pGame ? _Ctx.pGame : malloc(sizeof *g);
    if (!g) return -1;
    memset(g, 0, sizeof *g);
    g->where = e.where;
    if (e.where == kW_Cart) { char t[40]; CartTitle(t, sizeof t); snprintf(g->title, sizeof g->title, "%.23s", t); }
    else if (e.where == kW_Lib)
    {
        NwLibGame_t lg;
        if (w->lib >= 0)
        {
            if (NW_LibGet(e.i, &lg)) { if (!_Ctx.pGame) free(g); return -1; }
            snprintf(g->title, sizeof g->title, "%s", lg.Title);
            g->uid = lg.Uid; g->size = lg.RomSize; g->save = lg.SaveSize;
        }
        else snprintf(g->title, sizeof g->title, "%s", w->kept_title);
    }
    else
    {
        sdc_rec_t r;
        if (SdLib_Get((uint32_t)e.i, &r)) { if (!_Ctx.pGame) free(g); return -1; }
        snprintf(g->title, sizeof g->title, "%s", r.title);
        g->size = r.size; g->flags = r.flags; g->cgb = r.cgb;
        snprintf(g->path, sizeof g->path, "%s", r.path);
    }
    _Ctx.pGame = g;
    _Ctx.Act = 0;
    _Ctx.ActTop = 0;
    NW_SaveCloudForget();                                         // lane savecloud1: whether this game has a save is asked afresh
    return 0;
}

// ---- the actions
static bool Playing(void) { NwPlayView_t v; NW_PlayView(&v); return v.Kind == kNwPv_Playing || v.Kind == kNwPv_Loading; }
static bool IsPlaying(const Game_t *g)
{
    if (!Playing()) return false;
    if (g->where == kW_Lib) return g->uid ? (uint32_t)NW_LibPlayingUid() == g->uid : true;
    if (g->where == kW_Card) return NW_SdPlayingPath(g->path);
    return false;
}
static bool JobIdle(void) { return Sd_Busy() == kSdOp_None && !NW_LibBusy(); }
static bool OkPlay(const Game_t *g)
{
    if (g->where == kW_Cart) return true;                         // (a game playing with no cartridge: back to the cartridge)
    if (IsPlaying(g)) return false;
    if (g->where == kW_Card) return (g->flags & SDC_F_PLAYABLE) != 0;
    return true;
}
static void Result(const char *r) { snprintf(_Ctx.Res, sizeof _Ctx.Res, "%s", r); SetView(kV_Result); }
static void PlayNow(const Game_t *g);
static int _SwitchSlot;
static volatile int _SwitchDone, _SwitchResult, _SwitchBusy;
static void SwitchTask(void *arg)
{
    (void)arg;
    _SwitchResult = CfDev_FpgaSwitch(_SwitchSlot);
    _SwitchDone = 1;
#ifndef GAMES_HOST
    vTaskDelete(NULL);
#endif
}
static int GameSlot(const Game_t *g)
{
    if (g->where == kW_Card) {
        FIL f;
        uint32_t size, got = 0;
        char marker[8];
        if (!Sd_ReadOpen(&f, g->path, &size)) {
            const int e = Sd_Read(&f, 0x150, marker, sizeof marker, &got);
            Sd_Close(&f);
            if (!e && got == sizeof marker && !memcmp(marker, "GGCHROM1", sizeof marker)) return 1;
        }
    }
    return 0;
}
static void RunPlay(const Game_t *g)
{
    if (_SwitchBusy) return;
    if (g->where != kW_Cart) {
        const int running = CfDev_FpgaSlot();
        const int needed = running >= 0 ? GameSlot(g) : running;
        if (running >= 0 && needed != running) {
            _SwitchSlot = needed;
            _Ctx.Confirm = kC_Switch;
            SetView(kV_Confirm);
            return;
        }
    }
    // 1.1.2 (owner 2026-10-26): an ON CHROMATIC game's load blanks the screen for a while: said first, A plays
    if (g->where == kW_Lib) { _Ctx.Confirm = kC_PlayLib; SetView(kV_Confirm); return; }
    PlayNow(g);
}
static void PlayNow(const Game_t *g)
{
    if (g->where == kW_Cart)
    {
        if (Playing()) { NW_PlayButton(1); Result("BACK  TO  THE|CARTRIDGE"); }
        else Result("THE  CARTRIDGE|IS  PLAYING  NOW");
        return;
    }
    // a game playing is stopped first (its save kept), then this one starts (NW_PlayAfterStop waits for the stop)
    if (g->where == kW_Lib && g->uid) NW_PlayAfterStop(0, g->uid, NULL);
    else if (g->where == kW_Lib) NW_PlayButton(1);                // (the 1.1.0 kept game: A on its page)
    else { (void)SdLib_RecentAdd(g->path, g->title); NW_PlayAfterStop(1, 0, g->path); }   // (fr1: first in PLAYED LAST)
    SetView(kV_Main);
}
static bool OkStop(const Game_t *g) { return IsPlaying(g); }
static void RunStop(const Game_t *g)
{
    (void)g;
    CancelStopReopen();
    _Ctx.StopPending = 1;
    _Ctx.StopAsked = NowMs();
    _Ctx.StopDone = CfRc_StopDone();
#ifndef GAMES_HOST
    if (xTaskCreate(StopAliveTask, "stop_alive", 3072, (void *)(uintptr_t)_Ctx.StopDone, 1, NULL) != pdPASS)
        printf("[%u ms] diag: alive task failed\n", (unsigned)esp_log_timestamp());
#endif
    NW_PlayButton(1);
    Result("STOPPED|ITS  SAVE  IS  KEPT");
}
static bool CardTakes(void) { return Sd_Writable() != 0; }
static bool OkBackup(const Game_t *g) { (void)g; return CardTakes(); }
static int Job(int kind, const Game_t *g, int flags)
{
    if (Sd_AskJob(kind, g->path[0] ? g->path : NULL, g->uid, flags, g->title) != 0) { Result("NOT  NOW|SOMETHING  ELSE  IS|RUNNING:  WAIT"); return -1; }
    _Ctx.BusyOwn = 1;
    SetView(kV_Busy);
    return 0;
}
static void RunBackup(const Game_t *g) { (void)Job(kSdJob_Backup, g, 0); }
static bool OkKeep(const Game_t *g)
{
    const int n = NW_LibCount(NULL, NULL, NULL);
    if (n < 0 || n >= 16) return false;                           // (no library on this unit; 16 games is its most)
    if (g->where == kW_Card) return (g->flags & SDC_F_PLAYABLE) && g->size <= 0x400000u;
    return true;
}
static void RunKeep(const Game_t *g) { (void)Job(g->where == kW_Cart ? kSdJob_CartToLib : kSdJob_CardToLib, g, 0); }
static bool OkToCard(const Game_t *g) { return g->uid != 0 && CardTakes(); }
static void RunToCard(const Game_t *g) { (void)Job(kSdJob_LibToCard, g, 0); }
static bool OkToCart(const Game_t *g)
{
    if (!Sd_CartIn() || g->size > 0x1F0000u || g->size < 0x8000u) return false;   // (the cartridge's writable 1.94 MB)
    return g->where == kW_Card || g->uid != 0;
}
static void RunToCart(const Game_t *g) { (void)g; _Ctx.Confirm = CardTakes() ? kC_Write : kC_WriteNoBackup; SetView(kV_Confirm); }
static bool OkDelete(const Game_t *g)
{
    // 1.1.3 (owner QA 2026-10-26: DELETE was not there while the game played, and nothing said why): offered always; a
    // game playing is stopped first (its confirm says so)
    return g->where == kW_Lib ? g->uid != 0 : CardTakes();
}
static void RunDelete(const Game_t *g) { (void)g; _Ctx.Confirm = kC_Delete; SetView(kV_Confirm); }
static void DoDelete(const Game_t *g)
{
    if (g->where == kW_Lib) { if (NW_LibDelete(g->uid) == 0) { _Ctx.BusyOwn = 2; (void)Sd_SetJobTitle(g->title); SetView(kV_Busy); } else Result("NOT  NOW|SOMETHING  ELSE  IS|RUNNING:  WAIT"); }
    else if (Sd_Ask(kSdOp_Delete, g->path) == 0) { _Ctx.BusyOwn = 1; (void)Sd_SetJobTitle(g->title); SetView(kV_Busy); }
    else Result("NOT  NOW|SOMETHING  ELSE  IS|RUNNING:  WAIT");
}
// lane share1: SHARE (cf_share_dev.c owns the session; share.c draws its pages over this one)
static bool OkShare(const Game_t *g)
{
    if (!Share_Available()) return false;
    if (g->where == kW_Cart) return Sd_CartIn();
    if (g->where == kW_Lib) return g->uid != 0;
    return (g->flags & SDC_F_PLAYABLE) != 0;
}
// lane savecloud1 (owner 2026-11-08: manual only, only a game kept ON CHROMATIC): the game's newest save up to the paired
// account's My games. Offered when no game is playing and the game has save memory. Not paired or no Wi-Fi saved: the row is
// there and pressing it says what is missing (a player on Wi-Fi who had never paired saw no row and no reason, 2026-11-09).
// Ready: only when a save is stored. The job (kSdJob_SaveToCloud) stops the link, sends, and brings the link back; its words
// come from cf_sup.c.
static bool OkSaveCloud(const Game_t *g)
{
    if (g->where != kW_Lib || !g->uid || !g->save) return false;
    if (Playing()) return false;
    if (!NW_SaveCloudReady()) return true;
    return NW_SaveCloudHas(g->uid) == 1;
}
static void RunSaveCloud(const Game_t *g) { (void)Job(kSdJob_SaveToCloud, g, 0); }
static void RunShare(const Game_t *g)
{
    // (the title goes to the other unit as is; the cart's own is read from its header at SEND)
    int e;
    if (g->where == kW_Cart) e = Share_SendStart(kShareSrc_Cart, 0, NULL, "THE  CARTRIDGE", OnCapture);
    else if (g->where == kW_Lib) e = Share_SendStart(kShareSrc_OnChromatic, g->uid, NULL, g->title, OnCapture);
    else e = Share_SendStart(kShareSrc_Card, 0, g->path, g->title, OnCapture);
    // m110s: a share that could not start says so (m110f, unit 1: A on SHARE from the CARD did nothing at all)
    if (e == kShare_NoMemory) Result("NOT  SENT|NOT  ENOUGH  MEMORY|TURN  IT  OFF  AND  ON");
    else if (e != kShare_Ok) Result("NOT  NOW|SOMETHING  ELSE  IS|RUNNING:  WAIT");
}

// the actions a game shows now, in table order: their table numbers into o (up to kActN; the page shows kActRows of them at a
// time and follows the selection: savecloud1 made six possible, with a card and a cartridge in)
static int Acts(const Game_t *g, uint8_t o[kActN])
{
    int n = 0;
    for (int a = 0; a < kActN && n < kActN; a++)
        if ((kActs[a].where & g->where) && kActs[a].ok(g)) o[n++] = (uint8_t)a;
    return n;
}

static void AskLook(void)
{
    const uint32_t now = (uint32_t)(NowMs());
    if (_Ctx.LastAsk && now - _Ctx.LastAsk < 3000) return;
    // 1.1.6 (owner: "why does it need to read the sd card so often"): the page's first ask reads the card's games again;
    // then every 3 s only whether the card is still there (one card command), its games read again only when a card went
    // in (the unit's own adds and deletes read them already)
    const int op = _Ctx.ReadAsked ? kSdOp_Look : kSdOp_Mount;
    _Ctx.LastAsk = now ? now : 1;
    if (Sd_Busy() == kSdOp_None && Sd_Ask(op, NULL) == 0 && op == kSdOp_Mount) _Ctx.ReadAsked = 1;   // (a mounted card: asked whether it is still there, the list refreshed)
}

static void FreeLine(const Where_t *w, char *b, size_t n)
{
    char l[16] = "", c[16] = "";
    if (w->lib >= 0) Bytes(l, sizeof l, w->lib_free);
    if (w->card >= 0 || w->card == -2) Mbytes(c, sizeof c, w->sd.free_mb);
    if (l[0] && c[0]) snprintf(b, n, "FREE:  ON  CHROMATIC  %s,  CARD  %s", l, c);   // (row 0 scrolls)
    else if (l[0]) snprintf(b, n, "FREE:  ON  CHROMATIC  %s,  NO  CARD", l);
    else if (c[0]) snprintf(b, n, "FREE  ON  THE  CARD:  %s", c);
    else snprintf(b, n, "NO  CARD  IN");
}

static const char *JobWord(int job)
{
    switch (job) {
    case kSdJob_Backup:     return "BACKING  UP";
    case kSdJob_CardToCart:
    case kSdJob_LibToCart:  return "WRITING  THE  CART";
    case kSdJob_CardToLib:
    case kSdJob_CartToLib:  return "KEEPING  IT";
    case kSdJob_LibToCard:  return "COPYING  TO  CARD";
    case kSdJob_SaveToCloud: return "SAVING  ONLINE";
    default:                return "WORKING";
    }
}

// ---- fr1: the library (kV_Lib)
static void Upper(char *b, size_t n, const char *t)
{
    char u[SDC_NAME_LEN + 8];
    size_t k = 0;
    for (; *t && k + 1 < sizeof u; t++) u[k++] = (char)((*t >= 'a' && *t <= 'z') ? *t - 32 : ((unsigned char)*t < 0x20 || (unsigned char)*t > 0x7E) ? '?' : *t);
    u[k] = 0;
    Spaced(b, n, u);
}
static bool CardFolders(void) { return SdLib_Folders() > 0; }
// the entries where the library is: their count (-2: the card is busy, the next draw asks again)
static int LibCount(const Where_t *w, LibRows_t *L)
{
    if (w->card == -3 && (_Pos.view != kLv_Folders || _Pos.loc == kLoc_Card)) return 1;
    switch (_Pos.view)
    {
    case kLv_AZ: return w->card > 0 ? w->card : 1;                 // (none: one note row)
    case kLv_Recent:
    {
        int n = 0;
        char p[SDC_PATH_MAX], t[SDC_NAME_LEN];
        for (uint32_t k = 0; k < SDC_RECENT_MAX; k++) { const int e = SdLib_Recent(k, p, sizeof p, t, sizeof t); if (e == -2) return -2; if (e) break; n++; }
        return n ? n : 1;
    }
    default: break;
    }
    if (_Pos.loc == kLoc_Top) return (w->cart ? 1 : 0) + (LibShown(w) ? 1 : 0) + 1;
    if (_Pos.loc == kLoc_Lib) return LibGames(w) > 0 ? LibGames(w) : 1;
    if (w->card <= 0) return 1;
    if (!CardFolders()) return w->card;                             // (no folder files yet: the card's games A-Z)
    const int e = SdLib_Folder(_Pos.folder, &L->f);
    if (e == -2) return -2;
    if (e) { _Pos.folder = 0; if (SdLib_Folder(0, &L->f)) return -2; }   // (the folder went: the card's top)
    const int n = (int)(L->f.child_count + L->f.game_count);
    return n ? n : 1;
}
// the title: where the library is (CARD / RPG / POKEMON); the folders' names read up to the top
static void LibPath(const Where_t *w, LibRows_t *L)
{
    char b[96] = "";
    if (_Pos.view == kLv_AZ) snprintf(b, sizeof b, "CARD,  A  TO  Z  (%d)", w->card > 0 ? w->card : 0);   // (fr1: whole screen)
    else if (_Pos.view == kLv_Recent) snprintf(b, sizeof b, "PLAYED  LAST");   // (fr1: whole screen)
    else if (_Pos.loc == kLoc_Top) snprintf(b, sizeof b, "GAMES");   // (fr1: whole screen)
    else if (_Pos.loc == kLoc_Lib) snprintf(b, sizeof b, "ON  CHROMATIC  (%d)", LibGames(w) > 0 ? LibGames(w) : 0);   // (fr1: whole screen)
    else
    {
        char names[SDC_DEPTH_MAX + 1][SDC_FNAME_LEN];
        int d = 0;
        uint32_t id = _Pos.folder;
        sdc_folder_t f;
        while (id && d < SDC_DEPTH_MAX && CardFolders() && SdLib_Folder(id, &f) == 0) { snprintf(names[d++], sizeof names[0], "%s", f.name); id = f.parent; }
        size_t k = (size_t)snprintf(b, sizeof b, "CARD");   // (fr1: whole screen)
        while (d-- > 0 && k < sizeof b) { char u[96]; Upper(u, sizeof u, names[d]); k += (size_t)snprintf(b + k, sizeof b - k, "  /  %s", u); }
    }
    snprintf(L->path, sizeof L->path, "%s", b);
}
// the rows on screen read again when where the library is, its first row or the card changed. 0 = read
static int LibFill(const Where_t *w, LibRows_t *L, int n)
{
    const uint32_t gen = Sd_Generation();
    if (L->valid && L->view == _Pos.view && L->loc == _Pos.loc && L->folder == _Pos.folder && L->top == _Pos.top && L->gen == gen &&
        L->n == n && L->card == w->card && L->lib == w->lib) return 0;
    for (int r = 0; r < kFullListRows; r++) { L->kind[r] = kR_None; L->text[r][0] = 0; L->cnt[r] = 0; L->ref[r] = 0; }
    for (int r = 0; r < kFullListRows && _Pos.top + r < n; r++)
    {
        const int k = _Pos.top + r;
        char t[SDC_NAME_LEN];
        if (w->card == -3 && (_Pos.view != kLv_Folders || _Pos.loc == kLoc_Card)) {
            L->kind[r] = kR_Note;
            snprintf(L->text[r], sizeof L->text[r], "READING  THE  CORE");
            continue;
        }
        if (_Pos.view == kLv_AZ || (_Pos.view == kLv_Folders && _Pos.loc == kLoc_Card && !CardFolders()))
        {
            if (w->card <= 0) { L->kind[r] = kR_Note; snprintf(L->text[r], sizeof L->text[r], "%s", w->card == -2 ? "READING..." : w->sd.st == kSdSt_None ? "NO  CARD  IN" : "NO  GAMES  ON  THE  CARD"); continue; }   // (fr1: whole screen)
            sdc_rec_t rec;
            const int e = SdLib_Get((uint32_t)k, &rec);
            if (e == -2) return -2;
            L->kind[r] = kR_CardGame; L->ref[r] = (uint32_t)k;
            snprintf(L->text[r], sizeof L->text[r], "%s", e ? "?" : rec.title);
            continue;
        }
        if (_Pos.view == kLv_Recent)
        {
            char p[SDC_PATH_MAX];
            const int e = SdLib_Recent((uint32_t)k, p, sizeof p, t, sizeof t);
            if (e == -2) return -2;
            if (e) { L->kind[r] = kR_Note; snprintf(L->text[r], sizeof L->text[r], "NOTHING  PLAYED  YET"); continue; }   // (fr1: whole screen)
            L->kind[r] = kR_CardGame; L->ref[r] = (uint32_t)k;
            snprintf(L->text[r], sizeof L->text[r], "%s", t);
            continue;
        }
        if (_Pos.loc == kLoc_Top)
        {
            int slot = k;
            if (w->cart && slot-- == 0) { L->kind[r] = kR_Cart; CartTitle(t, sizeof t); snprintf(L->text[r], sizeof L->text[r], "CARTRIDGE:  %s", t); continue; }   // (fr1: whole screen)
            if (LibShown(w) && slot-- == 0) { L->kind[r] = kR_PlaceLib; L->cnt[r] = (uint32_t)(LibGames(w) > 0 ? LibGames(w) : 0); snprintf(L->text[r], sizeof L->text[r], "ON  CHROMATIC"); continue; }   // (fr1: whole screen)
            L->kind[r] = w->card >= 0 ? kR_PlaceCard : kR_Note; L->cnt[r] = (uint32_t)(w->card > 0 ? w->card : 0);
            snprintf(L->text[r], sizeof L->text[r], "%s", w->card >= 0 ? "CARD" : w->card == -3 ? "READING  THE  CORE" : w->card == -2 ? "CARD:  READING..." : w->sd.st == kSdSt_None ? "CARD:  NONE" : "CARD:  CANNOT  READ");   // (fr1: whole screen)
            continue;
        }
        if (_Pos.loc == kLoc_Lib)
        {
            if (LibGames(w) <= 0) { L->kind[r] = kR_Note; snprintf(L->text[r], sizeof L->text[r], "%s", w->lib == -2 || w->lib == -3 ? "READING..." : "NO  GAMES  YET"); continue; }   // (fr1: whole screen)
            NwLibGame_t g;
            L->kind[r] = kR_LibGame; L->ref[r] = (uint32_t)k;
            if (w->lib >= 0) snprintf(L->text[r], sizeof L->text[r], "%s", NW_LibGet(k, &g) == 0 ? g.Title : "?");   // (fr1: whole screen)
            else snprintf(L->text[r], sizeof L->text[r], "%s", w->kept_title);
            continue;
        }
        // a folder of the card: its folders, then its games
        if (w->card <= 0 || L->f.child_count + L->f.game_count == 0) { L->kind[r] = kR_Note; snprintf(L->text[r], sizeof L->text[r], "%s", w->card == -2 ? "READING..." : "NO  GAMES  HERE"); continue; }   // (fr1: whole screen)
        if ((uint32_t)k < L->f.child_count)
        {
            uint32_t id = 0;
            sdc_folder_t cf;
            int e = SdLib_FolderChild(L->f.child_first + (uint32_t)k, &id);
            if (!e) e = SdLib_Folder(id, &cf);
            if (e == -2) return -2;
            L->kind[r] = kR_Folder; L->ref[r] = id; L->cnt[r] = e ? 0 : cf.total;
            snprintf(L->text[r], sizeof L->text[r], "%s", e ? "?" : cf.name);
            continue;
        }
        sdc_rec_t rec;
        const uint32_t pos = L->f.game_first + (uint32_t)k - L->f.child_count;
        const int e = SdLib_DirGet(pos, &rec);
        if (e == -2) return -2;
        L->kind[r] = kR_CardGame; L->ref[r] = pos;
        snprintf(L->text[r], sizeof L->text[r], "%s", e ? "?" : rec.title);
    }
    LibPath(w, L);
    L->valid = 1; L->view = _Pos.view; L->loc = _Pos.loc; L->folder = _Pos.folder; L->top = _Pos.top; L->gen = gen;
    L->n = n; L->card = w->card; L->lib = w->lib;
    return 0;
}
// the library opened (the GAMES tab's A, or SELECT in the menu): where it was left, or the places
static void LibOpen(void)
{
    if (!_Ctx.pLib) _Ctx.pLib = calloc(1, sizeof *_Ctx.pLib);
    if (!_Ctx.pLib) { Result("NOT  NOW|NOT  ENOUGH  MEMORY"); return; }   // (fr1: whole screen)
    _Ctx.pLib->valid = 0;
    _Ctx.pLib->selectPlaying = 1;
    _Ctx.Back = kV_Lib;
    SetView(kV_Lib);
}
static bool RowIsGame(uint8_t k) { return k == kR_Cart || k == kR_LibGame || k == kR_CardGame; }
// the selected row's game as the game page keeps it (a copy: the list may change under it). 0 = opened
static int LibOpenGame(const Where_t *w, LibRows_t *L, int r)
{
    Game_t *g = _Ctx.pGame ? _Ctx.pGame : malloc(sizeof *g);
    if (!g) return -1;
    memset(g, 0, sizeof *g);
    const uint8_t kind = L->kind[r];
    int e = 0;
    if (kind == kR_Cart) { char t[40]; g->where = kW_Cart; CartTitle(t, sizeof t); snprintf(g->title, sizeof g->title, "%s", t); }
    else if (kind == kR_LibGame)
    {
        g->where = kW_Lib;
        NwLibGame_t lg;
        if (w->lib >= 0) { e = NW_LibGet((int)L->ref[r], &lg); if (!e) { snprintf(g->title, sizeof g->title, "%s", lg.Title); g->uid = lg.Uid; g->size = lg.RomSize; g->save = lg.SaveSize; } }
        else snprintf(g->title, sizeof g->title, "%s", w->kept_title);
    }
    else if (kind == kR_CardGame)
    {
        sdc_rec_t rec;
        g->where = kW_Card;
        if (_Pos.view == kLv_Recent)
        {
            char t[SDC_NAME_LEN];
            e = SdLib_Recent(L->ref[r], L->rpath, sizeof L->rpath, t, sizeof t);
            const int i = e ? -1 : SdLib_Find(L->rpath);          // (the card read once, on the press)
            e = i < 0 ? -3 : SdLib_Get((uint32_t)i, &rec);
        }
        else if (_Pos.view == kLv_AZ || !CardFolders()) e = SdLib_Get(L->ref[r], &rec);
        else e = SdLib_DirGet(L->ref[r], &rec);
        if (!e) { snprintf(g->title, sizeof g->title, "%s", rec.title); g->size = rec.size; g->flags = rec.flags; g->cgb = rec.cgb; snprintf(g->path, sizeof g->path, "%s", rec.path); }
    }
    else e = -1;
    if (e) { if (!_Ctx.pGame) free(g); if (e == -3) Result("NOT  FOUND|IT  IS  NOT  ON  THE|CARD  ANY  MORE"); return -1; }   // (fr1: whole screen)
    _Ctx.pGame = g;
    _Ctx.Act = 0;
    return 0;
}
static void LibGo(uint8_t loc, uint32_t folder)
{
    _Pos.loc = loc; _Pos.folder = folder; _Pos.sel = 0; _Pos.top = 0;
    if (_Ctx.pLib) { _Ctx.pLib->valid = 0; _Ctx.pLib->selectPlaying = 1; }
}
// B: one folder up (the folder left selected), the places, then out of the library
static void LibUp(void)
{
    if (_Pos.view != kLv_Folders) { _Pos.view = kLv_Folders; LibGo(kLoc_Top, 0); SetView(kV_Main); return; }
    if (_Pos.loc == kLoc_Top) { SetView(kV_Main); return; }
    if (_Pos.loc == kLoc_Lib || _Pos.folder == 0 || !CardFolders())
    {
        Where_t w;
        Look(&w);
        const int at = _Pos.loc == kLoc_Lib ? (w.cart ? 1 : 0) : (w.cart ? 1 : 0) + (LibShown(&w) ? 1 : 0);
        LibGo(kLoc_Top, 0);
        _Pos.sel = at;
        return;
    }
    sdc_folder_t f, p;
    const uint32_t from = _Pos.folder;
    if (SdLib_Folder(from, &f) != 0) { LibGo(kLoc_Card, 0); return; }
    LibGo(kLoc_Card, f.parent);
    if (SdLib_Folder(f.parent, &p) == 0)
        for (uint32_t k = 0; k < p.child_count; k++) { uint32_t id; if (SdLib_FolderChild(p.child_first + k, &id) == 0 && id == from) { _Pos.sel = (int)k; break; } }
}
// Match the running card game in this folder without changing the library's location.
// A busy card may defer the entry selection; a user move cancels it.
static void LibSelectPlaying(const Where_t *w, LibRows_t *L)
{
    if (!L->selectPlaying) return;
    if (!Playing() || _Pos.view != kLv_Folders || _Pos.loc != kLoc_Card)
    { L->selectPlaying = 0; return; }
    if (w->card == -2 || w->card == -3) return;
    const bool folders = CardFolders();
    const int n = folders ? (int)L->f.game_count : w->card;
    for (int i = 0; i < n; i++)
    {
        sdc_rec_t rec;
        const int e = folders ? SdLib_DirGet(L->f.game_first + (uint32_t)i, &rec) : SdLib_Get((uint32_t)i, &rec);
        if (e == -2) return;
        if (e) continue;
        Game_t g = { .where = kW_Card };
        snprintf(g.path, sizeof g.path, "%s", rec.path);
        if (!IsPlaying(&g)) continue;
        _Pos.sel = i + (folders ? (int)L->f.child_count : 0);
        break;
    }
    L->selectPlaying = 0;
}
static void LibDraw(lv_obj_t *pScreen, const Where_t *w0)
{
    LibRows_t *L = _Ctx.pLib;
    if (!L) { SetView(kV_Main); return; }
    // fr1 (on the unit: READING came and went every 1-2 s): the card is asked again every 3 s while the page shows
    // (AskLook) and ON CHROMATIC is read again now and then; meanwhile they say "being read" (-2 / -3). The rows already
    // on screen stay (as the panel's list keeps its cursor, 1.1.4); READING... only before anything was read
    Where_t we = *w0;
    if (L->valid && we.card == -2 && L->card >= 0) we.card = L->card;
    if (L->valid && (we.lib == -2 || we.lib == -3) && L->lib >= 0) we.lib = L->lib;
    const Where_t *const w = &we;
    const int n = LibCount(w, L);
    if (n == -2) return;                               // (the card is busy: drawn at the next try)
    LibSelectPlaying(w, L);
    if (_Pos.sel >= n) _Pos.sel = n - 1;
    if (_Pos.sel < 0) _Pos.sel = 0;
    if (_Pos.sel < _Pos.top) _Pos.top = _Pos.sel;
    if (_Pos.sel >= _Pos.top + kFullListRows) _Pos.top = _Pos.sel - kFullListRows + 1;
    if (_Pos.top > n - kFullListRows) _Pos.top = n - kFullListRows;
    if (_Pos.top < 0) _Pos.top = 0;
    if (LibFill(w, L, n)) return;
    char b[SDC_NAME_LEN + 40], t[SDC_NAME_LEN + 24];
    if (n > kFullListRows) { snprintf(b, sizeof b, "%s   %d/%d", L->path, _Pos.sel + 1, n); LineM(pScreen, 0, true, b, 2); }
    else LineM(pScreen, 0, true, L->path, 2);
    for (int r = 0; r < kFullListRows; r++)
    {
        const bool sel = _Pos.top + r == _Pos.sel;
        const uint8_t k = L->kind[r];
        if (k == kR_None) { LineM(pScreen, 1 + r, false, "", 0); continue; }
        if (k == kR_Note) { LineM(pScreen, 1 + r, false, L->text[r], 1); continue; }
        if (k == kR_Folder || k == kR_PlaceLib || k == kR_PlaceCard)
        {
            char u[SDC_NAME_LEN + 8];
            Upper(u, sizeof u, L->text[r]);
            snprintf(t, sizeof t, "%s/   %lu", u, (unsigned long)L->cnt[r]);
        }
        else Spaced(t, sizeof t, L->text[r]);
        snprintf(b, sizeof b, "%s%s", sel ? ">" : "    ", t);   // (four 1-pixel spaces: as wide as the cursor)
        LineM(pScreen, 1 + r, sel, b, sel ? 2 : 1);
    }
    const int sr = _Pos.sel - _Pos.top;
    const uint8_t sk = sr >= 0 && sr < kFullListRows ? L->kind[sr] : kR_None;
    static const char *const kView[kLv_N] = { "A  TO  Z", "LAST  PLAYED", "FOLDERS" };   // (what SELECT shows next)   // (fr1: whole screen)
    if (RowIsGame(sk)) snprintf(b, sizeof b, "A:  PLAY   START:  MORE   B:  BACK");   // (fr1: whole screen)
    else if (sk == kR_Folder || sk == kR_PlaceLib || sk == kR_PlaceCard) snprintf(b, sizeof b, "A:  OPEN   B:  BACK   SELECT:  %s", kView[_Pos.view]);   // (fr1: whole screen)
    else snprintf(b, sizeof b, "B:  BACK   SELECT:  %s", kView[_Pos.view]);   // (fr1: whole screen)
    LineM(pScreen, kFullRows - 1, true, b, 1);
}
static void LibButton(Button_t Button)
{
    LibRows_t *L = _Ctx.pLib;
    if (!L) { SetView(kV_Main); return; }
    const int n = L->valid ? L->n : 0;
    const int r = _Pos.sel - _Pos.top;
    const uint8_t k = (L->valid && r >= 0 && r < kFullListRows) ? L->kind[r] : kR_None;
    switch (Button)
    {
    case kButton_Up:    L->selectPlaying = 0; if (n > 0) _Pos.sel = (_Pos.sel + n - 1) % n; break;
    case kButton_Down:  L->selectPlaying = 0; if (n > 0) _Pos.sel = (_Pos.sel + 1) % n; break;
    case kButton_Left:  L->selectPlaying = 0; _Pos.sel = _Pos.sel > kFullListRows ? _Pos.sel - kFullListRows : 0; break;
    case kButton_Right: L->selectPlaying = 0; if (n > 0) _Pos.sel = _Pos.sel + kFullListRows < n ? _Pos.sel + kFullListRows : n - 1; break;
    case kButton_B:     LibUp(); break;
    case kButton_Select:
        _Pos.view = (uint8_t)((_Pos.view + 1) % kLv_N);
        LibGo(_Pos.view == kLv_Folders ? kLoc_Top : kLoc_Card, 0);
        break;
    case kButton_A:
    case kButton_Start:
    {
        if (k == kR_PlaceLib) { LibGo(kLoc_Lib, 0); break; }
        if (k == kR_PlaceCard) { LibGo(kLoc_Card, 0); break; }
        if (k == kR_Folder) { LibGo(kLoc_Card, L->ref[r]); break; }
        if (!RowIsGame(k)) break;
        Where_t w;
        Look(&w);
        if (LibOpenGame(&w, L, r)) break;
        // A plays at once (an ON CHROMATIC game asks first: its load darkens the screen); START: its actions
        if (Button == kButton_A && OkPlay(_Ctx.pGame) && JobIdle()) RunPlay(_Ctx.pGame);
        else SetView(kV_Game);
        break;
    }
    default: break;
    }
}

static uint8_t _DrawnView = 0xFF;      // fr1: the view the rows were last drawn for (over the whole screen: cleared on a change)
static OSD_Result_t Draw(lv_obj_t *const pScreen);
OSD_Result_t Games_Draw(void *arg)
{
    if (arg == NULL) return kOSD_Result_Err_NullDataPtr;
    if (_Ctx.InTab) { (void)Games_OnTransition(NULL); _Ctx.InTab = 0; }   // (fr1: from the GAMES tab to SYSTEM > GAMES)
    return Draw((lv_obj_t *const)arg);
}
// fr1: the GAMES tab (its own tab: MenuMgr gives it a Tab_DrawCtx_t)
OSD_Result_t GamesTab_Draw(void *arg)
{
    const Tab_DrawCtx_t *const c = (const Tab_DrawCtx_t *)arg;
    if (c == NULL || c->pScreen == NULL) return kOSD_Result_Err_NullDataPtr;
    if (!_Ctx.InTab) { (void)Games_OnTransition(NULL); _Ctx.InTab = 1; }
    if (_Ctx.OpenReq && !_Ctx.PlayView) { _Ctx.OpenReq = 0; if (_Ctx.View == kV_Main || _Ctx.View == kV_Result) LibOpen(); }
    return Draw((lv_obj_t *const)c->pScreen);
}
OSD_Result_t GamesTab_OnButton(const Button_t Button, const ButtonState_t State, void *arg) { return Games_OnButton(Button, State, arg); }
OSD_Result_t GamesTab_OnTransition(void *arg) { return Games_OnTransition(arg); }
void Games_OpenLibrary(void) { _Ctx.OpenReq = 1; }
static OSD_Result_t Draw(lv_obj_t *const pScreen)
{
    if (_Ctx.StopPending)
    {
        const uint32_t now = NowMs();
        if (now - _Ctx.StopAsked >= 10000u)
        {
            printf("[%u ms] osd: reopen timeout\n", (unsigned)esp_log_timestamp());
            _Ctx.StopPending = 0;
        }
        else if (_Ctx.StopPending == 1 && CfRc_StopDone() != _Ctx.StopDone)
        {
            printf("[%u ms] osd: reopen armed (stop end seen)\n", (unsigned)esp_log_timestamp());
            _Ctx.StopPending = 0;
            if (!_StopTimer)
            {
                const esp_timer_create_args_t args = { .callback = StopReopenTimer, .name = "stop_osd" };
                if (esp_timer_create(&args, &_StopTimer) != ESP_OK) _StopTimer = NULL;
            }
            _StopPhysicalEvents = Button_PhysicalEventCount();
            _StopArmed_us = esp_timer_get_time();
            // Covers the 500 ms settle and 120 ms MENU release, bounded to 2 s.
            PwrMgr_KeepAwakeFor(2000);
            __atomic_store_n(&_StopTimerPending, true, __ATOMIC_RELEASE);
            if (!_StopTimer || esp_timer_start_once(_StopTimer, 500 * 1000) != ESP_OK)
            {
                printf("[%u ms] osd: reopen timeout\n", (unsigned)esp_log_timestamp());
                CancelStopReopen();
            }
        }
    }
    if (!_Ctx.Held) { NW_LibHold(true); _Ctx.Held = 1; }
    {
        // fr1: the library, and what it opened, over the whole screen (the GAMES tab only)
        const uint8_t v = _Ctx.View;
        const bool full = _Ctx.InTab && !_Ctx.PlayView &&
                          (v == kV_Lib || (_Ctx.Back == kV_Lib && (v == kV_Game || v == kV_Confirm || v == kV_Busy || v == kV_Result)));
        SetFull(pScreen, full);
        if (full && _DrawnView != v) DeleteAll();
        _DrawnView = v;
    }
    {
        // lane share1: a share's page (started here, or still going) instead of this page's own rows
        ShareRows_t sr;
        if (Share_Rows(&sr)) { for (int i = 0; i < kRows; i++) LineM(pScreen, i, (sr.White >> i) & 1u, sr.T[i], sr.Mode[i]); return kOSD_Result_Ok; }
    }

    // a game loading from here (or anywhere), its result, or no FPGA for it: the play page draws it
    NwPlayView_t pv;
    NW_PlayView(&pv);
    // (a GAMES job's keep shows as "loading" on the RAM cart's view: the job's own busy page stays)
    const bool Play = !_CorePage && Sd_Busy() != kSdOp_Job && (pv.Kind == kNwPv_Loading || pv.Kind == kNwPv_NoFpga || (pv.Kind == kNwPv_Result && _Ctx.PlayView));
    if (Play != (bool)_Ctx.PlayView)
    {
        if (Play) SetFull(pScreen, false);             // (fr1: a game loading: the play page in the panel, as from SYSTEM)
        DeleteAll();
        (void)Play_OnTransition(NULL);
        _Ctx.PlayView = Play;
        if (Play && _Ctx.View != kV_Main && _Ctx.View != kV_Busy) SetView(kV_Main);
        if (!Play) { _Ctx.Held = 0; NW_LibHold(true); _Ctx.Held = 1; }   // (the play page let the library go)
    }
    if (Play) return Play_Draw(pScreen);

    if (_SwitchBusy && _SwitchDone) {
        _SwitchBusy = 0;
        Result(!_SwitchResult ? (_SwitchSlot ? "CORE  SWITCHED|GAME  GEAR  NEXT|TURN  OFF  AND  ON" : "CORE  SWITCHED|GAME  BOY  NEXT|TURN  OFF  AND  ON") :
               _SwitchResult == CF_CORE_NOT_READY ? "NOT  READY|TRY  AGAIN" :
               _SwitchResult == CF_FOTA_E_EMPTY ? "CORE  IS  EMPTY|NOTHING  CHANGED" :
               _SwitchResult == CF_FOTA_E_FULL ? "NO  SWITCHES  LEFT|USE  THE  CABLE" : "SWITCH  FAILED|NOTHING  CHANGED");
    }
    Where_t w;
    Look(&w);
    // m110t: ON CHROMATIC not read (no memory in one piece right after a job: the final gate, unit 1): said, and asked again
    if (w.lib == -3 && NowMs() - _Ctx.LibRetry >= 3000u) { _Ctx.LibRetry = NowMs(); NW_LibHold(true); }
    char b1[64] = "", b2[64] = "", b3[64] = "", b4[64] = "";
    // a job of the card's task started from anywhere (the console too): its progress here
    const bool job = w.sd.op == kSdOp_Job || w.sd.op == kSdOp_Backup || w.sd.op == kSdOp_ToCart || w.sd.op == kSdOp_Delete ||
                     w.sd.op == kSdOp_Format || w.sd.op == kSdOp_FormatExfat;
    if (!_CorePage && job && _Ctx.View != kV_Busy && _Ctx.View != kV_Main) { _Ctx.BusyOwn = 1; SetView(kV_Busy); }
    if (_Ctx.View == kV_Busy && _Ctx.BusyOwn == 1 && !job)
    {
        snprintf(_Ctx.Res, sizeof _Ctx.Res, "%s", w.sd.result[0] ? w.sd.result : "DONE|");
        Sd_ClearResult();
        _Ctx.CacheFrom = -1;
        SetView(kV_Result);
    }
    if (_Ctx.View == kV_Busy && _Ctx.BusyOwn == 3 && _Ctx.pGame)
    {
        if (!IsPlaying(_Ctx.pGame)) DoDelete(_Ctx.pGame);
        else if (NowMs() - _Ctx.StopFrom > 30000u) { snprintf(_Ctx.Res, sizeof _Ctx.Res, "NOT  DELETED|IT  DID  NOT  STOP:|TRY  AGAIN"); SetView(kV_Result); }
    }
    if (_Ctx.View == kV_Busy && _Ctx.BusyOwn == 2 && !NW_LibBusy())
    {
        snprintf(_Ctx.Res, sizeof _Ctx.Res, NW_LibResult() == 0 ? "DELETED|ITS  SAVE  WENT  TOO" : "NOT  DELETED|TRY  AGAIN");
        SetView(kV_Result);
    }

    switch ((View_t)_Ctx.View)
    {
    case kV_Lib:
        // fr2: the card is asked once, when the library opens: the slot is inside the unit (no card is pulled while the menu
        // shows), and games that arrive (Wi-Fi, SHARE, a job) refresh the library themselves. Asking every 3 s (AskLook, as
        // the panel's list does) walked the whole card each time: READING on screen, the card busy, the battery
        // (1.1.12 merge: this one ask is a read of the card's games, whatever the panel's list asked before in this visit)
        if (_Ctx.pLib && !_Ctx.pLib->asked) { _Ctx.LastAsk = 0; _Ctx.ReadAsked = 0; AskLook(); _Ctx.pLib->asked = 1; }
        LibDraw(pScreen, &w);
        break;
    case kV_Main:
    default:
    {
        if (_CorePage) {
            const int slot = CfDev_FpgaSlot(), next = CfDev_FpgaNext(), other = CfDev_FpgaOther();
            const int switches = CfDev_FpgaSwitchesLeft();
            char left[32] = "";
            if (switches >= 0) snprintf(left, sizeof left, "SWITCHES  LEFT:  %d", switches);
            const char *R[kRows] = { "CORE", slot < 0 ? "CORE  NOT  KNOWN" : slot ? "GAME  GEAR" : "GAME  BOY",
                next < 0 ? "RUNNING  NOW" : next ? "GAME  GEAR  NEXT" : "GAME  BOY  NEXT",
                next < 0 ? "" : "TURN  OFF  AND  ON",
                other == CF_CORE_NOT_READY ? "NOT  READY:  RETRY" : left,
                slot < 0 || other == CF_FOTA_E_EMPTY ? "NO  OTHER  CORE" : "A:  SWITCH", "B:  BACK" };
            const bool W[kRows] = { true, true, false, false, false, true, true };
            Rows(pScreen, R, W);
            break;
        }
        AskLook();
        if (w.cart) snprintf(b1, sizeof b1, "CARTRIDGE:  1");
        else snprintf(b1, sizeof b1, "CARTRIDGE:  NONE");
        if (w.lib >= 0) snprintf(b2, sizeof b2, "ON  CHROMATIC:  %d", w.lib);
        else if (w.lib == -2 || w.lib == -3) snprintf(b2, sizeof b2, "ON  CHROMATIC:  ...");   // (-3, m110t: read again; the list says why)
        else if (w.kept) snprintf(b2, sizeof b2, "ON  CHROMATIC:  1");
        if (w.card >= 0) snprintf(b3, sizeof b3, "CARD:  %d", w.card);
        else if (w.card == -3) snprintf(b3, sizeof b3, "READING  THE  CORE");
        else if (w.card == -2) snprintf(b3, sizeof b3, "CARD:  READING");
        else snprintf(b3, sizeof b3, "CARD:  %s", w.sd.st == kSdSt_None ? "NONE" : Sd_StateWord());
        if (job) snprintf(b4, sizeof b4, "%s...", JobWord(w.sd.op == kSdOp_Job ? Sd_JobKind() : w.sd.op == kSdOp_Backup ? kSdJob_Backup : kSdJob_CardToCart));
        else if (pv.Kind == kNwPv_Playing) snprintf(b4, sizeof b4, "PLAYING:  %s", pv.Title);
        LineM(pScreen, 0, true, "GAMES", 0);
        LineM(pScreen, 1, false, CfDev_FpgaSlot() == 1 ? "" : b1, 1);
        LineM(pScreen, 2, false, CfDev_FpgaSlot() == 1 ? "" : b2, 0);
        LineM(pScreen, 3, false, b3, 0);
        LineM(pScreen, 4, false, "", 0);
        LineM(pScreen, 5, job || pv.Kind == kNwPv_Playing, b4, 1);
        LineM(pScreen, 6, true, "A:  OPEN", 0);
        break;
    }
    case kV_List:
    {
        AskLook();
        const int n = Count(&w);
        const int found = FindSel(&w);
        bool parked = false;                       // the selected game's place is being read: the cursor waits for it
        if (found >= 0)
        {
            _Ctx.Sel = found; _Ctx.Away = 0;
            if (_Ctx.Pend) { const int p = _Ctx.Pend; _Ctx.Pend = 0; Move(p > 0 ? 1 : -1, p > 0 ? p : -p); }   // (1.1.6: pressed during the read)
        }
        else if (_Ctx.SelWhere && PlaceReading(&w, _Ctx.SelWhere)) parked = true;
        else if (!Selectable(&w, _Ctx.Sel) || _Ctx.SelWhere)
        {
            // the selected game not there (a card pulled, lent, a delete): the nearest one shown, else the first; the game
            // stays the one wanted (1.1.6), the cursor goes back to it when it is there again
            int best = -1;
            // (from past the list's end too: a list grown shorter)
            for (int d = 0; best < 0 && (_Ctx.Sel - d >= 0 || _Ctx.Sel + d < n); d++) { if (Selectable(&w, _Ctx.Sel - d)) best = _Ctx.Sel - d; else if (Selectable(&w, _Ctx.Sel + d)) best = _Ctx.Sel + d; }
            _Ctx.Sel = best;
            _Ctx.Pend = 0;
            if (_Ctx.SelWhere) _Ctx.Away = 1; else Remember(&w);
        }
        const int at = _Ctx.Sel < 0 ? 0 : _Ctx.Sel;
        if (!parked)
        {
            if (at < _Ctx.Top + 1 && at > 0 && EntAt(&w, at - 1).kind == kE_Hdr) _Ctx.Top = at - 1;   // (its place's name in view)
            else if (at < _Ctx.Top) _Ctx.Top = at;
            if (at >= _Ctx.Top + kListRows) _Ctx.Top = at - kListRows + 1;
        }
        if (_Ctx.Top > n - kListRows) _Ctx.Top = n - kListRows;
        if (_Ctx.Top < 0) _Ctx.Top = 0;
        // the card's names in the window
        int cfrom = -1;
        for (int r = 0; r < kListRows && _Ctx.Top + r < n; r++) { const Ent_t e = EntAt(&w, _Ctx.Top + r); if (e.where == kW_Card && e.kind == kE_Game) { cfrom = e.i; break; } }
        if (cfrom >= 0) FillCard(&w, cfrom);
        FreeLine(&w, b1, sizeof b1);
        LineM(pScreen, 0, false, b1, 2);
        for (int r = 0; r < kListRows; r++)
        {
            const int k = _Ctx.Top + r;
            if (k >= n) { LineM(pScreen, 1 + r, false, "", 0); continue; }
            const Ent_t e = EntAt(&w, k);
            char t[48];
            EntText(&w, &e, t, sizeof t);
            if (e.kind == kE_Game)
            {
                const bool sel = !parked && k == _Ctx.Sel;
                snprintf(b2, sizeof b2, "%s%s", sel ? ">" : "    ", t);   // (four 1-pixel spaces: as wide as the cursor)
                LineM(pScreen, 1 + r, sel, b2, sel ? 2 : 1);
            }
            else LineM(pScreen, 1 + r, false, t, 1);
        }
        LineM(pScreen, 6, true, _Ctx.Sel >= 0 && !parked ? "A:  OPEN   B:  BACK" : "B:  BACK", 0);
        break;
    }
    case kV_Game:
    {
        const Game_t *g = _Ctx.pGame;
        uint8_t a[kActN];
        const int na = Acts(g, a);
        if (_Ctx.Act >= na) _Ctx.Act = na ? (uint8_t)(na - 1) : 0;
        // (more actions than rows: the window follows the selection; savecloud1)
        if (_Ctx.ActTop > _Ctx.Act) _Ctx.ActTop = _Ctx.Act;
        if (_Ctx.Act >= _Ctx.ActTop + kActRows) _Ctx.ActTop = (uint8_t)(_Ctx.Act - kActRows + 1);
        if (na <= kActRows) _Ctx.ActTop = 0;
        else if (_Ctx.ActTop > na - kActRows) _Ctx.ActTop = (uint8_t)(na - kActRows);
        if (g->where == kW_Cart) snprintf(b1, sizeof b1, "CARTRIDGE");
        else if (g->where == kW_Lib) { GameBytes(b3, sizeof b3, g->size); snprintf(b1, sizeof b1, g->size ? "ON  CHROMATIC  %s" : "ON  CHROMATIC", b3); }
        else if (g->flags & SDC_F_PLAYABLE) { GameBytes(b3, sizeof b3, g->size); snprintf(b1, sizeof b1, "CARD  %.16s", b3); }
        else snprintf(b1, sizeof b1, "CANNOT  PLAY  HERE");
        if (IsPlaying(g)) snprintf(b1, sizeof b1, "PLAYING  NOW");
        Spaced(b4, sizeof b4, g ? g->title : "");
        LineM(pScreen, 0, true, b4, 2);
        LineM(pScreen, 1, false, CfDev_FpgaSlot() == 1 ? "" : b1, 1);
        for (int r = 0; r < kActRows; r++)
        {
            if (_Ctx.ActTop + r < na) LineM(pScreen, 2 + r, _Ctx.ActTop + r == _Ctx.Act, kActs[a[_Ctx.ActTop + r]].label, 0);
            else LineM(pScreen, 2 + r, false, (r == 0 && na == 0) ? "NOTHING  TO  DO  NOW" : "", 0);
        }
        break;
    }
    case kV_Confirm:
    {
        const Game_t *g = _Ctx.pGame;
        Spaced(b4, sizeof b4, g ? g->title : "");
        if (_Ctx.Confirm == kC_Delete && IsPlaying(g))
        {
            const char *R[kRows] = { "DELETE  THIS  GAME?", b4, "IT  IS  PLAYING  NOW:", "IT  STOPS,  THEN  IT", g->where == kW_Lib ? "GOES  WITH  ITS  SAVE." : "LEAVES  THE  CARD.",
                                     "A:  STOP  AND  DELETE", "B:  KEEP  IT" };
            const bool W[kRows] = { true, true, false, false, false, true, true };
            for (int i = 0; i < kRows; i++) LineM(pScreen, i, W[i], R[i], i == 1 ? 2 : 0);
        }
        else if (_Ctx.Confirm == kC_Delete)
        {
            const char *R[kRows] = { "DELETE  THIS  GAME?", b4, g->where == kW_Lib ? "IT  LEAVES  THE" : "IT  LEAVES  THE  CARD.",
                                     g->where == kW_Lib ? "CHROMATIC  AND  SO" : "ITS  SAVE  STAYS.", g->where == kW_Lib ? "DOES  ITS  SAVE." : "", "A:  DELETE", "B:  KEEP  IT" };
            const bool W[kRows] = { true, true, false, false, false, true, true };
            for (int i = 0; i < kRows; i++) LineM(pScreen, i, W[i], R[i], i == 1 ? 2 : 0);
        }
        else if (_Ctx.Confirm == kC_Switch)
        {
            const char *R[kRows] = { _CorePage ? "START  NEXT  TIME?" : "THIS  GAME  NEEDS", _SwitchSlot ? "THE  GAME  GEAR  CORE" : "THE  GAME  BOY  CORE",
                                     "AFTER  SWITCHING:", "TURN  OFF  AND  ON", "", "A:  SWITCH", "B:  BACK" };
            const bool W[kRows] = { true, true, false, false, false, true, true };
            Rows(pScreen, R, W);
        }
        else if (_Ctx.Confirm == kC_PlayLib)
        {
            const char *R[kRows] = { "PLAY  THIS  GAME?", b4, "THE  SCREEN  GOES", "DARK  FOR  UP  TO", "20  SECONDS  TO  LOAD.", "A:  PLAY", "B:  BACK" };
            const bool W[kRows] = { true, true, false, false, false, true, true };
            for (int i = 0; i < kRows; i++) LineM(pScreen, i, W[i], R[i], i == 1 ? 2 : 0);
        }
        else if (_Ctx.Confirm == kC_Write)
        {
            const char *R[kRows] = { "WRITE  THIS  GAME", "TO  THE  CARTRIDGE?", b4, "THE  CARTRIDGE  GAME", "IS  BACKED  UP  FIRST.", "A:  WRITE", "B:  NO" };
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
    case kV_Busy:
    {
        if (_Ctx.BusyOwn == 4) {
            const char *R[kRows] = { "SWITCHING  CORE", "", "", "", "DO  NOT  TURN  OFF", "", "B:  HIDE" };
            const bool W[kRows] = { true, false, false, false, false, false, true };
            Rows(pScreen, R, W);
            break;
        }
        const int jk = _Ctx.BusyOwn == 2 ? -1 : w.sd.op == kSdOp_Job ? Sd_JobKind() : w.sd.op == kSdOp_Backup ? kSdJob_Backup : w.sd.op == kSdOp_ToCart ? kSdJob_CardToCart : 0;
        const char *what = _Ctx.BusyOwn == 3 ? "STOPPING  IT" : _Ctx.BusyOwn == 2 ? "DELETING" : w.sd.op == kSdOp_Delete ? "DELETING" : (w.sd.op == kSdOp_Format || w.sd.op == kSdOp_FormatExfat) ? "FORMATTING" : JobWord(jk);
        if (_Ctx.BusyOwn != 2 && w.sd.pct >= 0) snprintf(b1, sizeof b1, "%d%%", w.sd.pct);
        snprintf(b2, sizeof b2, "%s", _Ctx.BusyOwn != 2 && w.sd.op == kSdOp_Job ? Sd_JobStep() : "");
        const bool cart = jk == kSdJob_Backup || jk == kSdJob_CardToCart || jk == kSdJob_LibToCart || jk == kSdJob_CartToLib;
        Spaced(b3, sizeof b3, Sd_JobTitle());
        const char *R[kRows] = { what, b3, b1, b2, cart ? "DO  NOT  TAKE  OUT" : "DO  NOT  TURN  OFF", cart ? "THE  CARTRIDGE" : "", "B:  HIDE" };
        const bool W[kRows] = { true, false, true, false, false, false, true };
        for (int i = 0; i < kRows; i++) LineM(pScreen, i, W[i], R[i], i == 1 ? 1 : 0);
        break;
    }
    case kV_Result:
    {
        const char *bar = strchr(_Ctx.Res, '|');
        const char *bar2 = bar ? strchr(bar + 1, '|') : NULL;
        snprintf(b1, sizeof b1, "%.*s", bar ? (int)(bar - _Ctx.Res) : (int)strlen(_Ctx.Res), _Ctx.Res);
        snprintf(b2, sizeof b2, "%.*s", bar2 ? (int)(bar2 - bar - 1) : bar ? (int)strlen(bar + 1) : 0, bar ? bar + 1 : "");
        snprintf(b3, sizeof b3, "%s", bar2 ? bar2 + 1 : "");
        const char *R[kRows] = { b1, b2, b3, "", "", "A:  OK", "" };
        const bool W[kRows] = { true, false, false, false, false, true, false };
        Rows(pScreen, R, W);
        break;
    }
    }
    return kOSD_Result_Ok;
}

static void Move(int dir, int step)
{
    Where_t w;
    Look(&w);
    // 1.1.6: the game's place being read (the list has no games there now): the press waits for the read
    if (_Ctx.SelWhere && FindSel(&w) < 0 && PlaceReading(&w, _Ctx.SelWhere))
    {
        const int p = _Ctx.Pend + dir * step;
        _Ctx.Pend = (int8_t)(p > 20 ? 20 : p < -20 ? -20 : p);
        return;
    }
    const int n = Count(&w);
    if (n <= 0) return;
    int k = _Ctx.Sel < 0 ? 0 : _Ctx.Sel;
    for (int s = 0; s < step; s++)
    {
        int j = k;
        do { j += dir; } while (j >= 0 && j < n && !Selectable(&w, j));
        if (j < 0 || j >= n)
        {
            if (step > 1) break;                       // (Left / Right stop at the ends)
            j = dir > 0 ? 0 : n - 1;                   // (Up / Down go round)
            while (j >= 0 && j < n && !Selectable(&w, j)) j += dir;
            if (j < 0 || j >= n) return;
        }
        k = j;
    }
    _Ctx.Sel = k;
    Remember(&w);
}

static OSD_Result_t OnCapture(const Button_t Button, const ButtonState_t State, void *arg)
{
    (void)arg;
    if (State != kButtonState_Pressed) return kOSD_Result_Ok;
    switch ((View_t)_Ctx.View)
    {
    case kV_Lib:
        LibButton(Button);
        break;
    case kV_List:
        if (Button == kButton_Up) Move(-1, 1);
        else if (Button == kButton_Down) Move(1, 1);
        else if (Button == kButton_Left) Move(-1, kListRows);
        else if (Button == kButton_Right) Move(1, kListRows);
        else if (Button == kButton_B) SetView(kV_Main);
        else if (Button == kButton_A && _Ctx.Sel >= 0) { Where_t w; Look(&w); if (OpenEnt(&w, _Ctx.Sel) == 0) SetView(kV_Game); }
        break;
    case kV_Game:
    {
        uint8_t a[kActN];
        const int na = Acts(_Ctx.pGame, a);
        if (Button == kButton_Up && na) _Ctx.Act = (uint8_t)((_Ctx.Act + na - 1) % na);
        else if (Button == kButton_Down && na) _Ctx.Act = (uint8_t)((_Ctx.Act + 1) % na);
        else if (Button == kButton_B) SetView((View_t)_Ctx.Back);
        else if (Button == kButton_A && _Ctx.Act < na)
        {
            if (!JobIdle() && a[_Ctx.Act] != 0 && a[_Ctx.Act] != 1) { Result("NOT  NOW|SOMETHING  ELSE  IS|RUNNING:  WAIT"); break; }
            kActs[a[_Ctx.Act]].run(_Ctx.pGame);
        }
        break;
    }
    case kV_Confirm:
        if (Button == kButton_B) SetView(_CorePage ? kV_Main : kV_Game);
        else if (Button == kButton_A)
        {
            const Game_t *g = _Ctx.pGame;
            if (_Ctx.Confirm == kC_Switch) {
                if (Playing()) { Result("STOP  THE  GAME|FIRST"); break; }
                if (_SwitchBusy) break;
                _SwitchDone = 0; _SwitchBusy = 1;
                _Ctx.BusyOwn = 4;
                SetView(kV_Busy);
                PwrMgr_KeepAwakeFor(30000);
#ifndef GAMES_HOST
                if (xTaskCreate(SwitchTask, "core_switch", 4096, NULL, 1, NULL) != pdPASS) {
                    _SwitchBusy = 0;
                    Result("SWITCH  FAILED|NOTHING  CHANGED");
                }
#else
                SwitchTask(NULL);
#endif
            }
            else if (_Ctx.Confirm == kC_PlayLib) PlayNow(g);
            else if (_Ctx.Confirm == kC_Delete)
            {
                // 1.1.3: playing: stopped first (its save kept), the delete once it has stopped
                if (IsPlaying(g)) { NW_PlayButton(1); _Ctx.BusyOwn = 3; _Ctx.StopFrom = NowMs(); (void)Sd_SetJobTitle(g->title); SetView(kV_Busy); }
                else DoDelete(g);
            }
            else (void)Job(g->where == kW_Lib ? kSdJob_LibToCart : kSdJob_CardToCart, g, _Ctx.Confirm == kC_Write ? kSdJf_BackupFirst : 0);
        }
        break;
    case kV_Busy:
        if (Button == kButton_B) SetView(_Ctx.Back == kV_Lib && _Ctx.pLib ? kV_Lib : kV_Main);   // (the job goes on; its result waits in the card's view)
        break;
    case kV_Result:
        if (Button == kButton_A || Button == kButton_B) { CancelStopReopen(); _Ctx.StopPending = 0; _Ctx.CacheFrom = -1; if (_Ctx.pLib) _Ctx.pLib->valid = 0; SetView((View_t)_Ctx.Back); }
        break;
    case kV_Main:
    default:
        break;
    }
    return kOSD_Result_Ok;
}

OSD_Result_t Games_OnButton(const Button_t Button, const ButtonState_t State, void *arg)
{
    (void)arg;
    if (State != kButtonState_Pressed) return kOSD_Result_Ok;
    if (_Ctx.PlayView) return Play_OnButton(Button, State, arg);
    if (Button == kButton_A && _Ctx.View == kV_Main)
    {
        sd_view_t v;
        Sd_View(&v);
        if (v.op == kSdOp_Job || v.op == kSdOp_Backup || v.op == kSdOp_ToCart) { _Ctx.BusyOwn = 1; SetView(kV_Busy); return kOSD_Result_Ok; }
        if (v.result[0] && v.op == kSdOp_None) { snprintf(_Ctx.Res, sizeof _Ctx.Res, "%s", v.result); Sd_ClearResult(); SetView(kV_Result); return kOSD_Result_Ok; }
        if (_Ctx.InTab) { LibOpen(); return kOSD_Result_Ok; }   // (fr1: the GAMES tab opens the library)
        Where_t w;
        Look(&w);
        // 1.1.6 (owner): the list opens at its top, the first game, not where it was left
        _Ctx.Sel = -1; for (int k = 0; k < Count(&w); k++) if (Selectable(&w, k)) { _Ctx.Sel = k; break; }
        Remember(&w);
        _Ctx.Pend = 0;
        _Ctx.Top = 0;
        _Ctx.CacheFrom = -1;
        SetView(kV_List);
    }
    return kOSD_Result_Ok;
}

OSD_Result_t Games_OnTransition(void *arg)
{
    (void)arg;
    _CorePage = false;
    Share_HostLeft();                                  // lane share1 (a share goes on: SYSTEM > DOWNLOAD shows it)
    SetView(kV_Main);                                  // (a job goes on; its result waits in the card's view)
    SetFull(NULL, false);                              // (fr1: the whole screen's background goes too)
    DeleteAll();
    _DrawnView = 0xFF;
    if (_Ctx.PlayView) { (void)Play_OnTransition(NULL); _Ctx.PlayView = 0; }
    if (_Ctx.Held) { NW_LibHold(false); _Ctx.Held = 0; }
    NW_SaveCloudForget();
    _Ctx.LastAsk = 0;
    _Ctx.ReadAsked = 0;
    return kOSD_Result_Ok;
}

// CORE reuses the games page's confirmation, worker, busy page and result.
OSD_Result_t Core_Draw(void *arg)
{
    if (_Ctx.InTab) { (void)Games_OnTransition(NULL); _Ctx.InTab = 0; }
    _CorePage = true;
    return Draw((lv_obj_t *)arg);
}
OSD_Result_t Core_OnButton(const Button_t Button, const ButtonState_t State, void *arg)
{
    _CorePage = true;
    if (_Ctx.View != kV_Main) return OnCapture(Button, State, arg);
    if (Button != kButton_A || State != kButtonState_Pressed) return kOSD_Result_Ok;
    _Ctx.Back = kV_Main;
    const int slot = CfDev_FpgaSlot(), other = CfDev_FpgaOther();
    if (other == CF_CORE_NOT_READY) Result("NOT  READY|TRY  AGAIN");
    else if (slot >= 0 && !other) {
        _SwitchSlot = 1 - slot;
        _Ctx.Confirm = kC_Switch;
        SetView(kV_Confirm);
    }
    return kOSD_Result_Ok;
}

// bench (`gpage`): the rows as drawn, and a button
void Games_Bench(char *out, size_t n)
{
    size_t k = 0;
    for (int i = 0; i < kRows && k < n; i++)
    {
        const char *t = _Ctx.pLabel[i] ? lv_label_get_text(_Ctx.pLabel[i]) : "";
        k += (size_t)snprintf(out + k, n - k, "%s%s%s", i ? "|" : "", (_Ctx.White & (1u << i)) ? "!" : "", t ? t : "");
    }
}
void Games_BenchButton(int b)
{
    const Button_t map[6] = { kButton_A, kButton_B, kButton_Up, kButton_Down, kButton_Left, kButton_Right };
    if (b < 0 || b > 5) return;
    if (_Ctx.Captured) (void)OnCapture(map[b], kButtonState_Pressed, NULL);
    else (void)Games_OnButton(map[b], kButtonState_Pressed, NULL);
}
int Games_BenchView(void) { return _Ctx.View; }

#ifndef GAMES_HOST
// bench console: gpage [show|a|b|up|down|left|right] [MS], then the seven rows as drawn ("!" = white)
static int cmd_gpage(int argc, char **argv)
{
    static const char *const kB[6] = { "a", "b", "up", "down", "left", "right" };
    if (argc >= 2 && !strcmp(argv[1], "show")) NW_OpenPage("GAMES");
    for (int i = 0; argc >= 2 && i < 6; i++) if (!strcmp(argv[1], kB[i])) Games_BenchButton(i);
    vTaskDelay(pdMS_TO_TICKS(argc >= 3 ? atoi(argv[2]) : 400));
    char rows[260];
    Games_Bench(rows, sizeof rows);
    printf("gpage: e=0 view %u rows %s\n", _Ctx.View, rows);
    return 0;
}
void Games_Register(void)
{
    const esp_console_cmd_t cmd = { .command = "gpage", .help = "SYSTEM > GAMES (bench): gpage [show|a|b|up|down|left|right] [MS]", .func = &cmd_gpage };
    (void)esp_console_cmd_register(&cmd);
}
#endif
