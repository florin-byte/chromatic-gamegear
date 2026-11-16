/* gg_shots.c (lane ui110dev): SYSTEM > GET GAMES as the unit draws it, on the Mac. The real page (getgames.c) over the
 * REAL install machine (nw/cartflash/cf_install.c, catalog mode) with a scripted server (the catalog's pages, the order,
 * the poll that brings the offer, the install's result); every screen's rows printed for osd_page.py, the page's rules
 * checked (PASS / FAIL on stderr). make gg-shots renders the PNGs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "osd_shared.h"
#include "nw_dev.h"
#include "sd.h"
#include "../getgames.h"
#include "cf_install.h"
#include "cf_proto.h"

static int pass, fail;
#define CHECK(c, ...) do { if (c) pass++; else { fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static lv_style_t W = {1}, G = {0};
lv_style_t *OSD_GetStyleTextWhite(void) { return &W; }
lv_style_t *OSD_GetStyleTextGrey(void) { return &G; }
static lv_obj_t *objs[32]; static int nobj;
lv_obj_t *lv_label_create(lv_obj_t *p) { (void)p; lv_obj_t *o = calloc(1, sizeof *o); objs[nobj++] = o; return o; }
void lv_obj_align(lv_obj_t *o, int a, int x, int y) { (void)a; o->x = x; o->y = y; }
void lv_obj_move_foreground(lv_obj_t *o) { (void)o; }
void lv_obj_add_style(lv_obj_t *o, lv_style_t *s, int sel) { (void)sel; o->style = s; }
void lv_obj_remove_style(lv_obj_t *o, lv_style_t *s, int sel) { (void)s; (void)sel; o->style = NULL; }
void lv_label_set_text(lv_obj_t *o, const char *t) { snprintf(o->text, sizeof o->text, "%s", t); }
char *lv_label_get_text(const lv_obj_t *o) { return (char *)o->text; }
void lv_obj_del(lv_obj_t *o) { for (int i = 0; i < nobj; i++) if (objs[i] == o) { objs[i] = objs[--nobj]; break; } free(o); }
void lv_label_set_long_mode(lv_obj_t *o, int m) { o->mode = m; }
void lv_obj_set_size(lv_obj_t *o, int w, int h) { (void)h; o->w = w == LV_SIZE_CONTENT ? 0 : w; }
static uint32_t T;
uint32_t lv_tick_get(void) { return T; }
static fnOSDButtonCb_t Cap;
void MenuMgr_SetCapture(fnOSDButtonCb_t fn) { Cap = fn; }

/* ---- the unit's places */
static int lib_n = 3, card = kSdSt_Ready, cart_in = 1, paired = 1;
int  NW_LibCount(uint32_t *FreeB, uint32_t *TotalB, uint32_t *Gen) { if (FreeB) *FreeB = 1258291; if (TotalB) *TotalB = 2977792; if (Gen) *Gen = 1; return lib_n; }
int  Sd_State(void) { return card; }
int  Sd_Writable(void) { return card == kSdSt_Ready; }
int  Sd_Lent(void) { return 0; }
int  Sd_CartIn(void) { return cart_in; }
int  NW_Paired(void) { return paired; }
static int gg_owns;
void NW_GgOwnJob(int On) { gg_owns = On; }
int  NW_GgOwnsJob(void) { return gg_owns; }
void NW_OpenPage(const char *Item) { (void)Item; }
/* the card's job (the cartridge's backup): runs for a while, then its words */
static int job_ticks = -1, job_ok = 1, n_jobs;
static char job_res[48];
int  Sd_AskJob(int kind, const char *path, uint32_t uid, int flags, const char *title) { (void)path; (void)uid; (void)flags; (void)title; if (job_ticks > 0 || kind != kSdJob_Backup) return -1; n_jobs++; job_ticks = 300; job_res[0] = 0; return 0; }
void Sd_View(sd_view_t *v)
{
    memset(v, 0, sizeof *v);
    v->st = card; v->free_mb = 20000; v->pct = -1;
    if (job_ticks > 0) { v->op = kSdOp_Job; v->pct = 100 - job_ticks / 3; if (--job_ticks == 0) snprintf(job_res, sizeof job_res, "%s", job_ok ? "CART  BACKED  UP|GAME  +  SAVE  ON  CARD" : "NOT  BACKED  UP|CARD  WRITE  FAILED"); }
    snprintf(v->result, sizeof v->result, "%s", job_ticks > 0 ? "" : job_res);
}
void Sd_ClearResult(void) { job_res[0] = 0; job_ticks = -1; }

/* ---- the machine, with a scripted server */
static cf_inst_t C;
static int wifi = 2, job = -1, arm_ret = 0, order_code = 200, page_code = 200;
static char poll_answer[1200], order_answer[200], last_order[200];
static int n_page_calls, n_orders, n_cancels;
#define OFFERF " \"pending\": true, \"id\": \"0123456789abcdef0123456789abcdef\", \"title\": \"SHINY KART COLOR\", \"size\": 166933," \
    " \"crc32\": \"501a1d4b\", \"sha256\": \"2d48b2156992f3fbc71161d349c386cd866fed7f3b94499de3bae4c415781511\"," \
    " \"enc_alg\": \"AES-256-CTR\", \"enc_key\": \"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\"," \
    " \"enc_iv\": \"f0f1f2f3f4f5f6f7f8f9fafbfffffffe\", \"blob_url\": \"http://dl.shinyrelay.com/gbnet/v1/install/0123456789abcdef0123456789abcdef/blob/00112233445566778899aabbccddeeff\"}"
static const char *PAGES[2] = {
    "SGC1|0|2|8\nkart|262144|SHINY KART COLOR|RACE EIGHT TINKER-BOTS IN THREE-WHEEL BUGGIES...\nfarm|262144|SHINY FARM COLOR|GROW CROPS, SELL THEM, AND BUILD UP A SMALL...\n"
    "monsters|524288|SHINY MONSTERS COLOR|TEAM UP WITH A YOUNG MONSTER, CROSS BRAMBLE...\nvillage|262144|SHINY VILLAGE COLOR|TAKE OVER THE OLD LAMP KEEPER'S COTTAGE IN...\n"
    "collection|1769472|SHINY COLLECTION|ALL 13 FREE SHINY GEN GAMES AND APPS ON ONE...\n",
    "SGC1|1|2|8\nfighter|524288|SHINY FIGHTER COLOR|A FIGHTING GAME.\nalmanac|524288|SHINY ALMANAC|THE WEATHER, THE SKY AND THE WORLD.\ncrosser|131072|SHINY CROSSER|CROSS THE ROAD.\n" };
static const char *MINE = "SGM1|0|1|3\n0123456789ab|2097152|POKEMON  CRYSTAL|2 SAVES|2\n00000000beef|6291456|A  VERY  BIG  ONE||0\n0000cafe0000|262144|TETRIS  DX||0\n";
static int mine_empty, n_save_calls;
static int ack_slow, no_offer;   /* lane m110x: the result's report slow (ticks); an order the server takes but whose game never comes */
static int pc;
static int a_start(void *u, const char *path, const char *body, char *out, int cap)
{
    (void)u;
    pc = 3;
    if (!strcmp(path, "/link_device/cart_install/ack") && ack_slow) pc = ack_slow;
    if (!strcmp(path, "/link_device/recv/start")) { snprintf(out, cap, "{\"code\": \"KX7M2Q\", \"expires_in\": 600, \"poll_ms\": 4000}"); return 1; }
    if (!strcmp(path, "/link_device/recv/poll")) { snprintf(out, cap, "%s", poll_answer); return 1; }
    if (!strcmp(path, "/link_device/recv/cancel")) { n_cancels++; snprintf(out, cap, "{}"); return 1; }
    if (!strcmp(path, "/link_device/my_games")) { n_page_calls++; snprintf(out, cap, "%s", mine_empty ? "SGM1|0|0|0\n" : MINE); return 1; }
    if (!strcmp(path, "/link_device/my_games/saves")) { n_save_calls++; snprintf(out, cap, "%s", "SGS1|2\n0a0b0c0d|32816|JOHTO  DONE\n1a1b1c1d|32816|BEFORE  THE  BOSS\n"); return 1; }
    if (!strcmp(path, "/link_device/catalog")) { n_page_calls++; const char *p = strstr(body, "\"p\":"); snprintf(out, cap, "%s", PAGES[p && p[4] == '1' ? 1 : 0]); return 1; }
    if (!strcmp(path, "/link_device/catalog/order")) {
        n_orders++; snprintf(last_order, sizeof last_order, "%s", body); snprintf(out, cap, "%s", order_answer);
        if (order_code == 200 && no_offer) snprintf(poll_answer, sizeof poll_answer, "{\"session\": \"waiting\", \"expires_in\": 590}");
        else if (order_code == 200) {
            char of[900];
            snprintf(of, sizeof of, "%s", OFFERF);
            of[strstr(of, "0123456789abcdef0123456789abcdef") - of] = (char)('0' + n_orders % 10);   /* (each order its own install id) */
            char *b2 = strstr(of, "/install/"); if (b2) b2[9] = (char)('0' + n_orders % 10);
            snprintf(poll_answer, sizeof poll_answer, "{\"session\": \"sent\",%s%s", strstr(body, "\"to\":\"cart\"") ? "" : strstr(body, "\"to\":\"sd\"") ? " \"kind\": \"play\", \"sd\": \"keep\", \"file\": \"FARM.gbc\"," :
                     strstr(body, "\"to\":\"now\"") ? " \"kind\": \"play\", \"lib\": \"none\"," : " \"kind\": \"play\", \"lib\": \"keep\",", of);
        }
        return 1;
    }
    snprintf(out, cap, "{}");
    return 1;
}
static int cur_code(const char *path) { (void)path; return 200; }
static int last_kind;
static int a_result(void *u)
{
    (void)u;
    if (pc > 0 && --pc) return 0;
    if (C.side_st == 2) return !strcmp(C.side_path, "/link_device/catalog/order") ? order_code : page_code;
    return cur_code(NULL);
}
static const char *base(void *u) { (void)u; return "https://api.shinyrelay.com/gbnet/v1"; }
static int one(void *u) { (void)u; return 1; }
static void none(void *u) { (void)u; }
static int f_wifi(void *u) { (void)u; return wifi; }
static void hold(void *u, int on) { (void)u; (void)on; }
static int peek_s(void *u, const cf_offer_t *o) { (void)u; (void)o; return 1; }
static int peek_r(void *u, char *t, char *n) { (void)u; snprintf(t, 17, "SHINY KART COLOR"); n[0] = 0; return 0; }
static int arm(void *u, const cf_offer_t *o) { (void)u; last_kind = o->lib; return arm_ret; }
static int jobr(void *u) { (void)u; return job; }
static int pending(void *u, char *id, char *t) { (void)u; (void)id; (void)t; return 0; }
static void logm(void *u, const char *m) { (void)u; if (getenv("V")) fprintf(stderr, "  %s\n", m); }
static int busy(void *u) { (void)u; return 0; }
static void openp(void *u) { (void)u; }
static const cf_inst_host_t H = { NULL, a_start, a_result, base, one, none, one, f_wifi, hold, one, peek_s, peek_r, one, openp, arm, jobr, pending, none, logm, busy, NULL, NULL, NULL };

static int pct = -1;
void NW_CartView(NwCartView_t *out)
{
    cf_view_t v; cf_install_view(&C, &v);
    memset(out, 0, sizeof *out);
    out->Kind = v.kind; snprintf(out->Title, sizeof out->Title, "%s", v.title); snprintf(out->Note, sizeof out->Note, "%s", v.note);
    snprintf(out->Code, sizeof out->Code, "%s", v.code); out->Mins = v.mins; out->Anyway = v.anyway;
    if (v.kind == CV_INSTALLING) snprintf(out->Note, sizeof out->Note, pct >= 0 ? "%d%%" : "STARTING", pct);
}
void NW_CartButton(int a) { cf_install_button(&C, a); }
void NW_GgStart(void) { cf_install_catalog(&C); }
int  NW_GgSide(const char *Path, const char *Body, char *Out, int Cap) { return cf_install_side(&C, Path, Body, Out, Cap); }
int  NW_GgSideDone(int *Code) { return cf_install_side_done(&C, Code); }
int  NW_GgReady(void) { return cf_install_has_code(&C); }
int  NW_GgActive(void) { return C.cat; }

static lv_obj_t screen;
static const char *Tabs = "NETWORK,GAMES,*GET  GAMES,DOWNLOAD,UPDATE";
static void run(uint32_t ms) { for (uint32_t e = 0; e < ms; e += 100) { T += 100; cf_install_tick(&C, T); (void)GetGames_Draw(&screen); } }
static void press(Button_t b) { if (Cap) (void)Cap(b, kButtonState_Pressed, NULL); else (void)GetGames_OnButton(b, kButtonState_Pressed, NULL); run(300); }
static void row(int r, char *out, size_t n)
{
    const lv_obj_t *o = NULL;
    for (int i = 0; i < nobj; i++) if (objs[i]->y == 46 + 9 * r && objs[i]->x == 77) o = objs[i];
    snprintf(out, n, "%s%s%s", o && o->style == &W ? "!" : "", o && o->w ? (o->mode == LV_LABEL_LONG_DOT ? "~." : "~") : "", o ? o->text : "");
}
static int has(const char *t) { char b[128]; for (int r = 0; r < 7; r++) { row(r, b, sizeof b); if (strstr(b, t)) return 1; } return 0; }
static void shot(const char *name)
{
    (void)GetGames_Draw(&screen);
    printf("%s|%s", name, Tabs);
    char b[128];
    for (int r = 0; r < 7; r++) { row(r, b, sizeof b); printf("|%s", b); }
    printf("\n");
}

int main(void)
{
    T = 1000;
    snprintf(poll_answer, sizeof poll_answer, "{\"session\": \"waiting\", \"expires_in\": 590}");
    snprintf(order_answer, sizeof order_answer, "{\"ok\":true,\"title\":\"SHINY KART COLOR\"}");
    cf_install_init(&C, &H, T);
    run(300); shot("01_main");
    CHECK(has("GET  GAMES") && has("A:  LOOK") && has("YOUR  OWN  GAMES"), "main");
    press(kButton_A); shot("02_which");
    CHECK(has(">FREE  GAMES") && has("MY  GAMES") && has("A:  OPEN"), "which list: FREE GAMES first");
    press(kButton_A); shot("03_getting_ready");
    run(2500); shot("04_list");
    CHECK(has("FREE  GAMES  1/8") && has(">SHINY  KART  COLOR") && has("A:  GET  256  KB"), "the first page, the first game, its size");
    CHECK(n_page_calls == 1 && !strstr(C.code, "?"), "one page asked");
    press(kButton_Down); press(kButton_Down); shot("05_list_third");
    CHECK(has(">SHINY  MONSTERS  COLOR") && has("FREE  GAMES  3/8") && has("512  KB"), "down twice");
    press(kButton_Down); press(kButton_Down); press(kButton_Down); run(1500); shot("06_list_page2");
    CHECK(n_page_calls == 2 && has("FREE  GAMES  6/8") && has(">SHINY  FIGHTER  COLOR"), "past the page's end: the next page");
    press(kButton_Up); run(1500);
    CHECK(has("FREE  GAMES  5/8") && has(">SHINY  COLLECTION"), "up from the page's top: the page before, its last game");
    press(kButton_Up); press(kButton_Up); press(kButton_Up); press(kButton_Up);
    CHECK(has(">SHINY  KART  COLOR"), "back to the first");
    /* the four places; a card in and ready: CARD first */
    press(kButton_A); shot("07_where_card_first");
    CHECK(has("WHERE  DOES  IT  GO?") && has(">CARD") && has("ON  CHROMATIC") && has("PLAY  NOW") && has("CARTRIDGE"), "where: the four places, CARD first with a card in");
    /* lane m110u: the order is SHARE's and the site's (ON CHROMATIC, CARD, PLAY NOW, CARTRIDGE), the place picked said in plain words */
    { char r1[64], r2[64]; row(1, r1, sizeof r1); row(2, r2, sizeof r2);
      CHECK(strstr(r1, "STAYS  ON  THE  CARD") && strstr(r2, "ON  CHROMATIC"), "the card's hint under the question, ON CHROMATIC listed first: |%s|%s|", r1, r2); }
    press(kButton_Up); shot("08_where_on_chromatic");
    CHECK(has(">ON  CHROMATIC") && has("STAYS  IN  CHROMATIC"), "up: ON CHROMATIC, its hint");
    press(kButton_A); shot("09_asking");
    run(800);
    CHECK(n_orders == 1 && strstr(last_order, "\"id\":\"kart\"") && strstr(last_order, "\"to\":\"lib\"") && strstr(last_order, "\"play\":false") && strstr(last_order, "\"code\":\"KX7M2Q\"") && !strstr(last_order, "mine"),
          "the order: kart, ON CHROMATIC, kept, this unit's code: %s", last_order);
    run(6000); pct = 42; shot("10_getting_42");
    CHECK(has("GETTING  IT") && has("42%"), "the download's percent");
    job = CF_D_OK; run(20000); shot("11_result_kept");
    CHECK(has("ON  CHROMATIC") && has("PLAY  IT  FROM") && has("SYSTEM  >  GAMES") && has("A:  OK"), "kept: its result, how to play it");
    press(kButton_A); run(2500); shot("12_list_again");
    CHECK(has("FREE  GAMES"), "A on the result: the list again");
    /* PLAY NOW */
    job = -1; pct = -1;
    press(kButton_Down); press(kButton_A); press(kButton_Down); shot("13_where_play_now");
    CHECK(has(">PLAY  NOW") && has("PLAYS  NOW,  NOT  KEPT"), "down from CARD: PLAY NOW, its hint");
    press(kButton_A); run(1200);
    CHECK(strstr(last_order, "\"id\":\"farm\"") && strstr(last_order, "\"to\":\"now\""), "the order: farm, play now: %s", last_order);
    job = CF_D_OK; run(20000); shot("14_result_play_now");
    CHECK(has("PLAYING  NOW") && has("NOT  KEPT") && !has("CART"), "played, nothing kept: its result says so");
    press(kButton_B); shot("15_after_b");
    CHECK(has("A:  LOOK"), "B: back");
    /* no card in: ON CHROMATIC first, the card greyed with why */
    card = kSdSt_None; job = -1;
    press(kButton_A); press(kButton_A); run(2500); press(kButton_A); shot("16_where_no_card");
    CHECK(!has("CARD:  NO  CARD  IN") && has(">ON  CHROMATIC") && has("    CARD"), "no card: greyed by its name only; ON CHROMATIC first");
    press(kButton_Down); shot("16b_where_on_greyed_card");
    CHECK(has(">CARD") && has("CARD:  NO  CARD  IN"), "1.1.3: down: onto the greyed card, its reason in full under the question");
    { const int n0 = n_orders; press(kButton_A); CHECK(n_orders == n0 && has("WHERE  DOES  IT  GO?"), "A on a greyed place: nothing"); }
    press(kButton_Down);
    CHECK(has(">PLAY  NOW"), "down: PLAY NOW");
    press(kButton_Down);
    CHECK(has(">CARTRIDGE") && has("REPLACES  CART  GAME"), "down: the cartridge, its hint");
    /* the cartridge with no card: NO BACKUP: WRITE ANYWAY? (B = no) */
    press(kButton_A); shot("17_cart_no_backup");
    CHECK(has("NO  BACKUP:") && has("WRITE  ANYWAY?") && has("B:  NO"), "no card: the no-backup question");
    press(kButton_B);
    CHECK(has("WHERE  DOES  IT  GO?") && n_orders == 2, "B = no: nothing ordered");
    press(kButton_A); press(kButton_A); run(1200);
    CHECK(n_orders == 3 && strstr(last_order, "\"to\":\"cart\""), "A: write anyway: the order to the cartridge: %s", last_order);
    job = CF_D_OK; run(20000); shot("18_cart_written");
    CHECK(has("INSTALLED") && has("ON  THE  CARTRIDGE"), "written to the cartridge: where it is");
    press(kButton_B);
    /* no library, no cartridge */
    lib_n = -1; card = kSdSt_Ready; cart_in = 0; job = -1;
    press(kButton_A); press(kButton_A); run(2500); press(kButton_A); shot("19_where_no_lib_no_cart");
    CHECK(has(">CARD") && !has("CANNOT  KEEP  GAMES"), "no library, no cartridge: the card picked, the greyed ones by name");
    press(kButton_Up);
    CHECK(has(">ON  CHROMATIC") && has("ON  CHROMATIC:  CANNOT  KEEP  GAMES"), "up onto the greyed ON CHROMATIC: why");
    press(kButton_Up);
    CHECK(has(">CARTRIDGE") && has("CARTRIDGE:  NO  CARTRIDGE  IN"), "up again: the greyed cartridge: why");
    press(kButton_B); press(kButton_B); press(kButton_B);
    lib_n = 3; cart_in = 1;
    /* MY GAMES: not paired */
    paired = 0;
    press(kButton_A); press(kButton_Down); shot("20_which_unpaired");
    CHECK(has(">MY  GAMES") && has("PAIR  FIRST  IN") && has("SYSTEM  >  ONLINE"), "not paired: why, and where");
    press(kButton_A); shot("21_pair_first");
    CHECK(has("PAIR  IT  WITH  YOUR") && has("SYSTEM  >  ONLINE"), "how to pair");
    press(kButton_B); press(kButton_B);
    paired = 1;
    /* MY GAMES: an account with none */
    mine_empty = 1;
    press(kButton_A); press(kButton_Down); press(kButton_A); run(2500); shot("22_my_games_none");
    CHECK(has("NO  GAMES  YET") && has("ON  SHINYRELAY.COM"), "no games yet: where to put them");
    press(kButton_B); press(kButton_B);
    mine_empty = 0;
    /* MY GAMES: the list, a game with saves to the CARD with one of them */
    press(kButton_A); press(kButton_Down); press(kButton_A); run(2500); shot("23_my_games");
    CHECK(has("MY  GAMES  1/3") && has(">POKEMON  CRYSTAL") && has("A:  GET  2  MB:  2  SAVES"), "MY GAMES: the list, size and saves");
    press(kButton_A); shot("24_my_where");
    CHECK(has(">CARD") && !has("TOO  BIG"), "a 2 MB game: the card picked");
    press(kButton_Down); press(kButton_Down);
    CHECK(has(">CARTRIDGE") && has("CARTRIDGE:  TOO  BIG:  1.94  MB  AT  MOST"), "a 2 MB game: the cartridge greyed, why in full");
    press(kButton_Up); press(kButton_Up);
    CHECK(has(">CARD"), "back to the card");
    press(kButton_A); run(1500); shot("25_which_save");
    CHECK(n_save_calls == 1 && has("WHICH  SAVE?") && has(">NO  SAVE") && has("JOHTO  DONE") && has("BEFORE  THE  BOSS"), "its saves, NO SAVE first");
    press(kButton_Down); shot("26_save_picked");
    CHECK(has(">JOHTO  DONE"), "a save picked");
    press(kButton_A); run(1200);
    CHECK(strstr(last_order, "\"id\":\"0123456789ab\"") && strstr(last_order, "\"to\":\"sd\"") && strstr(last_order, "\"mine\":true") && strstr(last_order, "\"save\":\"0a0b0c0d\""),
          "the order: my game, the card, its save: %s", last_order);
    job = CF_D_OK; run(20000); shot("27_my_result_card");
    press(kButton_A); run(2500);
    CHECK(has("MY  GAMES"), "A on the result: MY GAMES again");
    /* a 6 MB game: only the CARD */
    job = -1;
    press(kButton_Down); press(kButton_A); shot("28_big_where");
    CHECK(has(">CARD"), "4-8 MB: the card");
    press(kButton_Up);
    CHECK(has("ON  CHROMATIC:  TOO  BIG:  4  MB  AT  MOST"), "ON CHROMATIC greyed: why");
    press(kButton_Down); press(kButton_Down);
    CHECK(has("PLAY  NOW:  TOO  BIG:  4  MB  AT  MOST"), "PLAY NOW greyed: why");
    { const int n0 = n_orders; press(kButton_A); CHECK(n_orders == n0, "A on a greyed place: nothing"); }
    press(kButton_Down);
    CHECK(has("CARTRIDGE:  TOO  BIG"), "the cartridge greyed: why");
    press(kButton_Up); press(kButton_Up);
    CHECK(has(">CARD"), "back to the card");
    press(kButton_B);
    /* the cartridge with a card in: backed up first, then written */
    press(kButton_Down); press(kButton_A); press(kButton_Down); press(kButton_Down); shot("29_my_where_cart");
    CHECK(has(">CARTRIDGE"), "down twice from CARD: the cartridge");
    press(kButton_A); shot("30_cart_backup_first");
    CHECK(has("WRITE  IT  TO  THE") && has("IS  BACKED  UP  FIRST."), "the backup first");
    const int o0 = n_orders;
    press(kButton_A); run(3000); shot("31_backing_up");
    CHECK(n_jobs == 1 && has("BACKING  UP") && n_orders == o0, "the cartridge backed up before anything is ordered");
    run(20000); run(2500); shot("32_after_backup");
    CHECK(n_orders == o0 + 1 && strstr(last_order, "\"to\":\"cart\"") && strstr(last_order, "\"id\":\"0000cafe0000\""), "backed up: then the order: %s", last_order);
    job = CF_D_OK; run(20000); shot("33_cart_written_mine");
    CHECK(has("INSTALLED"), "written");
    press(kButton_B);
    /* a backup that fails: nothing written */
    job = -1; job_ok = 0;
    press(kButton_A); press(kButton_Down); press(kButton_A); run(2500); press(kButton_Down); press(kButton_Down); press(kButton_A); press(kButton_Down); press(kButton_Down); press(kButton_A);
    const int o1 = n_orders;
    press(kButton_A); run(30000); shot("34_backup_failed");
    CHECK(has("NOT  BACKED  UP") && n_orders == o1, "the backup failed: nothing ordered, why");
    press(kButton_B);
    job_ok = 1;
    /* the server says no: its words */
    press(kButton_A); press(kButton_A); run(2500);
    order_code = 507; snprintf(order_answer, sizeof order_answer, "{\"ok\":false,\"error\":\"no room on the unit\",\"why\":\"NOT  ENOUGH  ROOM|ON  CHROMATIC\"}");
    press(kButton_A); press(kButton_Up); press(kButton_A); run(1500); shot("35_fail_no_room");
    CHECK(has("NOT  ENOUGH  ROOM") && has("ON  CHROMATIC") && has("A:  TRY  AGAIN"), "the server's why, as it said it");
    press(kButton_B); shot("36_back_main");
    CHECK(has("A:  LOOK") && n_cancels >= 1, "B: back, the code given back");
    /* lane m110x (m110g1 finding 1): A on a CARD result while its report still goes out. The next GET's new receive
       session saw the finished job's result, went to the install view and sat on GETTING IT / DO NOT TURN OFF with
       B doing nothing. Now: the list again, and nothing ordered */
    order_code = 200; ack_slow = 40; job = -1; pct = -1;
    press(kButton_A); press(kButton_A); run(2500); press(kButton_Down); press(kButton_A); shot("38_where_farm");
    CHECK(has(">CARD") && has("WHERE  DOES  IT  GO?"), "farm: the card first");
    const int o2 = n_orders;
    press(kButton_A); run(1200);
    CHECK(n_orders == o2 + 1 && strstr(last_order, "\"id\":\"farm\"") && strstr(last_order, "\"to\":\"sd\""), "the order: farm to the card: %s", last_order);
    job = CF_D_OK;
    for (int i = 0; i < 300 && !has("ON  THE  CARD"); i++) run(100);
    shot("39_result_card_reporting");
    CHECK(has("ON  THE  CARD") && has("A:  OK") && C.st == CI_REPORT, "the card's result while its report goes out (st %d)", C.st);
    job = -1;
    press(kButton_A); run(6000); shot("40_next_get_after_card");
    CHECK(has("FREE  GAMES") && !has("GETTING  IT") && !has("DO  NOT  TURN  OFF"), "A on the card's result: the list again, not the last game's GETTING IT");
    CHECK(n_orders == o2 + 1, "nothing ordered by A");
    press(kButton_B); press(kButton_B); shot("41_b_after_next_get");
    CHECK(has("A:  LOOK"), "B leaves");
    ack_slow = 0;
    /* an order taken whose game does not come yet: GETTING IT, and B leaves (nothing is on its way to the unit) */
    no_offer = 1;
    press(kButton_A); press(kButton_A); run(2500); press(kButton_A); press(kButton_Down); press(kButton_A); run(1500); shot("42_ordered_waiting");
    CHECK(has("GETTING  IT") && has("DO  NOT  TURN  OFF"), "ordered, its game not here yet: GETTING IT");
    const int c0 = n_cancels;
    press(kButton_B); run(1500); shot("43_b_while_waiting");
    CHECK(has("A:  LOOK") && n_cancels == c0 + 1, "B while the game has not come: back, the code given back (%d cancels)", n_cancels - c0);
    no_offer = 0;
    /* no Wi-Fi */
    order_code = 200; wifi = 0;
    press(kButton_A); run(1500); shot("37_no_wifi");
    CHECK(has("NO  WI-FI") && has("SET  UP  WI-FI  FIRST"), "no Wi-Fi: why");
    fprintf(stderr, "gg_shots: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
