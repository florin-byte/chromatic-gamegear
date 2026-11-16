/* games_shots.c (lane ui110dev): SYSTEM > GAMES as the unit draws it, on the Mac. The real page (games.c, and play.c for a
 * game loading) over mock places (a cartridge, the games kept ON CHROMATIC, the card's games, a job of the card's task),
 * driven by its own buttons through every screen; each screen's seven rows are printed as osd_page.py arguments
 * ("!" = white, "~" = a label with a width: cut at the panel's edge, "~." with dots). It also checks the page's rules
 * (which actions each row shows, the confirm before a cartridge write, the job asked) and prints PASS / FAIL.
 * make games-shots renders the PNGs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "esp_timer.h"
#include "osd_shared.h"
#include "nw_dev.h"
#include "sd.h"
#include "../games.h"
#include "../play.h"
#include "../share.h"
#include "tab_shared.h"
int Games_BenchView(void);

static int pass, fail;
#define CHECK(c, ...) do { if (c) pass++; else { fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* ---- LVGL, recorded */
static lv_style_t W = {1}, G = {0};
lv_style_t *OSD_GetStyleTextWhite(void) { return &W; }
lv_style_t *OSD_GetStyleTextGrey(void) { return &G; }
static lv_obj_t *objs[64]; static int nobj;
/* fr1: the library's background and lines (boxes) */
lv_obj_t *lv_obj_create(lv_obj_t *p) { (void)p; lv_obj_t *o = calloc(1, sizeof *o); o->box = 1; objs[nobj++] = o; return o; }
void lv_obj_remove_style_all(lv_obj_t *o) { (void)o; }
void lv_obj_set_pos(lv_obj_t *o, int x, int y) { o->x = x; o->y = y; }
void lv_obj_set_style_bg_color(lv_obj_t *o, lv_color_t c, int sel) { (void)sel; o->rgb = c.rgb; }
void lv_obj_set_style_bg_opa(lv_obj_t *o, int opa, int sel) { (void)o; (void)opa; (void)sel; }
lv_color_t lv_color_hex(uint32_t rgb) { lv_color_t c = { rgb }; return c; }
lv_obj_t *lv_label_create(lv_obj_t *p) { (void)p; lv_obj_t *o = calloc(1, sizeof *o); objs[nobj++] = o; return o; }
void lv_obj_align(lv_obj_t *o, int a, int x, int y) { (void)a; o->x = x; o->y = y; }
void lv_obj_move_foreground(lv_obj_t *o) { (void)o; }
void lv_obj_add_style(lv_obj_t *o, lv_style_t *s, int sel) { (void)sel; o->style = s; }
void lv_obj_remove_style(lv_obj_t *o, lv_style_t *s, int sel) { (void)s; (void)sel; o->style = NULL; }
void lv_label_set_text(lv_obj_t *o, const char *t) { snprintf(o->text, sizeof o->text, "%s", t); }
char *lv_label_get_text(const lv_obj_t *o) { return (char *)o->text; }
void lv_obj_del(lv_obj_t *o) { for (int i = 0; i < nobj; i++) if (objs[i] == o) { objs[i] = objs[--nobj]; break; } free(o); }
void lv_label_set_long_mode(lv_obj_t *o, int m) { o->mode = m; }
void lv_obj_set_size(lv_obj_t *o, int w, int h) { o->h = h; o->w = w == LV_SIZE_CONTENT ? 0 : w; }
static uint32_t Tick = 100000;
uint32_t lv_tick_get(void) { return Tick += 50; }
uint32_t esp_log_timestamp(void) { return Tick; }
static int64_t sleep_us;
int64_t esp_timer_get_time(void) { return (int64_t)Tick * 1000 + sleep_us; }
static uint32_t physical_events, awake_ms;
uint32_t Button_PhysicalEventCount(void) { return physical_events; }
void PwrMgr_KeepAwakeFor(uint32_t ms) { awake_ms = ms; }
struct host_timer { esp_timer_create_args_t args; uint32_t due; int active; };
static struct host_timer stop_timer;
static int timer_create_fail, timer_start_fail;
int esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out)
{
    if (timer_create_fail) return -1;
    stop_timer.args = *args; *out = &stop_timer; return ESP_OK;
}
int esp_timer_start_once(esp_timer_handle_t t, uint64_t us)
{
    if (timer_start_fail) return -1;
    t->due = Tick + (uint32_t)(us / 1000); t->active = 1; return ESP_OK;
}
int esp_timer_stop(esp_timer_handle_t t) { t->active = 0; return ESP_OK; }
static void timer_advance(uint32_t ms)
{
    Tick += ms;
    if (stop_timer.active && (int32_t)(Tick - stop_timer.due) >= 0)
    {
        stop_timer.active = 0;
        stop_timer.args.callback(stop_timer.args.arg);
    }
}
static bool osd_visible = true;
bool OSD_IsVisible(void) { return osd_visible; }
static uint32_t stop_done;
uint32_t CfRc_StopDone(void) { return stop_done; }
static int reopen_calls;
void OSD_ReopenCurrent(void) { reopen_calls++; }
static int open_calls;
static char open_page[24];
static fnOSDButtonCb_t Cap;
void MenuMgr_SetCapture(fnOSDButtonCb_t fn) { Cap = fn; }

static int fpga_slot = -1, marker_gg, switch_calls, switch_slot = -1, switch_result;
static int core_pending;
int CfDev_FpgaSlotPending(void) { return core_pending; }
int CfDev_FpgaSlot(void) { return fpga_slot; }
static int switches_left = -1;
int CfDev_FpgaSwitchesLeft(void) { return switches_left; }
static int other_state, next_slot = -1;
int CfDev_FpgaOther(void) { return other_state; }
int CfDev_FpgaNext(void) { return next_slot; }
int CfDev_FpgaSwitch(int slot) { switch_calls++; switch_slot = slot; return switch_result; }
int Sd_ReadOpen(FIL *f, const char *path, uint32_t *size) { (void)f; (void)path; *size = 32768; return 0; }
int Sd_Read(FIL *f, uint32_t off, void *b, uint32_t n, uint32_t *got)
{
    (void)f;
    CHECK(off == 0x150 && n == 8, "reads the eight-byte wrapper marker");
    memcpy(b, marker_gg ? "GGCHROM1" : "ordinary", 8); *got = 8; return 0;
}
void Sd_Close(FIL *f) { (void)f; }

/* ---- the places */
static int card_reading = 0, lib_reading = 0;   /* 1.1.4: the card read again / lent, the library read */
static int cart = 1, card_st = kSdSt_Ready, card_n = 12, lib_n = 3, sd_op = kSdOp_None, sd_pct = -1, job_kind = 0, lib_busy = 0, lib_res = 0;
static char cart_title[17] = "MR.TETRIS", sd_res[48] = "", job_step[24] = "", job_title[24] = "";
static NwPlayView_t play = { kNwPv_None, "", "", 0 };
static int playing_uid = 0;
static const char *playing_path = "";
static struct { int kind; char path[96]; uint32_t uid; int flags; } asked;
static int deleted_uid = 0, played_uid = 0, play_button = 0;
static char played_path[96] = "", sd_deleted[96] = "";
static const char *const kLib[] = { "POKEMON CRYSTAL", "SHINY KART COLOR", "EMERALD COLOR", "SPARK GARDEN", "SHINY MONSTERS COLOR" };
static const uint32_t kLibSize[] = { 2097152, 524288, 4194304, 262144, 524288 };
static const char *const kCard[] = { "ALMANAC", "DAILY TRACE", "KIRBY'S DREAM LAND 2", "LINKS AWAKENING DX", "MR.TETRIS", "POKEMON YELLOW", "SHINY COLLECTION",
                                      "SHINY CROSSER", "SHINY FIGHTER", "SHINY VILLAGE", "TETRIS DX", "WARIO LAND 3" };

int  NW_LibCount(uint32_t *FreeB, uint32_t *TotalB, uint32_t *Gen) { if (FreeB) *FreeB = 1258291; if (TotalB) *TotalB = 2977792; if (Gen) *Gen = 1; return lib_reading ? -2 : lib_n; }
int  NW_LibGet(int I, NwLibGame_t *Out)
{
    if (I < 0 || I >= lib_n) return -1;
    memset(Out, 0, sizeof *Out);
    Out->Uid = (uint32_t)(I + 1); Out->RomSize = kLibSize[I % 5]; Out->StoredSize = kLibSize[I % 5] / 2; Out->SaveSize = 32768;
    snprintf(Out->Title, sizeof Out->Title, "%s", kLib[I % 5]);
    return 0;
}
static int lib_holds;
void NW_LibHold(bool On) { if (On) lib_holds++; }
int  NW_LibBusy(void) { return lib_busy; }
int  NW_LibResult(void) { return lib_res; }
int  NW_LibDelete(uint32_t Uid) { deleted_uid = (int)Uid; lib_busy = 1; return 0; }
void NW_LibPlay(uint32_t Uid) { played_uid = (int)Uid; }
int  NW_LibPlayingUid(void) { return playing_uid; }
void NW_PlayView(NwPlayView_t *Out) { *Out = play; }
void NW_PlayButton(int A) { play_button = A ? 1 : 2; }
void NW_PlayAfterStop(int Card, uint32_t Uid, const char *Path) { if (Card) snprintf(played_path, sizeof played_path, "%s", Path); else played_uid = (int)Uid; }
void NW_CartTitle(char *Out, unsigned Len) { snprintf(Out, Len, "%s", cart_title); }
bool NW_SdPlayingPath(const char *Path) { return playing_path[0] && !strcmp(Path, playing_path); }
void NW_OpenPage(const char *Item) { open_calls++; snprintf(open_page, sizeof open_page, "%s", Item); }
int Sd_CartIn(void) { return cart; }
int Sd_State(void) { return card_st; }
int Sd_Writable(void) { return card_st == kSdSt_Ready; }
const char *Sd_StateWord(void) { return card_st == kSdSt_NoFs ? "NOT  FORMATTED" : card_st == kSdSt_Error ? "CARD  ERROR" : "NO  CARD"; }
uint32_t Sd_Generation(void) { return 7; }
void Sd_View(sd_view_t *v)
{
    memset(v, 0, sizeof *v);
    v->st = card_st;
    v->games = (card_st == kSdSt_Ready || card_st == kSdSt_ReadOnly) && !card_reading ? card_n : -1;
    v->free_mb = 30528; v->total_mb = 30660;
    v->op = sd_op; v->pct = sd_pct;
    snprintf(v->result, sizeof v->result, "%s", sd_res);
}
int SdLib_Get(uint32_t i, sdc_rec_t *r)
{
    if ((int)i >= card_n) return -1;
    memset(r, 0, sizeof *r);
    snprintf(r->title, sizeof r->title, "%s", kCard[i % 12]);
    snprintf(r->path, sizeof r->path, "/GAMES/%s.gbc", kCard[i % 12]);
    r->size = i == 2 ? 524288 : 1048576; r->flags = SDC_F_PLAYABLE | SDC_F_BATTERY; r->cgb = 0x80;
    if (i == 3) r->flags = 0;                      /* one that cannot play here */
    return 0;
}
/* fr1: the card's folders: / has GAME BOY and GAME BOY COLOR; GAME BOY has RPG and 3 games; RPG 2; GAME BOY COLOR 5 */
static int lib_folders = 0;
static const struct { const char *name; uint32_t parent, cf, cc, gf, gc, total; } kFold[4] = {
    { "", 0, 0, 2, 0, 0, 10 }, { "Game Boy", 0, 2, 1, 0, 3, 5 }, { "Game Boy Color", 0, 3, 0, 5, 5, 5 }, { "RPG", 1, 3, 0, 3, 2, 2 } };
static const uint32_t kOrder[3] = { 1, 2, 3 };
static const char *const kDir[10] = { "ALLEYWAY", "SUPER MARIO LAND", "TETRIS", "POKEMON - BLUE VERSION", "POKEMON - RED VERSION",
    "THE LEGEND OF ZELDA - LINK'S AWAKENING DX", "POKEMON - CRYSTAL VERSION", "SHANTAE", "SUPER MARIO BROS. DELUXE", "WARIO LAND 3" };
static const uint32_t kDirFolder[10] = { 1, 1, 1, 3, 3, 2, 2, 2, 2, 2 };
static char recent[3][2][80] = { { "/GB/Tetris.gb", "TETRIS" }, { "/GBC/Shantae.gbc", "SHANTAE" } };
static int recent_n = 2, recent_added = 0;
int SdLib_Folders(void) { return lib_folders ? 4 : 0; }
int SdLib_Folder(uint32_t id, sdc_folder_t *f)
{
    if (!lib_folders || id >= 4) return -1;
    memset(f, 0, sizeof *f);
    snprintf(f->name, sizeof f->name, "%s", kFold[id].name);
    f->parent = kFold[id].parent; f->child_first = kFold[id].cf; f->child_count = kFold[id].cc;
    f->game_first = kFold[id].gf; f->game_count = kFold[id].gc; f->total = kFold[id].total;
    return 0;
}
int SdLib_FolderChild(uint32_t pos, uint32_t *id) { if (pos >= 3) return -1; *id = kOrder[pos]; return 0; }
int SdLib_DirGet(uint32_t pos, sdc_rec_t *r)
{
    if (pos >= 10) return -1;
    memset(r, 0, sizeof *r);
    snprintf(r->title, sizeof r->title, "%s", kDir[pos]);
    snprintf(r->path, sizeof r->path, "/dir/%u.gbc", (unsigned)pos);
    r->folder = kDirFolder[pos]; r->size = 1048576; r->flags = SDC_F_PLAYABLE | SDC_F_BATTERY; r->cgb = 0x80;
    return 0;
}
int SdLib_Find(const char *path) { (void)path; return 0; }
int SdLib_RecentAdd(const char *path, const char *title) { (void)title; recent_added++; snprintf(recent[2][0], sizeof recent[2][0], "%s", path); return 0; }
int SdLib_Recent(uint32_t k, char *path, size_t pn, char *title, size_t tn)
{
    if ((int)k >= recent_n) return -1;
    snprintf(path, pn, "%s", recent[k][0]); snprintf(title, tn, "%s", recent[k][1]);
    return 0;
}
static int mount_asks;
int Sd_Ask(int op, const char *path) { if (op == kSdOp_Mount) mount_asks++; if (sd_op) return -1; if (op == kSdOp_Delete) { snprintf(sd_deleted, sizeof sd_deleted, "%s", path); sd_op = op; } return 0; }
int Sd_Busy(void) { return sd_op; }
void Sd_ClearResult(void) { sd_res[0] = 0; }
int Sd_AskJob(int kind, const char *path, uint32_t uid, int flags, const char *title)
{
    if (sd_op) return -1;
    asked.kind = kind; snprintf(asked.path, sizeof asked.path, "%s", path ? path : ""); asked.uid = uid; asked.flags = flags;
    snprintf(job_title, sizeof job_title, "%s", title ? title : "");
    sd_op = kSdOp_Job; sd_pct = 0; job_kind = kind;
    return 0;
}
int Sd_JobKind(void) { return job_kind; }
const char *Sd_JobTitle(void) { return job_title; }
const char *Sd_JobStep(void) { return job_step; }
void Sd_JobSetStep(const char *w) { snprintf(job_step, sizeof job_step, "%s", w ? w : ""); }
int Sd_SetJobTitle(const char *t) { snprintf(job_title, sizeof job_title, "%s", t ? t : ""); return 0; }

/* ---- the page, driven */
static lv_obj_t screen;
static const char *Tabs = "NETWORK,*GAMES,DOWNLOAD,UPDATE,SERIAL  #";
static void draw(void) { (void)Games_Draw(&screen); }
static void press(Button_t b)
{
    if (Cap) (void)Cap(b, kButtonState_Pressed, NULL); else (void)Games_OnButton(b, kButtonState_Pressed, NULL);
    draw();
}
static void row(int r, char *out, size_t n)
{
    const lv_obj_t *o = NULL;
    for (int i = 0; i < nobj; i++) if (objs[i]->y == 46 + 9 * r && objs[i]->x == 77) o = objs[i];
    snprintf(out, n, "%s%s%s", o && o->style == &W ? "!" : "", o && o->w ? (o->mode == LV_LABEL_LONG_DOT ? "~." : "~") : "", o ? o->text : "");
}
static int has(const char *t)
{
    char b[128];
    for (int r = 0; r < 7; r++) { row(r, b, sizeof b); if (strstr(b, t)) return 1; }
    return 0;
}
static void shot(const char *name)
{
    draw();
    printf("%s|%s", name, Tabs);
    char b[128];
    for (int r = 0; r < 7; r++) { row(r, b, sizeof b); printf("|%s", b); }
    printf("\n");
}
static void go_list(void) { (void)Games_OnTransition(NULL); draw(); press(kButton_A); }
/* the list: Down until the selected row is this game */
static int pick(const char *name)
{
    char want[64];
    size_t k = 0;
    want[k++] = '>';
    for (const char *q = name; *q && k < sizeof want - 3; q++) { want[k++] = *q; if (*q == ' ') want[k++] = ' '; }
    want[k] = 0;
    for (int i = 0; i < 40; i++) { if (has(want)) return 1; press(kButton_Down); }
    CHECK(0, "%s not found in the list", name);
    return 0;
}
static void finish_job(const char *res) { sd_op = kSdOp_None; sd_pct = -1; snprintf(sd_res, sizeof sd_res, "%s", res); draw(); }

/* ---- fr1: the GAMES tab's library, over the whole screen: a title (8, 4), 12 rows (8, 18 + 9 r), the buttons (8, 133) */
static void tab_draw(void) { Tab_DrawCtx_t c = { NULL, &screen, { 0 } }; (void)GamesTab_Draw(&c); }
static void tpress(Button_t b)
{
    if (Cap) (void)Cap(b, kButtonState_Pressed, NULL); else (void)GamesTab_OnButton(b, kButtonState_Pressed, NULL);
    tab_draw();
}
static void frow(int r, char *out, size_t n)
{
    const int y = r == 0 ? 4 : r == 13 ? 133 : 18 + 9 * (r - 1);
    const lv_obj_t *o = NULL;
    for (int i = 0; i < nobj; i++) if (!objs[i]->box && objs[i]->y == y && objs[i]->x == 8) o = objs[i];
    snprintf(out, n, "%s%s%s", o && o->style == &W ? "!" : "", o && o->w ? (o->mode == LV_LABEL_LONG_DOT ? "~." : "~") : "", o ? o->text : "");
}
static int fhas(const char *t)
{
    char b[200];
    for (int r = 0; r < 14; r++) { frow(r, b, sizeof b); if (strstr(b, t)) return 1; }
    return 0;
}
static int boxes(void) { int n = 0; for (int i = 0; i < nobj; i++) n += objs[i]->box; return n; }
static void fshot(const char *name)
{
    tab_draw();
    printf("%s|FULL", name);
    char b[200];
    for (int r = 0; r < 14; r++) { frow(r, b, sizeof b); printf("|%s", b); }
    printf("\n");
}
static void tshot(const char *name)
{
    tab_draw();
    printf("%s|%s", name, Tabs);
    char b[128];
    for (int r = 0; r < 7; r++) { row(r, b, sizeof b); printf("|%s", b); }
    printf("\n");
}
static void t_library(void)
{
    lib_folders = 1; cart = 1; lib_n = 3; card_n = 12; play.Kind = kNwPv_None; sd_op = kSdOp_None; sd_res[0] = 0;
    (void)Games_OnTransition(NULL); Cap = NULL;
    tab_draw(); tshot("L00_tab_main");
    CHECK(has("CARTRIDGE:  1") && boxes() == 0, "the GAMES tab: its main view in the panel (the tab's picture shows)");
    tpress(kButton_A); fshot("L01_top");
    CHECK(boxes() == 3 && fhas("!~GAMES") && fhas(">CARTRIDGE:  MR.TETRIS") && fhas("ON  CHROMATIC/   3") && fhas("CARD/   12"),
          "A: the library over the whole screen: the places");
    CHECK(fhas("A:  PLAY   START:  MORE   B:  BACK"), "the cartridge selected: A plays");
    tpress(kButton_Down); tpress(kButton_Down); tpress(kButton_A); fshot("L02_card_top");
    CHECK(fhas("CARD") && fhas(">GAME  BOY/   5") && fhas("GAME  BOY  COLOR/   5"), "CARD: its folders (by name, with their games)");
    tpress(kButton_A); fshot("L03_gameboy");
    CHECK(fhas("CARD  /  GAME  BOY") && fhas(">RPG/   2") && fhas("ALLEYWAY") && fhas("SUPER  MARIO  LAND") && fhas("TETRIS"),
          "GAME BOY: its folder first, then its games");
    /* fr1 (on the unit: READING came and went every 1-2 s): the card asked again, ON CHROMATIC read again: the rows stay */
    card_reading = 1; lib_reading = 1; fshot("L03b_card_reread");
    CHECK(fhas(">RPG/   2") && fhas("TETRIS") && !fhas("READING"), "the card read again: the rows stay, no READING");
    card_reading = 0; lib_reading = 0; tab_draw();
    {   /* fr2: the card asked once when the library opens, not every 3 s */
        const int m0 = mount_asks;
        for (int i = 0; i < 200; i++) tab_draw();      /* (200 draws: 10 s of ticks) */
        CHECK(mount_asks == m0, "the open library does not ask the card again (%d asks)", mount_asks - m0);
    }
    tpress(kButton_A); fshot("L04_rpg");
    CHECK(fhas("CARD  /  GAME  BOY  /  RPG") && fhas(">POKEMON  -  BLUE  VERSION") && fhas("POKEMON  -  RED  VERSION"), "RPG: two games, the path on top");
    tpress(kButton_B); fshot("L05_back_up");
    CHECK(fhas(">RPG/   2"), "B: one folder up, RPG still selected");
    tpress(kButton_B); tpress(kButton_Down); tpress(kButton_A); fshot("L06_gbc");
    CHECK(fhas(">THE  LEGEND  OF  ZELDA  -  LINK'S  AWAKENING  DX"), "a long name kept whole (the selected row scrolls)");
    char b[200];
    frow(1, b, sizeof b);
    CHECK(strstr(b, "!~>THE") != NULL, "the selected row: white, a width (it scrolls), the cursor");
    played_path[0] = 0; recent_added = 0;
    tpress(kButton_A);
    CHECK(!strcmp(played_path, "/dir/5.gbc") && recent_added == 1, "A on a game plays it at once, and it goes first in PLAYED LAST");
    CHECK(Games_BenchView() == 0, "after A: the main view (the play page takes the screen)");
    (void)Games_OnTransition(NULL); tab_draw();
    tpress(kButton_A); fshot("L07_back_where_left");
    CHECK(fhas("CARD  /  GAME  BOY  COLOR") && fhas(">THE  LEGEND"), "the library opens again where it was left");
    tpress(kButton_Down); tpress(kButton_Start); fshot("L08_game_page_full");
    CHECK(boxes() == 3 && fhas("POKEMON  -  CRYSTAL  VERSION") && fhas("PLAY") && fhas("DELETE"), "START: the game's actions, over the whole screen");
    tpress(kButton_B); fshot("L09_back_from_game");
    CHECK(fhas(">POKEMON  -  CRYSTAL  VERSION") && fhas("CARD  /  GAME  BOY  COLOR"), "B from the game page: the library, the same row");
    tpress(kButton_Select); fshot("L10_az");
    CHECK(fhas("CARD,  A  TO  Z  (12)") && fhas(">ALMANAC") && fhas("WARIO  LAND  3"), "SELECT: A to Z, the whole card");
    tpress(kButton_Right); fshot("L10b_az_page");
    tpress(kButton_Select); fshot("L11_recent");
    CHECK(fhas("PLAYED  LAST") && fhas(">TETRIS") && fhas("SHANTAE"), "SELECT again: the games played last");
    tpress(kButton_Select); fshot("L12_folders_again");
    CHECK(fhas("!~GAMES") && fhas("CARD/   12"), "SELECT again: the folders, from the places");
    tpress(kButton_B); tab_draw();
    CHECK(boxes() == 0 && has("CARTRIDGE:  1"), "B from the places: the tab's main view, the background gone");
    /* SELECT in the menu: the library at the next draw of the tab */
    const int m1 = mount_asks;
    Games_OpenLibrary(); fshot("L13_select_opens");
    CHECK(mount_asks == m1 + 1, "opening the library asks the card once (%d)", mount_asks - m1);
    CHECK(boxes() == 3 && fhas("!~GAMES"), "SELECT anywhere: the GAMES tab opens its library");
    /* no folder files yet (an old index): the card's games A to Z under CARD */
    lib_folders = 0; tpress(kButton_Down); tpress(kButton_Down); tpress(kButton_A); fshot("L14_card_no_folders");
    CHECK(fhas(">ALMANAC") && fhas("WARIO  LAND  3"), "no folder files: CARD lists its games A to Z");
    /* fr3i: the tenth card game is selected by path on entry, then user moves win. */
    play.Kind = kNwPv_Playing; playing_path = "/GAMES/SHINY VILLAGE.gbc";
    (void)Games_OnTransition(NULL); tab_draw(); tpress(kButton_A);
    CHECK(fhas(">SHINY  VILLAGE"), "reopening selects the tenth card game and shows its row");
    tpress(kButton_Down); tab_draw();
    CHECK(fhas(">TETRIS  DX"), "redraw preserves the user's move away from the running game");
    card_reading = 1; tab_draw(); card_reading = 0; tab_draw();
    CHECK(fhas(">TETRIS  DX"), "card re-read preserves the user's library selection");
    playing_path = "/elsewhere/SHINY VILLAGE.gbc";
    (void)Games_OnTransition(NULL); tab_draw(); tpress(kButton_A);
    CHECK(fhas(">TETRIS  DX"), "same title at another path does not change the selection");
    play.Kind = kNwPv_None; playing_path = "";
    tpress(kButton_B); tpress(kButton_A);
    CHECK(fhas(">ALMANAC"), "entering a card folder without a running game selects its first entry");
    /* SYSTEM > GAMES keeps its panel list */
    (void)Games_Draw(&screen);
    CHECK(boxes() == 0, "SYSTEM > GAMES: no background (the panel)");
    lib_folders = 0; (void)Games_OnTransition(NULL);
    /* Reopen the current tab without a page transition or changing its Back target. */
    play.Kind = kNwPv_Playing; playing_path = "/GAMES/ALMANAC.gbc";
    tab_draw(); tpress(kButton_A); tpress(kButton_Start);
    CHECK(fhas("STOP  PLAYING"), "the tab offers Stop playing for the selected card game");
    const int reopened = reopen_calls;
    tpress(kButton_A);
    osd_visible = false; play.Kind = kNwPv_None; tab_draw();
    CHECK(reopen_calls == reopened, "idle alone cannot reopen the tab");
    stop_done++; tab_draw(); timer_advance(500);
    CHECK(reopen_calls == reopened + 1, "stop end plus settle reopens current tab once");
    CHECK(Games_BenchView() == 5 && fhas("STOPPED") && fhas("ITS  SAVE  IS  KEPT"),
          "current tab keeps the stop result without a queued switch");
    tab_draw(); CHECK(reopen_calls == reopened + 1, "tab does not reopen again");
    osd_visible = true; tpress(kButton_A); playing_path = "";
    CHECK(Games_BenchView() == 6, "A returns to the original library");
}

static void t_core_switch(void)
{
    play.Kind = kNwPv_None; playing_path = ""; playing_uid = 0;
    for (int i = 0; i < 8; i++) {
        fpga_slot = i == 1 || i == 3 ? 1 : i == 4 ? -1 : 0;
        marker_gg = i != 1;
        switch_result = i == 6 ? -4 : i == 7 ? -1 : 0;
        switch_calls = 0; played_path[0] = 0;
        go_list(); pick("ALMANAC"); press(kButton_A); press(kButton_A);
        if (i == 3 || i == 4) {
            CHECK(played_path[0] && !switch_calls, "same / unknown slot starts normally");
            continue;
        }
        CHECK(has(i == 1 ? "THE  GAME  BOY  CORE" : "THE  GAME  GEAR  CORE") && has("A:  SWITCH"), "opposite core asks first");
        if (i == 2) {
            press(kButton_B);
            CHECK(!switch_calls && !played_path[0], "B leaves the core and game unchanged");
            continue;
        }
        if (i == 5) play.Kind = kNwPv_Playing;
        press(kButton_A); draw();
        if (i == 5) {
            CHECK(has("STOP  THE  GAME") && has("FIRST") && !switch_calls, "playing game prevents switching");
            play.Kind = kNwPv_None;
        } else {
            CHECK(switch_calls == 1 && switch_slot == (i == 1 ? 0 : 1), "switch asks for the required slot");
            CHECK(has(i == 6 ? "NO  SWITCHES  LEFT" : i == 7 ? "SWITCH  FAILED" : "CORE  SWITCHED") &&
                  has(i == 6 ? "USE  THE  CABLE" : i == 7 ? "NOTHING  CHANGED" : "TURN  OFF  AND  ON"), "switch result");
            CHECK(!played_path[0], "switch never starts the game");
        }
    }
    fpga_slot = -1; marker_gg = 0;
}

static void core_press(Button_t b)
{
    Core_OnButton(b, kButtonState_Pressed, NULL);
    Core_Draw(&screen);
}
static void t_core_page(void)
{
    Games_OnTransition(NULL);
    fpga_slot = 0; other_state = 0; next_slot = -1;
    play.Kind = kNwPv_None; switch_calls = 0; switch_result = 0;
    Core_Draw(&screen);
    CHECK(has("GAME  BOY") && has("A:  SWITCH"), "CORE shows the running core");
    CHECK(!has("SWITCHES  LEFT"), "unknown switch count is hidden");
    switches_left = 957; Core_Draw(&screen);
    CHECK(has("SWITCHES  LEFT:  957"), "CORE shows remaining switches");
    switches_left = 0; Core_Draw(&screen);
    CHECK(has("SWITCHES  LEFT:  0"), "CORE shows exhausted switches");
    switches_left = -1;
    core_press(kButton_A);
    CHECK(has("THE  GAME  GEAR  CORE"), "CORE confirms the next core");
    core_press(kButton_B);
    CHECK(Games_BenchView() == 0 && !switch_calls, "CORE cancel performs no write");
    core_press(kButton_A); core_press(kButton_A);
    CHECK(switch_calls == 1 && switch_slot == 1 && has("GAME  GEAR  NEXT") && has("TURN  OFF  AND  ON"), "CORE shares switch worker and result");
    core_press(kButton_A);
    next_slot = 1; Core_Draw(&screen);
    CHECK(has("GAME  BOY") && has("GAME  GEAR  NEXT"), "CORE retains running and pending core separately");
    other_state = -100; core_press(kButton_A);
    CHECK(has("NOT  READY") && has("TRY  AGAIN") && switch_calls == 1, "CORE not ready never writes");
    core_press(kButton_A); other_state = -9; Core_Draw(&screen);
    core_press(kButton_A);
    CHECK(Games_BenchView() == 0 && switch_calls == 1, "CORE empty slot has no action");
    other_state = 0; core_press(kButton_A); play.Kind = kNwPv_Playing;
    core_press(kButton_A);
    CHECK(has("STOP  THE  GAME") && switch_calls == 1, "CORE cannot switch while playing");
    Games_OnTransition(NULL); play.Kind = kNwPv_None;
    fpga_slot = 1; draw();
    CHECK(!has("CARTRIDGE:") && !has("ON  CHROMATIC:"), "GG main hides GB sources and counts");
    fpga_slot = -1; next_slot = -1; Games_OnTransition(NULL);
}

/* Resolve the boot slot with the page left open, without a card generation change. */
static void t_core_pending(void)
{
    cart = 0; lib_n = -1; card_n = 12; card_st = kSdSt_Ready;
    lib_folders = 0; play.Kind = kNwPv_None;
    fpga_slot = -1; core_pending = 1;
    go_list();
    CHECK(has("READING  THE  CORE") && !has("ALMANAC"), "pending: panel has a core note, no card games");
    const int opened = Games_BenchView();
    press(kButton_A);
    CHECK(Games_BenchView() == opened, "pending: panel note cannot open a game");
    core_pending = 0; fpga_slot = 0; draw();
    CHECK(has("ALMANAC"), "known GB: open panel refreshes without card change");
    core_pending = 1; fpga_slot = -1;
    (void)Games_OnTransition(NULL); Cap = NULL; tab_draw(); tpress(kButton_A);
    CHECK(fhas("READING  THE  CORE"), "pending: full-screen places show core note");
    tpress(kButton_Select);
    CHECK(fhas("READING  THE  CORE") && !fhas("ALMANAC"), "pending: A-Z shows no games");
    tpress(kButton_Select);
    CHECK(fhas("READING  THE  CORE") && !fhas("TETRIS"), "pending: recent shows no games");
    core_pending = 0; fpga_slot = 1; tab_draw();
    CHECK(fhas("TETRIS"), "known GG: open recent view refreshes");
    core_pending = 1; fpga_slot = -1; tab_draw();
    CHECK(fhas("READING  THE  CORE") && !fhas("TETRIS"), "pending: cached recent games are hidden");
    core_pending = 0; tab_draw();
    CHECK(fhas("TETRIS"), "unknown after deadline: open recent view refreshes");
    tpress(kButton_Select); tpress(kButton_A);
    CHECK(fhas("ALMANAC"), "unknown: card games are available");
    core_pending = 1; tab_draw();
    CHECK(fhas("READING  THE  CORE") && !fhas("ALMANAC"), "pending: open card folder hides cached rows");
    core_pending = 0; fpga_slot = 0; card_n = 1; tab_draw();
    CHECK(fhas("ALMANAC") && !fhas("DAILY"), "known: open folder refreshes with fewer games");
    fpga_slot = -1; cart = 1; lib_n = 3; card_n = 12;
    (void)Games_OnTransition(NULL); Cap = NULL;
}

int main(void)
{
    draw(); shot("01_main");
    CHECK(has("CARTRIDGE:  1") && has("ON  CHROMATIC:  3") && has("CARD:  12"), "main rows");
    press(kButton_A); shot("02_list_top");
    CHECK(has("CARTRIDGE") && has(">MR.TETRIS") && has("ON  CHROMATIC") && has("POKEMON  CRYSTAL"), "list top: the cartridge first, selected");
    press(kButton_Down); shot("03_list_lib_selected");
    CHECK(has(">POKEMON  CRYSTAL"), "down: the first kept game (the header skipped)");
    press(kButton_Down); press(kButton_Down); press(kButton_Down); shot("04_list_card_section");
    CHECK(has(">ALMANAC") && has("CARD"), "down x3: the card's first game, its header in view");
    press(kButton_Right); shot("05_list_card_page");
    press(kButton_Up); press(kButton_Up); press(kButton_Up); press(kButton_Up); press(kButton_Up); press(kButton_Up); press(kButton_Up); press(kButton_Up); press(kButton_Up);
    /* 1.1.4 (a player: "menu keeps jumping back up to cartridge when trying to scroll sd card games"): the card read again
       (every 3 s while the list shows, or lent for the network) and the library read: the cursor stays on its game */
    go_list(); pick("SHINY CROSSER");
    card_reading = 1; shot("05b_list_card_reread");
    CHECK(!has(">MR.TETRIS") && !has(">POKEMON") && !has(">SHINY  KART") && !has(">EMERALD"), "card read again: the cursor does not jump up to another place");
    card_reading = 0; shot("05c_list_card_back");
    CHECK(has(">SHINY  CROSSER"), "card back: the cursor on the same game");
    lib_reading = 1; shot("05d_list_lib_reread");
    CHECK(has(">SHINY  CROSSER"), "library read again: the card game stays selected (its row moved)");
    lib_reading = 0; shot("05e_list_lib_back");
    CHECK(has(">SHINY  CROSSER"), "library back: still the same game");
    press(kButton_Down); CHECK(has(">SHINY  FIGHTER"), "and Down goes on from it");
    card_n = 8; shot("05f_list_card_shorter");
    CHECK(has(">SHINY  CROSSER"), "the card's list shorter but the game still there: kept");
    card_n = 12;
    for (int i = 0; i < 12 && !(has(">MR.TETRIS") && has("CARTRIDGE")); i++) press(kButton_Up);   /* (back to the cartridge for what follows) */
    CHECK(has(">MR.TETRIS") && has("CARTRIDGE"), "Up back to the cartridge");
    /* the cartridge's actions */
    go_list(); press(kButton_A); shot("06_game_cart");
    CHECK(has("PLAY") && has("BACK  UP  TO  CARD") && has("KEEP  ON  CHROMATIC") && !has("DELETE") && !has("WRITE  TO"), "cartridge actions");
    press(kButton_Down); press(kButton_A); shot("07_busy_backup");
    CHECK(asked.kind == kSdJob_Backup, "BACK UP TO CARD asks the backup job");
    sd_pct = 42; Sd_JobSetStep(""); shot("08_busy_backup_42");
    finish_job("CART  BACKED  UP|GAME  +  SAVE  ON  CARD"); shot("09_result_backup");
    press(kButton_A); shot("10_back_to_list");
    /* a kept game's actions */
    press(kButton_Down); press(kButton_A); shot("11a_game_lib_big");
    CHECK(has("PLAY") && has("COPY  TO  CARD") && !has("WRITE  TO  CARTRIDGE") && has("DELETE"), "a kept 2 MiB game: no WRITE TO CARTRIDGE (the cartridge takes 1.94 MB)");
    press(kButton_B); pick("SHINY KART COLOR"); press(kButton_A); shot("11_game_lib");
    CHECK(has("PLAY") && has("COPY  TO  CARD") && has("WRITE  TO  CARTRIDGE") && has("DELETE") && !has("KEEP  ON"), "kept game actions");
    press(kButton_A); shot("11b_play_lib_notice");
    CHECK(has("PLAY  THIS  GAME?") && has("THE  SCREEN  GOES") && has("20  SECONDS  TO  LOAD.") && played_uid != 2, "1.1.2: PLAY on a kept game says the screen goes dark first, nothing started yet");
    press(kButton_B); press(kButton_A); press(kButton_A);
    CHECK(played_uid == 2, "PLAY asks the kept game (uid 2) after A on the notice, got %d", played_uid);
    go_list(); pick("SHINY KART COLOR"); press(kButton_A); press(kButton_Down); press(kButton_A); Sd_JobSetStep("COPYING"); sd_pct = 63; shot("12_busy_copy_to_card");
    CHECK(asked.kind == kSdJob_LibToCard && asked.uid == 2, "COPY TO CARD asks lib2card uid 2");
    finish_job("COPIED  TO  THE  CARD|"); press(kButton_A);
    pick("SHINY KART COLOR"); press(kButton_A); press(kButton_Down); press(kButton_Down); press(kButton_A); shot("13_confirm_write_backup");
    CHECK(has("TO  THE  CARTRIDGE?") && has("BACKED  UP  FIRST"), "write: asked once more, with the backup");
    press(kButton_A); Sd_JobSetStep("BACKING  UP  FIRST"); sd_pct = 12; shot("14_busy_write_backup_first");
    CHECK(asked.kind == kSdJob_LibToCart && (asked.flags & kSdJf_BackupFirst), "write: lib2cart with the backup first");
    Sd_JobSetStep("WRITING"); sd_pct = 77; shot("15_busy_write_writing");
    finish_job("CARTRIDGE  WRITTEN|OLD  GAME  ON  CARD"); shot("16_result_written");
    press(kButton_A);
    pick("SHINY KART COLOR"); press(kButton_A); press(kButton_Down); press(kButton_Down); press(kButton_Down); press(kButton_A); shot("17_confirm_delete_lib");
    press(kButton_A); shot("18_busy_delete_lib");
    CHECK(deleted_uid == 2, "DELETE asks the kept game's delete (%d)", deleted_uid);
    lib_busy = 0; lib_res = 0; draw(); shot("19_result_deleted");
    press(kButton_A);
    /* a card game's actions */
    go_list(); pick("KIRBY'S DREAM LAND 2");
    press(kButton_A); shot("20_game_card");
    CHECK(has("PLAY") && has("KEEP  ON  CHROMATIC") && has("WRITE  TO  CARTRIDGE") && has("DELETE") && !has("COPY  TO"), "card game actions");
    press(kButton_Down); press(kButton_A); Sd_JobSetStep("PACKING  IT"); sd_pct = 30; shot("21_busy_keep_packing");
    CHECK(asked.kind == kSdJob_CardToLib && !strcmp(asked.path, "/GAMES/KIRBY'S DREAM LAND 2.gbc"), "KEEP ON CHROMATIC asks card2lib of the file (%s)", asked.path);
    Sd_JobSetStep("KEEPING  IT"); sd_pct = 80; shot("22_busy_keep_keeping");
    finish_job("KEPT  ON  CHROMATIC|IT  USES  130  KB"); shot("23_result_kept");
    press(kButton_A);
    pick("KIRBY'S DREAM LAND 2"); press(kButton_A); press(kButton_Down); press(kButton_Down); press(kButton_Down); press(kButton_A); shot("24_confirm_delete_card");
    press(kButton_A);
    CHECK(!strcmp(sd_deleted, "/GAMES/KIRBY'S DREAM LAND 2.gbc"), "card delete asked of the file (%s)", sd_deleted);
    finish_job("GAME  DELETED|ITS  SAVE  STAYS"); shot("25_result_card_deleted");
    press(kButton_A);
    /* no card in: WRITE TO CARTRIDGE asks NO BACKUP: WRITE ANYWAY? */
    card_st = kSdSt_None; go_list(); pick("SHINY KART COLOR"); press(kButton_A); shot("26_game_lib_no_card");
    CHECK(!has("COPY  TO  CARD"), "no card: no COPY TO CARD");
    press(kButton_Down); press(kButton_A); shot("27_confirm_no_backup");
    CHECK(has("NO  BACKUP:") && has("WRITE  ANYWAY?"), "no card: WRITE ANYWAY asked");
    press(kButton_B); shot("28_back_from_no_backup");
    press(kButton_B); press(kButton_B); press(kButton_B); (void)Games_OnTransition(NULL); draw(); shot("29_main_no_card");
    press(kButton_A); shot("30_list_no_card");
    /* no cartridge, an empty library, a card being read */
    cart = 0; lib_n = 0; card_st = kSdSt_Ready; card_n = 0; go_list(); shot("31_list_nothing");
    (void)Games_OnTransition(NULL); draw(); shot("32_main_nothing");
    /* a game playing: its row offers STOP PLAYING, the cartridge row PLAY = back to the cartridge */
    cart = 1; lib_n = 3; card_n = 12; play.Kind = kNwPv_Playing; snprintf(play.Title, sizeof play.Title, "SHINY KART"); playing_uid = 2;
    (void)Games_OnTransition(NULL); draw(); shot("33_main_playing");
    press(kButton_A); pick("SHINY KART COLOR"); press(kButton_A); shot("34_game_playing");
    CHECK(has("STOP  PLAYING") && has("DELETE") && has("PLAYING  NOW"), "the game playing: STOP PLAYING, and DELETE too (1.1.3)");
    press(kButton_A);
    CHECK(play_button == 1, "STOP PLAYING stops it");
    CHECK(reopen_calls == 0, "no reopen while still playing and visible");
    osd_visible = false; draw();
    play.Kind = kNwPv_None; draw();
    CHECK(reopen_calls == 0, "idle and hidden without stop end cannot reopen");
    stop_done++; draw();
    CHECK(reopen_calls == 0, "stop end only arms the reopen");
    CHECK(awake_ms == 2000, "arming requests a bounded awake window covering MENU release");
    timer_advance(499);
    CHECK(reopen_calls == 0, "no reopen before 500 ms without Draw");
    timer_advance(1);
    CHECK(reopen_calls == 1 && open_calls == 0, "settled stop reveals current page without switching");
    for (int i = 0; i < 5; i++) draw();
    CHECK(reopen_calls == 1 && Games_BenchView() == 5 && has("STOPPED") && has("ITS  SAVE  IS  KEPT"),
          "one reopen, the stop result stays current until A");
    osd_visible = true;
    shot("35_result_stopped");
    press(kButton_A);
    /* Hidden pages need no more Draw calls after arming. */
    play.Kind = kNwPv_Playing;
    go_list(); pick("SHINY KART COLOR"); press(kButton_A); press(kButton_A);
    stop_done++; draw();
    timer_advance(500);
    CHECK(reopen_calls == 1, "visible OSD suppresses timer MENU pulse");
    press(kButton_A);
    play.Kind = kNwPv_Playing;
    go_list(); pick("SHINY KART COLOR"); press(kButton_A); press(kButton_A);
    stop_done++; draw(); press(kButton_A); osd_visible = false;
    timer_advance(500);
    CHECK(reopen_calls == 1, "dismissal cancels the armed timer");
    osd_visible = true;
    play.Kind = kNwPv_Playing;
    go_list(); pick("SHINY KART COLOR"); press(kButton_A); press(kButton_A);
    stop_done++; draw(); osd_visible = false;
    timer_advance(10000);
    CHECK(reopen_calls == 1, "overdue timer times out without Draw");
    osd_visible = true; press(kButton_A);
    /* A real button event cancels even if visibility still says hidden. */
    play.Kind = kNwPv_Playing;
    go_list(); pick("SHINY KART COLOR"); press(kButton_A); press(kButton_A);
    stop_done++; draw(); osd_visible = false;
    physical_events++;
    timer_advance(500);
    CHECK(reopen_calls == 1, "physical event suppresses late automatic MENU while hidden");
    osd_visible = true; press(kButton_A);
    /* Simulate light sleep: real time advances independently of the OS tick. */
    play.Kind = kNwPv_Playing;
    go_list(); pick("SHINY KART COLOR"); press(kButton_A); press(kButton_A);
    stop_done++; draw(); osd_visible = false;
    sleep_us += 1001000;
    timer_advance(500);
    CHECK(reopen_calls == 1, "1501 ms real elapsed suppresses MENU despite only 500 ms of ticks");
    osd_visible = true; press(kButton_A);
    timer_start_fail = 1;
    play.Kind = kNwPv_Playing;
    go_list(); pick("SHINY KART COLOR"); press(kButton_A); press(kButton_A);
    stop_done++; draw(); timer_start_fail = 0;
    osd_visible = false; timer_advance(500);
    CHECK(reopen_calls == 1 && !stop_timer.active, "timer start failure leaves no reopen pending");
    osd_visible = true; press(kButton_A);
    play.Kind = kNwPv_Playing;
    go_list(); pick("SHINY KART COLOR"); press(kButton_A); press(kButton_A);
    Tick += 10000; draw();
    osd_visible = false; play.Kind = kNwPv_None; stop_done++; draw(); timer_advance(500);
    CHECK(reopen_calls == 1, "stop timeout ignores a late stop end");
    osd_visible = true; press(kButton_A); play.Kind = kNwPv_Playing;
    /* 1.1.3: DELETE on the game playing: its confirm says it stops first; A stops it, the delete once it has stopped */
    go_list(); pick("SHINY KART COLOR"); press(kButton_A); press(kButton_Up); shot("35b_game_playing_delete");
    press(kButton_A); shot("35c_confirm_delete_playing");
    CHECK(has("IT  IS  PLAYING  NOW:") && has("A:  STOP  AND  DELETE"), "DELETE while it plays: the confirm says it stops first");
    play_button = 0; deleted_uid = -1;
    press(kButton_A); shot("35d_stopping");
    CHECK(play_button == 1 && deleted_uid == -1 && has("STOPPING  IT"), "A stops it first, nothing deleted yet");
    play.Kind = kNwPv_None; draw();
    CHECK(deleted_uid == 2, "stopped: then deleted (uid 2), got %d", deleted_uid);
    lib_busy = 0; lib_res = 0; draw(); shot("35e_deleted");
    CHECK(has("DELETED"), "DELETED");
    press(kButton_A); play.Kind = kNwPv_Playing;
    /* a game loading: the play page */
    play.Kind = kNwPv_Loading; play.Pct = 64; snprintf(play.Title, sizeof play.Title, "POKEMON CRYSTAL"); draw(); shot("36_loading");
    play.Kind = kNwPv_None; playing_uid = 0; draw();
    /* a job running when the page opens: its progress at once */
    (void)Games_OnTransition(NULL); sd_op = kSdOp_Job; job_kind = kSdJob_CartToLib; Sd_JobSetStep("READING  THE  CART"); sd_pct = 18; snprintf(job_title, sizeof job_title, "MR.TETRIS");
    draw(); shot("37_main_job_running");
    press(kButton_A); shot("38_busy_from_main");
    press(kButton_B); shot("39_busy_hidden");
    finish_job("NOT  KEPT|NOT  ENOUGH  ROOM|DELETE  ONE  FIRST"); press(kButton_A); shot("40_result_not_kept");
    /* m110s: SHARE that cannot start says why (m110f, unit 1: A on SHARE from the CARD did nothing at all: no memory for its task) */
    {
        extern int GamesShare_Start, GamesShare_Calls;
        static const struct { int e; const char *name, *r0, *r1, *r2; } kS[] = {
            { kShare_NoMemory, "41_share_no_memory", "NOT  SENT", "NOT  ENOUGH  MEMORY", "TURN  IT  OFF  AND  ON" },
            { kShare_Busy, "42_share_busy", "NOT  NOW", "SOMETHING  ELSE  IS", "RUNNING:  WAIT" } };   /* (lane m110u: plain words) */
        for (unsigned i = 0; i < 2; i++) {
            GamesShare_Start = kS[i].e; GamesShare_Calls = 0;
            go_list(); pick("MR.TETRIS"); pick("MR.TETRIS");   /* (the cartridge's row first, then the card's) */
            press(kButton_Down); pick("MR.TETRIS"); press(kButton_A);
            for (int k = 0; k < 6 && !has("!SHARE"); k++) press(kButton_Down);
            CHECK(has("!SHARE") && has("CARD"), "a card game offers SHARE");
            press(kButton_A); shot(kS[i].name);
            char b0[64], b1[64], b2[64]; row(0, b0, sizeof b0); row(1, b1, sizeof b1); row(2, b2, sizeof b2);
            CHECK(GamesShare_Calls == 1 && !strcmp(b0 + 1, kS[i].r0) && !strcmp(b1, kS[i].r1) && !strcmp(b2, kS[i].r2) && has("!A:  OK"),
                  "SHARE not started (%d): |%s|%s|%s|", kS[i].e, b0, b1, b2);
            press(kButton_A);
            CHECK(Games_BenchView() == 1, "A: OK goes back to the list (view %d)", Games_BenchView());
        }
        GamesShare_Start = -1;
    }
    /* m110t: ON CHROMATIC not read for want of memory (the final gate, unit 1, GAMES right after a GET GAMES: READING... then
       the section gone): a line, never a section gone, and asked again every 3 s until it reads */
    {
        const int was = lib_n;
        lib_n = -3;
        (void)Games_OnTransition(NULL); draw(); shot("43_lib_not_read_main");
        CHECK(has("ON  CHROMATIC:  ...") && has("CARTRIDGE:  1") && has("CARD:  12"), "no memory for ON CHROMATIC: the main page keeps its row (...)");
        press(kButton_A); pick("MR.TETRIS"); shot("44_lib_not_read_list");
        CHECK(has("ON  CHROMATIC") && has("STILL  LOOKING...") && has(">MR.TETRIS"), "the list keeps ON CHROMATIC with its line");
        const int h0 = lib_holds;
        for (int i = 0; i < 40; i++) draw();               /* (40 draws: 2 s of ticks) */
        const int h1 = lib_holds;
        for (int i = 0; i < 60; i++) draw();               /* (3 s more) */
        CHECK(h1 - h0 <= 1 && lib_holds - h0 >= 1 && lib_holds - h0 <= 3, "asked again about every 3 s (%d then %d)", h1 - h0, lib_holds - h0);
        lib_n = was; draw();
        CHECK(has("ON  CHROMATIC") && has("POKEMON  CRYSTAL") && !has("NOT  READ"), "read at last: its games");
        const int h2 = lib_holds;
        for (int i = 0; i < 100; i++) draw();
        CHECK(lib_holds == h2, "read: no more asking (%d)", lib_holds - h2);
    }
    t_core_switch();
    t_core_page();
    t_library();
    t_core_pending();
    fprintf(stderr, "games_shots: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
