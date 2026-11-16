/* share_shots.c (lane share1): the SHARE / RECEIVE FROM A CHROMATIC pages as the unit draws them, on the Mac: the real rows
 * (share.c's Share_RowsFor) of every view the share session shows, with the longest names and numbers they can take. Each
 * screen's seven rows are printed as osd_phone.py arguments ("!" = white); make share-shots renders them and share_fit.py
 * checks every row fits the panel (x <= 148, at most 23 characters). */
#include <stdio.h>
#include <string.h>
#include "osd_shared.h"
#include "../share.h"
#include "cf_share.h"
#include "cf_share_dev.h"

void Share_RowsFor(const cf_share_view_t *v, ShareRows_t *o);
/* (the session is not here: these are never called by Share_RowsFor) */
int  CfShare_Active(void) { return 0; }
void CfShare_View(cf_share_view_t *v) { memset(v, 0, sizeof *v); }
void CfShare_Button(int b) { (void)b; }
int  CfShare_SendStart(int s, uint32_t u, const char *p, const char *t) { (void)s; (void)u; (void)p; (void)t; return -1; }
int  CfShare_RecvStart(void) { return -1; }
void CfShare_Close(void) {}
uint32_t ShareHost_NowMs(void) { return 0; }
bool ShareHost_Visible(void) { return true; }
void MenuMgr_SetCapture(fnOSDButtonCb_t f) { (void)f; }

static void show(const char *name, const cf_share_view_t *v)
{
    ShareRows_t r;
    Share_RowsFor(v, &r);
    printf("%s", name);
    for (int i = 0; i < kShareRows; i++) printf("|%s%s", (r.White >> i) & 1 ? "!" : "", r.T[i]);
    printf("\n");
}
int main(void)
{
    cf_share_view_t v;
    #define V0() memset(&v, 0, sizeof v); snprintf(v.title, sizeof v.title, "PM_CRYSTAL"); snprintf(v.peer, sizeof v.peer, "CHROMATIC  3C4D"); snprintf(v.code, sizeof v.code, "4729"); v.read_pct = -1
    V0(); v.kind = CF_SHV_A_CODE; v.cur = 1; v.save_avail = 1; show("a1_code", &v);
    V0(); v.kind = CF_SHV_A_CODE; v.cur = 4; v.save_avail = 1; v.with_save = 1; show("a1_code_save", &v);
    V0(); v.kind = CF_SHV_A_CODE; v.cur = 0; v.read_pct = 100; snprintf(v.title, sizeof v.title, "THE  CARTRIDGE"); show("a1_code_cart_reading", &v);
    V0(); v.kind = CF_SHV_A_CODE; v.cur = 3; snprintf(v.code, sizeof v.code, "8888"); show("a1_code_wide", &v);
    V0(); v.kind = CF_SHV_A_READ; v.read_pct = 47; show("a2_read", &v);
    V0(); v.kind = CF_SHV_A_FIND; show("a3_find", &v);
    V0(); v.kind = CF_SHV_A_ASKED; show("a4_asked", &v);
    V0(); v.kind = CF_SHV_A_SEND; v.pct = 100; v.kbps = 888; show("a5_send", &v);
    V0(); v.kind = CF_SHV_A_SEND; v.pct = 63; v.paused = 59; show("a5_send_paused", &v);
    V0(); v.kind = CF_SHV_A_END; show("a6_end", &v);
    {
        static const struct { int e; const char *n; } kE[] = { { CF_SH_OK, "ok" }, { CF_SH_E_NOTFOUND, "notfound" }, { CF_SH_E_DECLINED, "declined" },
            { CF_SH_E_BUSY, "busy" }, { CF_SH_E_LOST, "lost" }, { CF_SH_E_STOPPED, "stopped" }, { CF_SH_E_READ, "read" }, { CF_SH_E_ROOM, "room" },
            { CF_SH_E_WRITE, "write" }, { CF_SH_E_CHECK, "check" }, { CF_SH_E_NOANSWER, "noanswer" }, { CF_SH_E_RADIO, "radio" } };
        (void)kE;
    }
    /* the result rows come from cf_share_dev.c (literal pairs); every pair is listed here as the unit says them */
    static const char *const kRes[][3] = {
        { "a7_sent_lib", "IT  IS  ON  THAT", "CHROMATIC  NOW" }, { "a7_sent_card", "IT  IS  ON  THAT", "CHROMATIC'S  CARD" }, { "a7_sent_play", "IT  PLAYS  ON  THAT", "CHROMATIC  NOW" },
        { "a7_notfound", "NO  CHROMATIC  WITH", "THAT  CODE  NEARBY" }, { "a7_declined", "THE  OTHER  ONE", "SAID  NO" }, { "a7_busy", "THE  OTHER  ONE", "IS  BUSY" },
        { "a7_lost", "THE  OTHER  ONE", "WENT  OUT  OF  REACH" }, { "a7_stopped", "THE  OTHER  ONE", "STOPPED" }, { "a7_read", "THE  GAME  COULD", "NOT  BE  READ" },
        { "a7_room", "THE  OTHER  ONE", "HAS  NO  ROOM" }, { "a7_write", "THE  OTHER  ONE", "COULD  NOT  KEEP  IT" }, { "a7_check", "IT  DID  NOT  ARRIVE", "WHOLE:  TRY  AGAIN" },
        { "a7_noanswer", "NO  ANSWER  FROM", "THE  OTHER  ONE" }, { "a7_radio", "THE  RADIO  DID  NOT", "START:  TRY  AGAIN" }, { "a7_nocart", "NO  CARTRIDGE  IN", "THE  SLOT" },
        { "a7_other", "IT  DID  NOT  WORK", "TRY  AGAIN" }, { "a7_nostart", "IT  DID  NOT  START", "ON  THAT  CHROMATIC" },   /* m110s: PLAY NOW on B did not start */
        { "a7_nomem", "NOT  ENOUGH  MEMORY", "TURN  IT  OFF  AND  ON" } };                                             /* m110s: the share's task could not start */
    for (unsigned i = 0; i < sizeof kRes / sizeof kRes[0]; i++) {
        V0(); v.kind = CF_SHV_A_RESULT; v.ok = i < 3; snprintf(v.r1, sizeof v.r1, "%s", kRes[i][1]); snprintf(v.r2, sizeof v.r2, "%s", kRes[i][2]); show(kRes[i][0], &v);
    }
    V0(); v.kind = CF_SHV_B_START; show("b1_start", &v);
    V0(); v.kind = CF_SHV_B_CODE; show("b2_code", &v);
    V0(); v.kind = CF_SHV_B_OFFER; v.rom_size = 2097152; v.save_avail = 1; v.place_ok[0] = v.place_ok[1] = v.place_ok[2] = 1; v.sel = 0; show("b3_offer", &v);
    V0(); v.kind = CF_SHV_B_OFFER; v.rom_size = 262144; v.place_ok[0] = 0; v.place_why[0] = "FULL"; v.place_ok[1] = 0; v.place_why[1] = "NONE"; v.place_ok[2] = 1; v.sel = 2; show("b3_offer_greyed", &v);
    {
        static const char *const kW[] = { "", "WAIT", "FULL", "KEPT", "NONE", "LOCKED", "TOO  BIG" };
        for (unsigned i = 0; i < sizeof kW / sizeof kW[0]; i++) {
            char n[32]; snprintf(n, sizeof n, "b3_why%u", i);
            V0(); v.kind = CF_SHV_B_OFFER; v.rom_size = 4194304; v.save_avail = 1;
            for (int d = 0; d < 3; d++) { v.place_ok[d] = 0; v.place_why[d] = (d == 0 && i > 3) ? "FULL" : kW[i]; }   /* (ON CHROMATIC says only FULL / KEPT / WAIT / nothing) */
            show(n, &v);
        }
    }
    V0(); v.kind = CF_SHV_B_RECV; v.pct = 41; v.kbps = 123; v.dest = CF_SH_DEST_LIB; show("b4_recv_lib", &v);
    V0(); v.kind = CF_SHV_B_RECV; v.pct = 41; v.kbps = 123; v.dest = CF_SH_DEST_CARD; show("b4_recv_card", &v);
    V0(); v.kind = CF_SHV_B_RECV; v.pct = 41; v.kbps = 123; v.dest = CF_SH_DEST_PLAY; show("b4_recv_play", &v);
    V0(); v.kind = CF_SHV_B_RECV; v.pct = 41; v.paused = 12; show("b4_recv_paused", &v);
    V0(); v.kind = CF_SHV_B_KEEP; show("b5_keep", &v);
    V0(); v.kind = CF_SHV_B_AFTER; v.pct = 88; v.dest = CF_SH_DEST_CARD; show("b6_after_card", &v);
    V0(); v.kind = CF_SHV_B_AFTER; v.pct = 88; v.dest = CF_SH_DEST_PLAY; show("b6_after_play", &v);
    static const char *const kBRes[][3] = {
        { "b7_lib", "ON  CHROMATIC", "WITH  ITS  SAVE" }, { "b7_lib2", "ON  CHROMATIC", "PLAY  IT  FROM  GAMES" }, { "b7_card", "ON  THE  CARD", "WITH  ITS  SAVE" },
        { "b7_play", "PLAYING  NOW", "NOT  KEPT" }, { "b7_declined", "YOU  SAID  NO", "" }, { "b7_room", "NOT  ENOUGH  ROOM", "" }, { "b7_write", "IT  COULD  NOT", "BE  KEPT" },
        { "b7_cardfail", "IT  COULD  NOT", "BE  PUT  ON  THE  CARD" }, { "b7_nostart", "IT  DID  NOT  START", "UPDATE  FIRST" }, { "b7_lost", "THE  OTHER  ONE", "WENT  OUT  OF  REACH" },
        { "b7_nostart2", "IT  DID  NOT  START", "TRY  AGAIN" }, { "b7_nomem", "NOT  ENOUGH  MEMORY", "TURN  IT  OFF  AND  ON" } };   /* m110s */
    for (unsigned i = 0; i < sizeof kBRes / sizeof kBRes[0]; i++) {
        V0(); v.kind = CF_SHV_B_RESULT; v.ok = i < 4; snprintf(v.r1, sizeof v.r1, "%s", kBRes[i][1]); snprintf(v.r2, sizeof v.r2, "%s", kBRes[i][2]); show(kBRes[i][0], &v);
    }
    return 0;
}
