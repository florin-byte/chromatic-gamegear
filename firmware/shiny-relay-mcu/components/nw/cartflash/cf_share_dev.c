/* cf_share_dev.c: Chromatic to Chromatic sharing on the unit (lane share1). See cf_share_dev.h, cf_share.h.
 *
 * A (sends):   SHARE on a game -> the code page (B's code typed, WITH SAVE when the game has a save). A cartridge is read
 *              into the staging area meanwhile (the card backup's read: nothing is written to the cart); an ON CHROMATIC
 *              game is read from the game store as it goes (FREAD, each KiB checked by the FPGA); a CARD game from its file.
 *              A: SEND -> the radio (the link session paused) -> B found by its code -> B's person chooses -> the game, then
 *              the save -> B's verdict. The game travels as A keeps it: an SGZ1 container stays packed.
 * B (receives): SYSTEM > DOWNLOAD > B: FROM A CHROMATIC -> a 4-digit code -> an offer -> ON CHROMATIC / CARD / PLAY NOW:
 *   ON CHROMATIC  the store's stream (cf_lib_tx: sectors erased and written as the bytes come, each extent read back, the index
 *                 last), the save into the save RAM first and then into the store's journal
 *   CARD          a raw game straight into /GAMES/<name>.new (renamed when it checked), its save into /SAVES/<name>.sav;
 *                 a packed one into PSRAM scratch, unpacked onto the card after the radio is off (the unpack's heap)
 *   PLAY NOW      PSRAM: the ROM area (raw) or scratch (packed, unpacked at the play), the save in the save RAM
 * Nothing counts before B has every byte and the crc32 + sha256 of the game (and the save's crc32) are A's. */
#include "cf_share_dev.h"
#include "cf_share.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cf_esp.h"
#include "cf_lib.h"
#include "cf_rc_dev.h"
#include "cf_sgz.h"
#include "cwl.h"
#include "cwl_online.h"
#include "cwl_share.h"
#include "nw_dev.h"
#include "sd.h"

#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

int  CfDev_ShareCartBegin(void (*pct)(int), uint32_t *rom_len, uint32_t *sav_len, char title[17], uint8_t *cart_type, uint8_t *cart_ram, uint32_t *rom_crc, int *cgb);
int  CfDev_ShareCartRead(int sav, uint32_t off, uint8_t *b, uint32_t n);
void CfDev_ShareCartEnd(void);
int  CfDev_ShareOthersBusy(void);
void CfRc_View(NwPlayView_t *Out);    /* cf_dev.c: an install, receive mode, a cart job, an update: not now */

#define SH_ACC 2048u                 /* B: bytes gathered before a write to the store / the card (1 KiB measured: the store writes at half speed) */
#define SH_TASK_STACK 5120
/* m110s: the share's own task needs SH_TASK_STACK in one piece. After a few jobs in one boot the heap is in pieces (unit 1,
   m110f: 15,764 free, the largest 4,096), the task did not start and A on SHARE did nothing. Then a small task starts it:
   the link's TLS clients and its session let go (the Wi-Fi buffers back, as the radio's start does anyway), and the task
   asked for again while the heap comes back together */
#define SH_GO_STACK 2304
#define SH_GO_TRIES 120                  /* 100 ms apart */

typedef struct {
    int role;                        /* 1 A, 2 B */
    cf_share_view_t v;               /* what the screen shows (under _Mux) */
    volatile int go, stop, answer, closed, task_done;
    /* A */
    int src;
    uint32_t uid;
    char path[SDC_PATH_MAX];
    int prepared, prep_err;
    cf_lib_ent_t ent;
    cf_lib_save_t sv;
    FIL *fg, *fs;
    cf_sh_offer_t offer;
    cf_sh_tx_t tx;
    uint8_t *seg[CF_SH_NSEG];        /* the ring in 1 KiB pieces */
    /* B */
    cf_sh_rx_t rx;
    cf_sh_offer_t in;
    uint16_t peer_unit;
    cf_lib_tx_t *ltx;
    uint8_t *acc;
    uint32_t acc_off, acc_n, acc_cap;
    sd_wr_t *wg, *ws;
    char gpath[SDC_PATH_MAX], spath[SDC_PATH_MAX + 8];
    int ram_on, ram_save;            /* the PSRAM writer on; it is in the save RAM now */
    uint32_t ram_region_n;           /* bytes into the region being written */
    int fin, fin_uid_ok, lib_held;
    uint32_t lib_uid;
    int play_res;                    /* m110s: PLAY NOW: 1 playing, -1 did not start, -2 this FPGA cannot (0: not yet known) */
    uint32_t play_seq;               /* ... CfRc_PlayDone() before it was asked */
    int link_held;                   /* m110s: the starter held the link off (let go at the end if the radio did not) */
    int lib_let_go;                  /* m110s: A let the page's hold of the library go for the send (taken again at the end) */
    /* both */
    void *sha;
    uint32_t t_start, t_radio, heap_low, heap_start;
    char why[48];
} sh_t;

static sh_t *_S;
static portMUX_TYPE _Mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int _TaskOn;
/* m110s: a share's result, shown until A: OK, is a copy of its view: the session (sh_t) is freed when its task ends, so a
   result nobody dismissed (a PLAY NOW: the game starts at once) no longer keeps its 2 KB in the middle of the heap */
static cf_share_view_t _Done;
static volatile int _DoneOn;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void heap_note(sh_t *S) { const uint32_t f = (uint32_t)esp_get_free_heap_size(); if (!S->heap_low || f < S->heap_low) S->heap_low = f; }
static void set_view(sh_t *S, int kind) { portENTER_CRITICAL(&_Mux); S->v.kind = kind; portEXIT_CRITICAL(&_Mux); }
static void result(sh_t *S, int ok, const char *r1, const char *r2)
{
    portENTER_CRITICAL(&_Mux);
    S->v.ok = ok;
    snprintf(S->v.r1, sizeof S->v.r1, "%s", r1);
    snprintf(S->v.r2, sizeof S->v.r2, "%s", r2);
    S->v.kind = S->role == 1 ? CF_SHV_A_RESULT : CF_SHV_B_RESULT;
    portEXIT_CRITICAL(&_Mux);
    printf("share: %s: %s %s\n", ok ? "done" : "not done", r1, r2);
}
/* the two rows of why, as the screens say them */
static void why_rows(int e, int role, const char **r1, const char **r2)
{
    switch (e) {
    case CF_SH_E_NOTFOUND: *r1 = "NO  CHROMATIC  WITH"; *r2 = "THAT  CODE  NEARBY"; break;
    case CF_SH_E_DECLINED: *r1 = role == 1 ? "THE  OTHER  ONE" : "YOU  SAID  NO"; *r2 = role == 1 ? "SAID  NO" : ""; break;
    case CF_SH_E_BUSY: *r1 = "THE  OTHER  ONE"; *r2 = "IS  BUSY"; break;
    case CF_SH_E_LOST: *r1 = "THE  OTHER  ONE"; *r2 = "WENT  OUT  OF  REACH"; break;
    case CF_SH_E_STOPPED: case CF_SH_E_UNKNOWN: *r1 = "THE  OTHER  ONE"; *r2 = "STOPPED"; break;
    case CF_SH_E_CANCELLED: *r1 = "STOPPED"; *r2 = ""; break;
    case CF_SH_E_READ: *r1 = "THE  GAME  COULD"; *r2 = "NOT  BE  READ"; break;
    case CF_SH_E_ROOM: *r1 = role == 1 ? "THE  OTHER  ONE" : "NOT  ENOUGH  ROOM"; *r2 = role == 1 ? "HAS  NO  ROOM" : ""; break;
    case CF_SH_E_WRITE: *r1 = role == 1 ? "THE  OTHER  ONE" : "IT  COULD  NOT"; *r2 = role == 1 ? "COULD  NOT  KEEP  IT" : "BE  KEPT"; break;
    case CF_SH_E_CHECK: *r1 = "IT  DID  NOT  ARRIVE"; *r2 = "WHOLE:  TRY  AGAIN"; break;
    case CF_SH_E_NOANSWER: *r1 = "NO  ANSWER  FROM"; *r2 = "THE  OTHER  ONE"; break;
    case CF_SH_E_RADIO: *r1 = "THE  RADIO  DID  NOT"; *r2 = "START:  TRY  AGAIN"; break;
    case CF_SH_E_START: *r1 = "IT  DID  NOT  START"; *r2 = role == 1 ? "ON  THAT  CHROMATIC" : "TRY  AGAIN"; break;
    default: *r1 = "IT  DID  NOT  WORK"; *r2 = "TRY  AGAIN"; break;
    }
}

/* ---------------------------------------------------------------- the radio, as the protocol sees it */
static int r_send(void *u, const uint8_t dst[6], const uint8_t *f, uint32_t n) { (void)u; return CwlSh_Send(dst, f, n); }
static void r_chan(void *u, uint8_t ch) { (void)u; CwlSh_SetChannel(ch); }
static void r_log(void *u, const char *m) { (void)u; printf("%s\n", m); }

/* ================================================================ A: where the game comes from */
static void cart_pct(int p) { sh_t *S = _S; if (S) { portENTER_CRITICAL(&_Mux); S->v.read_pct = p; portEXIT_CRITICAL(&_Mux); } }
static int src_read(void *u, uint32_t off, uint8_t *b, uint32_t n)
{
    sh_t *S = u;
    const uint32_t gs = S->offer.game_size;
    while (n) {
        const int sav = off >= gs;
        const uint32_t o = sav ? off - gs : off;
        uint32_t k = n;
        if (!sav && off + k > gs) k = gs - off;
        int e = -1;
        if (S->src == CF_SH_SRC_CART) e = CfDev_ShareCartRead(sav, o, b, k);
        else if (S->src == CF_SH_SRC_LIB) e = CfRc_LibShareRead(&S->ent, &S->sv, sav, o, b, k);
        else {
            FIL *f = sav ? S->fs : S->fg;
            uint32_t got = 0;
            e = (!f || Sd_Read(f, o, b, k, &got) || got != k) ? -1 : 0;
        }
        if (e) { printf("share: the game's bytes at %lu could not be read\n", (unsigned long)off); return -1; }
        off += k; b += k; n -= k;
    }
    heap_note(S);
    return 0;
}
/* the game's facts from its first bytes (a ROM header, or an SGZ1 container's copy of 0x134..0x14F) */
static void hdr_facts(sh_t *S, const uint8_t *h, uint32_t n)
{
    if (n >= CF_SGZ_HDR && cf_sgz_is(h, 4)) {
        cf_sgz_hdr_t zh;
        if (!cf_sgz_parse(h, S->offer.game_size, &zh)) { S->offer.rom_size = zh.rom_size; S->offer.rom_crc32 = zh.rom_crc32; }
        S->offer.flags |= CF_SH_F_PACKED;
        S->offer.cart_type = h[48 + 0x13]; S->offer.cart_ram = h[48 + 0x15];
        if (h[48 + 0x0F] & 0x80) S->offer.flags |= 0x04;   /* (CGB: kept in the flags' bit 2 for the card's file name) */
    } else if (n >= 0x150) {
        S->offer.cart_type = h[0x147]; S->offer.cart_ram = h[0x149];
        if (h[0x143] & 0x80) S->offer.flags |= 0x04;
    }
}
static int prepare(sh_t *S)
{
    cf_sh_offer_t *o = &S->offer;
    o->src = (uint8_t)S->src;
    if (S->src == CF_SH_SRC_CART) {
        uint32_t rl = 0, sl = 0, crc = 0; int cgb = 0;
        char t[17] = "";
        cart_pct(0);
        /* the link off for the cart read (as a USB cart job: its heap is the read job's; unit 1 10-24: 3,464 bytes free at the
           lowest with the link up) */
        const bool held = CWL_CartHeld();
        if (!held) (void)CWL_CartHold(true, 8000);
        const int e = CfDev_ShareCartBegin(cart_pct, &rl, &sl, t, &o->cart_type, &o->cart_ram, &crc, &cgb);
        if (!held) (void)CWL_CartHold(false, 0);
        cart_pct(-1);
        if (e) return e == -2 ? -2 : -1;
        o->game_size = o->rom_size = rl; o->rom_crc32 = crc; o->save_size = sl;
        if (cgb) o->flags |= 0x04;
        snprintf(o->title, sizeof o->title, "%s", t);
        portENTER_CRITICAL(&_Mux); snprintf(S->v.title, sizeof S->v.title, "%s", t); portEXIT_CRITICAL(&_Mux);
    } else if (S->src == CF_SH_SRC_LIB) {
        if (CfRc_LibShareOpen(S->uid, &S->ent, &S->sv)) return -1;
        o->game_size = S->ent.stored_size; o->rom_size = S->ent.rom_size; o->rom_crc32 = S->ent.rom_crc32;
        o->save_size = (S->ent.save_size && S->sv.seq) ? S->ent.save_size : 0;
        snprintf(o->title, sizeof o->title, "%.16s", S->ent.title);
        uint8_t h[0x150];
        const uint32_t hn = S->ent.stored_size < sizeof h ? S->ent.stored_size : sizeof h;
        if (CfRc_LibShareRead(&S->ent, &S->sv, 0, 0, h, hn)) return -1;
        hdr_facts(S, h, hn);
        if (!(S->ent.flags & CF_LIB_F_SGZ)) o->flags &= (uint8_t)~CF_SH_F_PACKED;
    } else {
        if (Sd_Mount() != 0) return -1;
        S->fg = malloc(sizeof *S->fg);
        uint32_t sz = 0;
        if (!S->fg || Sd_ReadOpen(S->fg, S->path, &sz) != 0) { free(S->fg); S->fg = NULL; return -1; }
        o->game_size = o->rom_size = sz;
        uint8_t h[0x150];
        uint32_t got = 0;
        if (sz < sizeof h || Sd_Read(S->fg, 0, h, sizeof h, &got) || got != sizeof h) return -1;
        hdr_facts(S, h, sizeof h);
        o->flags &= (uint8_t)~CF_SH_F_PACKED;        /* (card games are plain files) */
        const char *base = strrchr(S->path, '/');
        base = base ? base + 1 : S->path;
        { const size_t L = strlen(base); if (L < sizeof o->file) memcpy(o->file, base, L + 1); }
        char sp[SDC_PATH_MAX + 8];
        uint32_t ss = 0;
        if (sdc_save_path(S->path, sp, sizeof sp) == 0 && Sd_FileSize(sp, &ss) == 0 && ss > 0 && ss <= 0x20030u) {
            S->fs = malloc(sizeof *S->fs);
            uint32_t z = 0;
            if (S->fs && Sd_ReadOpen(S->fs, sp, &z) == 0) o->save_size = z;
            else { free(S->fs); S->fs = NULL; }
        }
        if (!o->title[0]) snprintf(o->title, sizeof o->title, "%.16s", S->v.title);
    }
    portENTER_CRITICAL(&_Mux);
    S->v.size = o->game_size; S->v.rom_size = o->rom_size; S->v.save_avail = o->save_size > 0;
    if (!S->v.save_avail && S->src != CF_SH_SRC_CART) { S->v.with_save = 0; if (S->v.cur > 3) S->v.cur = 3; }
    portEXIT_CRITICAL(&_Mux);
    printf("share: send %s: %s%lu bytes (the game %lu, crc32 %08lx), save %lu, type %02x RAM %02x\n", o->title, (o->flags & CF_SH_F_PACKED) ? "packed, " : "",
           (unsigned long)o->game_size, (unsigned long)o->rom_size, (unsigned long)o->rom_crc32, (unsigned long)o->save_size, o->cart_type, o->cart_ram);
    return 0;
}
static void src_close(sh_t *S)
{
    if (S->src == CF_SH_SRC_CART) CfDev_ShareCartEnd();
    if (S->fg) { Sd_Close(S->fg); free(S->fg); S->fg = NULL; }
    if (S->fs) { Sd_Close(S->fs); free(S->fs); S->fs = NULL; }
}

/* ================================================================ B: where the game goes */
static const char *card_ext(const cf_sh_offer_t *o) { return (o->flags & 0x04) ? ".gbc" : ".gb"; }
static int place_check(const cf_sh_offer_t *o, int d, const char **why)
{
    *why = "";
    const uint32_t save_sz = cf_lib_cart_save(o->cart_type, o->cart_ram);
    if (d == CF_SH_DEST_LIB) {
        const uint32_t need = CfRc_LibRoomNeed(o->game_size, save_sz);
        if (need == ~0u) { *why = CfRc_LibThere() == 0 ? "" : "WAIT"; return 0; }   /* (no game area: an older FPGA; the store being read) */
        if (o->rom_crc32 && CfRc_LibHasGame(o->rom_crc32, o->rom_size)) { *why = "KEPT"; return 0; }   /* (this game is ON CHROMATIC already) */
        if (need) { *why = "FULL"; return 0; }
        return 1;
    }
    if (d == CF_SH_DEST_CARD) {
        if (Sd_Mount() != 0) { *why = "NONE"; return 0; }
        if (!Sd_Writable()) { *why = "LOCKED"; return 0; }
        uint32_t fmb = 0;
        Sd_Space(NULL, &fmb);
        if ((uint64_t)fmb * 1048576u < (uint64_t)o->rom_size + o->save_size + 0x40000u) { *why = "FULL"; return 0; }
        if ((o->flags & CF_SH_F_PACKED) && o->game_size > CF_RC_SCRATCH_SIZE) { *why = "TOO  BIG"; return 0; }
        return 1;
    }
    if (d == CF_SH_DEST_PLAY) {
        if (o->game_size > ((o->flags & CF_SH_F_PACKED) ? CF_RC_SCRATCH_SIZE : CF_RC_ROM_MAX) || o->rom_size > CF_RC_ROM_MAX) { *why = "TOO  BIG"; return 0; }
        return 1;
    }
    return 0;
}
static void on_offer(void *u, const cf_sh_offer_t *o, uint16_t unit)
{
    sh_t *S = u;
    S->in = *o;
    S->peer_unit = unit;
    const char *w[3];
    int ok[3];
    for (int d = 0; d < 3; d++) ok[d] = place_check(o, d, &w[d]);
    portENTER_CRITICAL(&_Mux);
    snprintf(S->v.title, sizeof S->v.title, "%s", o->title);
    snprintf(S->v.peer, sizeof S->v.peer, "CHROMATIC  %02X%02X", unit & 0xFF, unit >> 8);
    S->v.size = o->game_size; S->v.rom_size = o->rom_size; S->v.save_size = o->save_size; S->v.save_avail = o->save_size > 0;
    for (int d = 0; d < 3; d++) { S->v.place_ok[d] = ok[d]; S->v.place_why[d] = w[d]; }
    S->v.sel = ok[0] ? 0 : ok[1] ? 1 : 2;
    S->v.kind = CF_SHV_B_OFFER;
    portEXIT_CRITICAL(&_Mux);
    printf("share: an offer from %s: %s, %lu bytes%s%s, save %lu; ON CHROMATIC %s, CARD %s, PLAY NOW %s\n", S->v.peer, o->title, (unsigned long)o->game_size,
           (o->flags & CF_SH_F_PACKED) ? " packed" : "", o->src == CF_SH_SRC_CART ? " (a cartridge)" : o->src == CF_SH_SRC_LIB ? " (on that Chromatic)" : " (its card)",
           (unsigned long)o->save_size, ok[0] ? "ok" : w[0], ok[1] ? "ok" : w[1], ok[2] ? "ok" : w[2]);
}
static void sink_drop(sh_t *S)
{
    if (S->wg) { if (S->wg->open) Sd_WriteAbort(S->wg); free(S->wg); S->wg = NULL; }
    if (S->ws) { if (S->ws->open) Sd_WriteAbort(S->ws); free(S->ws); S->ws = NULL; }
    if (S->ram_on) { (void)CfRc_RamEnd(0); S->ram_on = 0; }
    free(S->ltx); S->ltx = NULL;
    free(S->acc); S->acc = NULL;
}
static int s_begin(void *u, const cf_sh_offer_t *o, uint8_t dest)
{
    sh_t *S = u;
    const char *why;
    if (dest >= CF_SH_NDEST || !place_check(o, dest, &why)) return CF_SH_E_ROOM;
    S->acc_off = S->acc_n = 0;
    S->ram_on = S->ram_save = 0; S->ram_region_n = 0; S->fin = 0;
    if (dest != CF_SH_DEST_LIB && S->lib_held) { CfRc_LibHold(0); S->lib_held = 0; }   /* (its 5.5 KB back: not needed for this place) */
    const int packed = (o->flags & CF_SH_F_PACKED) != 0;
    if (dest == CF_SH_DEST_LIB || (dest == CF_SH_DEST_CARD && !packed)) {
        S->acc_cap = SH_ACC;
        if (!(S->acc = malloc(S->acc_cap))) { S->acc_cap = SH_ACC / 2u; if (!(S->acc = malloc(S->acc_cap))) return CF_SH_E_WRITE; }
    }
    if (dest == CF_SH_DEST_LIB) {
        if (!(S->ltx = malloc(sizeof *S->ltx)) || CfRc_LibPlanStream(o->game_size, o->title, S->ltx)) { sink_drop(S); return CF_SH_E_ROOM; }
    }
    if (dest == CF_SH_DEST_CARD) {
        char name[64], file[64];
        snprintf(name, sizeof name, "%s", o->file[0] ? o->file : o->title[0] ? o->title : "GAME");
        if (!o->file[0]) for (char *q = name; *q; q++) if (*q == '_') *q = ' ';
        const char *ext = card_ext(o);
        if (o->file[0]) {
            char *dot = strrchr(name, '.');
            if (dot && (!strcasecmp(dot, ".gb") || !strcasecmp(dot, ".gbc"))) ext = strcasecmp(dot, ".gb") ? ".gbc" : ".gb";   /* (the sender's own file name and kind) */
            if (dot) *dot = 0;
        }
        if (sdc_clean_file(name, ext, file, sizeof file)) snprintf(file, sizeof file, "game%s", ext);
        if (Sd_FreeName(SDC_GAMES_DIR, file, S->gpath, sizeof S->gpath)) return CF_SH_E_WRITE;
        S->wg = malloc(sizeof *S->wg);
        if (!S->wg || Sd_WriteBegin(S->wg, S->gpath, o->rom_size)) { sink_drop(S); return CF_SH_E_WRITE; }
        if (o->save_size) {
            S->ws = malloc(sizeof *S->ws);
            if (!S->ws || sdc_save_path(S->gpath, S->spath, sizeof S->spath) || Sd_WriteBegin(S->ws, S->spath, 0)) { sink_drop(S); return CF_SH_E_WRITE; }
        }
    }
    /* PSRAM: a packed game for the card or to play (scratch), a raw one to play (the ROM area) */
    if ((dest == CF_SH_DEST_CARD && packed) || dest == CF_SH_DEST_PLAY) {
        if (CfRc_RamBegin()) { sink_drop(S); return CF_SH_E_WRITE; }
        S->ram_on = 1;
        if (packed && CfRc_RamRegion(CF_RC_SCRATCH_BASE, CF_RC_SCRATCH_SIZE)) { sink_drop(S); return CF_SH_E_WRITE; }
    }
    printf("share: taking it: %s%s%s (free heap %lu)\n", dest == CF_SH_DEST_LIB ? "ON CHROMATIC" : dest == CF_SH_DEST_CARD ? S->gpath : "PLAY NOW",
           packed ? ", packed" : "", o->save_size ? ", with its save" : "", (unsigned long)esp_get_free_heap_size());
    portENTER_CRITICAL(&_Mux); S->v.dest = dest; S->v.kind = CF_SHV_B_RECV; portEXIT_CRITICAL(&_Mux);
    return 0;
}
static int acc_flush(sh_t *S)
{
    if (!S->acc_n) return 0;
    int e = 0;
    if (S->ltx) e = CfRc_LibTxWrite(S->ltx, S->acc, S->acc_n);
    else if (S->wg) e = Sd_WriteAt(S->wg, S->acc_off, S->acc, S->acc_n);
    if (e) printf("share: the bytes at %lu were not taken (%d)\n", (unsigned long)S->acc_off, e);
    S->acc_off += S->acc_n; S->acc_n = 0;
    return e;
}
static int game_put(sh_t *S, uint32_t off, const uint8_t *b, uint32_t n)
{
    if (S->ram_on && !S->ram_save) { S->ram_region_n = off + n; return CfRc_RamPut(off, b, n); }
    while (n) {
        uint32_t k = S->acc_cap - S->acc_n;
        if (k > n) k = n;
        memcpy(S->acc + S->acc_n, b, k);
        S->acc_n += k; b += k; n -= k;
        if (S->acc_n == S->acc_cap && acc_flush(S)) return -1;
    }
    return 0;
}
static int save_put(sh_t *S, uint32_t off, const uint8_t *b, uint32_t n)
{
    if (S->ws) return Sd_WriteAt(S->ws, off, b, n);
    if (!S->ram_save) {
        /* into the save RAM (the store's journal or the play takes it from there) */
        if (!S->ram_on) { if (CfRc_RamBegin()) return -1; S->ram_on = 1; }
        if (CfRc_RamRegion(CF_RC_RAM_BASE, CF_RC_RAM_MAX)) return -1;
        S->ram_save = 1;
    }
    S->ram_region_n = off + n;
    return CfRc_RamPut(off, b, n);
}
static int s_write(void *u, uint32_t off, const uint8_t *b, uint32_t n)
{
    sh_t *S = u;
    const uint32_t gs = S->in.game_size;
    heap_note(S);
    if (off < gs) {
        const uint32_t k = off + n > gs ? gs - off : n;
        if (game_put(S, off, b, k)) return -1;
        if (off + k == gs && acc_flush(S)) return -1;   /* (the game's last bytes out before the save's) */
        off += k; b += k; n -= k;
    }
    return n ? save_put(S, off - gs, b, n) : 0;
}
/* m110s: PLAY NOW starts the game before the verdict goes back, so the sender's screen says what happened here (m110f:
   the sender said IT PLAYS ON THAT CHROMATIC NOW while this one showed IT DID NOT START). The radio is paused for the load
   (its frame pool back; the link stays off, so its session does not take the heap the load wants), up to 45 s; the
   sender asks again meanwhile and waits (CF_SH_LOST_MS). 0 playing, 1 still loading (asked again), < 0 did not start */
static int play_result(sh_t *S)
{
    if (CfRc_PlayDone() == S->play_seq) return 0;
    NwPlayView_t pv;
    CfRc_View(&pv);
    return pv.Kind == kNwPv_Playing ? 1 : pv.Kind == kNwPv_NoFpga ? -2 : -1;
}
static void play_look(sh_t *S)
{
    NwPlayView_t pv;
    CfRc_View(&pv);
    portENTER_CRITICAL(&_Mux); S->v.pct = pv.Pct; portEXIT_CRITICAL(&_Mux);
    if (!S->play_res) S->play_res = play_result(S);
}
static int play_check(sh_t *S)
{
    play_look(S);
    if (!S->play_res) return 1;
    printf("share: PLAY NOW: %s (free heap %lu)\n", S->play_res > 0 ? "playing" : S->play_res == -2 ? "this FPGA cannot play it" : "it did not start",
           (unsigned long)esp_get_free_heap_size());
    return S->play_res > 0 ? 0 : -CF_SH_E_START;
}
static int play_start(sh_t *S)
{
    const int packed = (S->in.flags & CF_SH_F_PACKED) != 0;
    portENTER_CRITICAL(&_Mux); S->v.kind = CF_SHV_B_AFTER; S->v.pct = 0; portEXIT_CRITICAL(&_Mux);
    CwlSh_Pause(1);
    S->play_res = 0;
    S->play_seq = CfRc_PlayDone();
    if (packed) CfRc_PlayScratch(S->in.game_size, S->in.title, S->in.save_size > 0);
    else CfRc_PlayInRamSave(S->in.game_size, S->rx.sum.crc, S->in.title, S->in.save_size > 0);
    for (int t = 0; t < 450 && (play_look(S), !S->play_res); t++) vTaskDelay(pdMS_TO_TICKS(100));
    CwlSh_Pause(0);
    return play_check(S);
}
/* all came: 0 kept (or ready for the step after the radio), 1 still working, < 0 not kept */
static int s_finish(void *u, int ok)
{
    sh_t *S = u;
    if (!ok) { sink_drop(S); printf("share: what came is let go (nothing kept)\n"); return 0; }
    const int packed = (S->in.flags & CF_SH_F_PACKED) != 0;
    switch (S->fin) {
    case 0:
        if (acc_flush(S)) { sink_drop(S); return -CF_SH_E_WRITE; }
        /* m110s: the gathering buffer goes as soon as the last bytes are out. Before, nothing freed it after a share that
           worked (only sink_drop, on a failure did): 2 KB lost for good per game received ON CHROMATIC or onto the CARD, in
           the middle of the heap (m110f: unit 1 1,180 / unit 2 1,300 lowest free after a few shares, from m110g1's 4,152) */
        free(S->acc); S->acc = NULL; S->acc_cap = 0;
        if (S->ram_on) {
            const int e = CfRc_RamEnd(S->ram_region_n);
            S->ram_on = 0;
            if (e) { sink_drop(S); return -CF_SH_E_WRITE; }
        }
        set_view(S, CF_SHV_B_KEEP);
        if (S->ltx) {
            /* the store's index (and the save) written with the radio paused: the store opens with 5,528 bytes in one piece,
               which were not there beside the radio's frame pool (unit 2 10-24: 8,668 free, the largest 2,816; holding the
               store through the transfer took unit 2 down to 2,000 free). The sender asks again every 300 ms meanwhile */
            CwlSh_Pause(1);
            int e = 0;
            for (int t = 0; t < 100 && CfRc_LibBusy(); t++) vTaskDelay(pdMS_TO_TICKS(100));
            const uint32_t f0 = (uint32_t)esp_get_free_heap_size(), b0 = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
            if (CfRc_LibStreamEnd(S->ltx)) e = -1;
            for (int t = 0; !e && t < 1200 && CfRc_LibBusy(); t++) vTaskDelay(pdMS_TO_TICKS(100));
            if (!e) e = CfRc_LibResult();
            printf("share: the store's index with the radio paused (free heap %lu, largest %lu): e=%d\n", (unsigned long)f0, (unsigned long)b0, e);
            if (!e) {
                S->lib_uid = CfRc_LibUid();
                if (S->in.save_size && S->ram_save) {
                    int se = CfRc_LibSavePutFromRam(S->lib_uid);
                    for (int t = 0; !se && t < 600 && CfRc_LibBusy(); t++) vTaskDelay(pdMS_TO_TICKS(100));
                    if (!se) se = CfRc_LibResult();
                    if (se) printf("share: the game is kept, its save did not go in (%d)\n", se);
                    CfRc_RamRelease();
                    S->ram_save = 0;
                }
            }
            free(S->ltx); S->ltx = NULL;
            CwlSh_Pause(0);
            if (e) { sink_drop(S); return -CF_SH_E_WRITE; }
            return 0;
        }
        if (S->rx.dest == CF_SH_DEST_PLAY) { S->fin = 1; return play_start(S); }
        if (S->rx.dest == CF_SH_DEST_CARD && !packed) {
            int e = Sd_WriteCommit(S->wg);
            free(S->wg); S->wg = NULL;
            if (!e && S->ws) { e = Sd_WriteCommit(S->ws); free(S->ws); S->ws = NULL; }
            Sd_RefreshSpace();
            if (e) { sink_drop(S); return -CF_SH_E_WRITE; }
            printf("share: on the card as %s%s\n", S->gpath, S->in.save_size ? " with its save" : "");
        }
        return 0;                                           /* (CARD packed: the step after the radio) */
    case 1:
        return play_check(S);                               /* (PLAY NOW: still loading after the radio came back) */
    }
    return 0;
}

/* ---------------------------------------------------------------- B, after the radio: unpack onto the card, or play */
static int sc_rd(void *u, uint32_t off, uint8_t *b, uint32_t n) { (void)u; return CfRc_ScratchRead(off, b, n) ? -1 : 0; }
static int sc_rd_fast(void *u, uint32_t off, uint8_t *b, uint32_t n) { sh_t *S = u; return CfRc_ScratchReadFast(off, b, n, S->in.game_size) ? -1 : 0; }
static int sd_wr(void *u, uint32_t off, const uint8_t *b, uint32_t n) { sh_t *S = u; return Sd_WriteAt(S->wg, off, b, n) ? -1 : 0; }
static void z_pct(void *u, int p) { sh_t *S = u; portENTER_CRITICAL(&_Mux); S->v.pct = p; portEXIT_CRITICAL(&_Mux); }
static void after(sh_t *S)
{
    const int packed = (S->in.flags & CF_SH_F_PACKED) != 0;
    const int d = S->rx.dest;
    if (d == CF_SH_DEST_LIB) { result(S, 1, "ON  CHROMATIC", S->in.save_size ? "WITH  ITS  SAVE" : "PLAY  IT  FROM  GAMES"); return; }
    if (d == CF_SH_DEST_CARD && !packed) { result(S, 1, "ON  THE  CARD", S->in.save_size ? "WITH  ITS  SAVE" : "PLAY  IT  FROM  GAMES"); return; }
    portENTER_CRITICAL(&_Mux); S->v.kind = CF_SHV_B_AFTER; S->v.pct = 0; portEXIT_CRITICAL(&_Mux);
    if (d == CF_SH_DEST_CARD) {
        cf_sgz_hdr_t h;
        /* the link stays off for it (its session would take the heap the unpack needs and the FPGA's UART), as a load does */
        const bool held = CWL_CartHeld();
        if (!held) (void)CWL_CartHold(true, 8000);
        const int64_t t0 = esp_timer_get_time();
        int e = CfRc_ScratchReadBegin() ? CF_SGZ_E_READ : CF_SGZ_E_MEM;
        for (int t = 0; t < 10 && e == CF_SGZ_E_MEM; t++) { e = cf_sgz_unpack2(sc_rd_fast, sc_rd, sd_wr, S, S->in.game_size, &h, z_pct); CfRc_ScratchFastDone(); if (e == CF_SGZ_E_MEM) vTaskDelay(pdMS_TO_TICKS(300)); }
        printf("share: unpacked onto the card: %s in %lld ms (reading %lld, inflating %lld, the card %lld ms; %lu blocks read again; free heap %lu)\n", cf_sgz_why(e),
               (long long)((esp_timer_get_time() - t0) / 1000), (long long)(cf_sgz_prof.read_us / 1000), (long long)(cf_sgz_prof.inflate_us / 1000),
               (long long)(cf_sgz_prof.write_us / 1000), (unsigned long)cf_sgz_prof.retries, (unsigned long)esp_get_free_heap_size());
        if (!held) (void)CWL_CartHold(false, 0);
        int ce = e ? -1 : Sd_WriteCommit(S->wg);
        free(S->wg); S->wg = NULL;
        if (!ce && S->ws) { ce = Sd_WriteCommit(S->ws); free(S->ws); S->ws = NULL; }
        CfRc_RamRelease();
        Sd_RefreshSpace();
        if (e || ce) { sink_drop(S); result(S, 0, "IT  COULD  NOT", "BE  PUT  ON  THE  CARD"); return; }
        result(S, 1, "ON  THE  CARD", S->in.save_size ? "WITH  ITS  SAVE" : "PLAY  IT  FROM  GAMES");
        return;
    }
    /* PLAY NOW: it started before the verdict went back (play_start); here only when it plays */
    result(S, 1, "PLAYING  NOW", "NOT  KEPT");
}

/* ================================================================ the task */
static void view_progress(sh_t *S, uint32_t have, uint32_t total, uint32_t t_first, uint32_t t_heard)
{
    const uint32_t now = now_ms();
    portENTER_CRITICAL(&_Mux);
    S->v.pct = total ? (int)((uint64_t)have * 100u / total) : 0;
    S->v.kbps = (t_first && now > t_first) ? (uint32_t)((uint64_t)have / (now - t_first)) : 0;   /* (bytes per ms = KB/s) */
    S->v.paused = (now - t_heard >= 2000u) ? (int)((now - t_heard) / 1000u) : 0;
    portEXIT_CRITICAL(&_Mux);
}
static void print_stats(sh_t *S, const char *who, uint32_t bytes, uint32_t t0, uint32_t t1)
{
    CwlShStats_t st;
    CwlSh_GetStats(&st);
    const uint32_t ms = (t0 && t1 > t0) ? t1 - t0 : 1;   /* (no data moved: t0 0) */
    printf("share: %s: %lu bytes in %lu ms = %lu B/s (radio: channel %u, rate %u, %lu sent, %lu acked, %lu not, %lu waits for room, %lu received, %lu dropped, queue low %lu, rssi %d, %lu us per frame in the air, %lu ms waiting for room; heap at the start %lu, lowest %lu / %lu)\n",
           who, (unsigned long)bytes, (unsigned long)ms, (unsigned long)((uint64_t)bytes * 1000u / ms), st.channel, st.rate, (unsigned long)st.tx, (unsigned long)st.tx_ok,
           (unsigned long)st.tx_fail, (unsigned long)st.tx_full, (unsigned long)st.rx, (unsigned long)st.rx_drop, (unsigned long)st.rx_queue_low, st.last_rssi,
           (unsigned long)st.lat_us, (unsigned long)st.wait_ms, (unsigned long)st.heap_at_start, (unsigned long)st.heap_low, (unsigned long)S->heap_low);
}
/* before the radio: the unit quiet, as a load makes it (cf_rc_dev.c load_quiet_begin): a RAM game stopped (its save kept)
   and gone, the relay's and the network window's TLS clients closed, the RAM cart's window buffer back; then up to 5 s for
   the heap. Unit 2 2026-10-24: a share started right after a PLAY NOW, with the relay just reconnected, had 5,024 bytes free
   (largest 1,216) at the radio's start, and its card write then stalled for 10 minutes */
/* m110: 1 = a RAM game was stopped: the cartridge's game restarted, and that closed the menu on the screen (the share's
   page with it) while the ESP32 still takes it as shown (main/fpga_rx.c learns it from button packets only): told, and
   the page opened again over the cartridge's game (unit 2, 2026-10-24: SHARE on an ON CHROMATIC game while one played,
   the code page gone, the code typed into the cartridge's game) */
static void menu_back(const char *page)
{
    extern void OSD_SetVisiblityState(const bool IsVisible);
    vTaskDelay(pdMS_TO_TICKS(1500));
    OSD_SetVisiblityState(false);
    NW_OpenPage(page);
}
static int quiet(sh_t *S)
{
    const uint32_t f0 = (uint32_t)esp_get_free_heap_size();
    const int stopped = CfRc_Yield();
    if (stopped) for (int t = 0; t < 100 && CfRc_Playing(); t++) vTaskDelay(pdMS_TO_TICKS(50));
    (void)CwlOnline_Quiet(5000);
    (void)NW_DropClient(5000);
    CfRc_Trim();
    for (int t = 0; t < 50 && (esp_get_free_heap_size() < 26000u || heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 12000u); t++) vTaskDelay(pdMS_TO_TICKS(100));
    printf("share: quiet for the radio: free heap %lu -> %lu (largest %lu)\n", (unsigned long)f0, (unsigned long)esp_get_free_heap_size(),
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    (void)S;
    return stopped;
}
static void run_a(sh_t *S)
{
    /* the cartridge is read only after SEND: its read runs the cart writer on the Game Boy side (a white screen, the menu
       gone: units 1 + 2 10-24, the SEND pressed meanwhile reached the game). The menu comes back on SYSTEM > GAMES after it */
    const int cart = S->src == CF_SH_SRC_CART;
    if (cart) {
        while (!S->go && !S->stop) vTaskDelay(pdMS_TO_TICKS(50));
        if (S->stop) { result(S, 0, "STOPPED", ""); return; }
        set_view(S, CF_SHV_A_READ);
    }
    const int stopped = quiet(S);                       /* (before the source: a RAM game stopped, the heap whole for the store's 5.5 KB) */
    S->prep_err = prepare(S);
    if (cart) { vTaskDelay(pdMS_TO_TICKS(1500)); NW_OpenPage("GAMES"); }   /* (the cart started again: the menu over it once more) */
    else if (stopped) menu_back("GAMES");               /* (m110: the RAM game stopped: the menu over the cartridge's game again) */
    S->prepared = 1;
    /* m110s: SYSTEM > GAMES holds the library open while it shows (~5.5 KB and its list). A send reads its game by the
       extents prepare() copied, so the hold goes for the send and comes back after: unit 1, after a few jobs, an ON
       CHROMATIC send with it held found 4,620 free (largest 992) for the radio's ring, THE RADIO DID NOT START */
    if (CfRc_LibHeld()) { S->lib_let_go = 1; CfRc_LibHold(0); }
    if (S->prep_err) {
        result(S, 0, S->prep_err == -2 ? "NO  CARTRIDGE  IN" : "THE  GAME  COULD", S->prep_err == -2 ? "THE  SLOT" : "NOT  BE  READ");
        src_close(S);
        return;
    }
    while (!S->go && !S->stop) vTaskDelay(pdMS_TO_TICKS(50));
    if (S->stop) { src_close(S); result(S, 0, "STOPPED", ""); return; }
    if (!S->v.with_save) S->offer.flags &= (uint8_t)~CF_SH_F_SAVE; else S->offer.flags |= CF_SH_F_SAVE;
    if (!(S->offer.flags & CF_SH_F_SAVE)) S->offer.save_size = 0;
    set_view(S, CF_SHV_A_FIND);
    quiet(S);                                           /* (again: the person may have typed for minutes) */
    /* the radio first (the Wi-Fi driver's buffers want the heap whole), then the ring and the hash */
    /* m110q: SYSTEM > GAMES opened again above (after a cartridge read, or a RAM game stopped) takes its hold when it
       first draws, which can come after the let-go above: unit 1, a cartridge share, `rc: lib: open` during this quiet,
       then the radio at 9,672 free (largest 4,096) and its ring at largest 992: THE RADIO DID NOT START. Let it go again */
    int late_hold = 0;
    if (CfRc_LibHeld()) { S->lib_let_go = 1; CfRc_LibHold(0); late_hold = 1; }
    for (int t = 0; t < 20 && CfRc_LibBusy(); t++) vTaskDelay(pdMS_TO_TICKS(100));
    if (S->src == CF_SH_SRC_LIB || late_hold) vTaskDelay(pdMS_TO_TICKS(800));   /* (the store's task closes it half a second after its last op: ~5.5 KB back) */
    if (late_hold) printf("share: the GAMES page took the library again: let go for the radio (free heap %lu, largest %lu)\n",
                          (unsigned long)esp_get_free_heap_size(), (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    const esp_err_t re = CwlSh_Start(CwlSh_HomeChannel(), 8);
    int ring_ok = re == ESP_OK;
    for (unsigned i = 0; ring_ok && i < CF_SH_NSEG; i++) ring_ok = (S->seg[i] = malloc(CF_SH_SEG)) != NULL;
    if (ring_ok) S->sha = malloc(cf_sh_sha_size());
    if (re != ESP_OK || !ring_ok || !S->sha) {
        printf("share: send: %s (free heap %lu, largest %lu)\n", re != ESP_OK ? "no radio" : "no memory for the ring", (unsigned long)esp_get_free_heap_size(),
               (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        const char *r1, *r2; why_rows(CF_SH_E_RADIO, 1, &r1, &r2);
        CwlSh_Stop(); src_close(S); result(S, 0, r1, r2); return;
    }
    S->t_radio = now_ms();
    uint8_t mac[6];
    CwlSh_OwnMac(mac);
    const cf_sh_radio_t r = { S, r_send, r_chan, r_log };
    cf_sh_tx_init(&S->tx, &r, src_read, S->seg, S->sha, mac, CwlSh_HomeChannel(), esp_random() | 1u, now_ms());
    cf_sh_tx_start(&S->tx, S->v.code, &S->offer, now_ms());
    int last = -1;
    CwlShRx_t *f = malloc(sizeof *f);
    while (f && cf_sh_tx_busy(&S->tx)) {
        int got = 0;
        while (got < 16 && CwlSh_Recv(f, got ? 0 : 2)) { cf_sh_tx_frame(&S->tx, f->src, f->f, f->len, now_ms()); got++; }
        cf_sh_tx_tick(&S->tx, now_ms());
        if (S->stop) cf_sh_tx_cancel(&S->tx, now_ms());
        const int st = S->tx.st;
        if (st != last) {
            last = st;
            if (st == CF_SH_TX_ASKED || st == CF_SH_TX_HELLO) {
                portENTER_CRITICAL(&_Mux); snprintf(S->v.peer, sizeof S->v.peer, "CHROMATIC  %02X%02X", S->tx.peer_unit & 0xFF, S->tx.peer_unit >> 8); S->v.kind = CF_SHV_A_ASKED; portEXIT_CRITICAL(&_Mux);
            } else if (st == CF_SH_TX_SEND) set_view(S, CF_SHV_A_SEND);
            else if (st == CF_SH_TX_END) set_view(S, CF_SHV_A_END);
        }
        if (st == CF_SH_TX_SEND) view_progress(S, S->tx.have, S->tx.total, S->tx.t_first, S->tx.t_heard);
        heap_note(S);
    }
    free(f);
    for (unsigned i = 0; i < CF_SH_NSEG; i++) { free(S->seg[i]); S->seg[i] = NULL; }   /* (freed before the link session comes back: */
    free(S->sha); S->sha = NULL;                                                   /*  its allocations then do not land between them) */
    print_stats(S, "sent", S->tx.have, S->tx.t_first, S->tx.t_last ? S->tx.t_last : now_ms());
    printf("share: sender: %lu data frames, %lu resends, %lu rewinds (%lu NAKs), %lu probes, %lu times no room, %lu frames in, %lu bad\n",
           (unsigned long)S->tx.n_data, (unsigned long)S->tx.n_resend, (unsigned long)S->tx.n_rewind, (unsigned long)S->tx.n_nak, (unsigned long)S->tx.n_probe,
           (unsigned long)S->tx.n_noroom, (unsigned long)S->tx.n_frames_in, (unsigned long)S->tx.n_bad);
    CwlSh_Stop();
    src_close(S);
    if (S->tx.st == CF_SH_TX_DONE) {
        static const char *const kWhere[3] = { "IT  IS  ON  THAT", "IT  IS  ON  THAT", "IT  PLAYS  ON  THAT" };
        static const char *const kWhere2[3] = { "CHROMATIC  NOW", "CHROMATIC'S  CARD", "CHROMATIC  NOW" };
        const int d = S->tx.dest < 3 ? S->tx.dest : 0;
        result(S, 1, kWhere[d], kWhere2[d]);
    } else {
        const char *r1, *r2; why_rows(S->tx.err, 1, &r1, &r2);
        result(S, 0, r1, r2);
    }
}
static void run_b(sh_t *S)
{
    if (quiet(S)) menu_back("DOWNLOAD");               /* (m110: as on A: the receive page back over the cartridge's game) */
    const esp_err_t re = CwlSh_Start(0, (int)CF_SH_QUEUE);        /* (0: the quietest of 1 / 6 / 11) */
    if (re == ESP_OK) S->sha = malloc(cf_sh_sha_size());
    if (re != ESP_OK || !S->sha) {
        printf("share: receive: %s (free heap %lu)\n", re != ESP_OK ? "no radio" : "no memory", (unsigned long)esp_get_free_heap_size());
        const char *r1, *r2; why_rows(CF_SH_E_RADIO, 2, &r1, &r2);
        CwlSh_Stop(); result(S, 0, r1, r2); return;
    }
    S->t_radio = now_ms();
    uint8_t mac[6];
    CwlSh_OwnMac(mac);
    char code[5];
    snprintf(code, sizeof code, "%04lu", (unsigned long)(esp_random() % 10000u));
    const cf_sh_radio_t r = { S, r_send, NULL, r_log };
    cf_sh_rx_init(&S->rx, &r, S->sha, mac, now_ms());
    S->rx.on_offer = on_offer; S->rx.begin = s_begin; S->rx.write = s_write; S->rx.finish = s_finish;
    cf_sh_rx_start(&S->rx, code, now_ms());
    portENTER_CRITICAL(&_Mux); snprintf(S->v.code, sizeof S->v.code, "%s", code); S->v.kind = CF_SHV_B_CODE; portEXIT_CRITICAL(&_Mux);
    printf("share: receiving: the code is %s (channel %u)\n", code, CwlSh_Channel());
    CwlShRx_t *f = malloc(sizeof *f);
    while (f && cf_sh_rx_busy(&S->rx)) {
        int got = 0;
        while (got < 24 && CwlSh_Recv(f, got ? 0 : 2)) { cf_sh_rx_frame(&S->rx, f->src, f->f, f->len, now_ms()); got++; }
        cf_sh_rx_tick(&S->rx, now_ms());
        if (S->answer) { const int a = S->answer; S->answer = 0; cf_sh_rx_answer(&S->rx, a == 1, (uint8_t)S->v.sel, now_ms()); }
        if (S->stop) cf_sh_rx_cancel(&S->rx, now_ms());
        if (S->rx.st == CF_SH_RX_DATA) view_progress(S, S->rx.have, S->rx.total, S->rx.t_first, S->rx.t_heard);
        heap_note(S);
    }
    /* the verdict goes on being said while the sender asks (its END comes again until it hears the RESULT) */
    const uint32_t t_end = now_ms();
    uint32_t t_last = now_ms();
    while (f && now_ms() - t_end < 4000u && now_ms() - t_last < 800u) {
        if (CwlSh_Recv(f, 20)) { cf_sh_rx_frame(&S->rx, f->src, f->f, f->len, now_ms()); t_last = now_ms(); }
    }
    free(f);
    free(S->sha); S->sha = NULL;
    print_stats(S, "received", S->rx.have, S->rx.t_first, S->rx.t_end ? S->rx.t_end : now_ms());
    printf("share: receiver: %lu data frames, %lu dups, %lu out of order, %lu ACKs, %lu bad\n", (unsigned long)S->rx.n_data, (unsigned long)S->rx.n_dup,
           (unsigned long)S->rx.n_ooo, (unsigned long)S->rx.n_acks, (unsigned long)S->rx.n_bad);
    CwlSh_Stop();
    if (S->lib_held) { CfRc_LibHold(0); S->lib_held = 0; }
    if (S->rx.st == CF_SH_RX_DONE) { after(S); return; }
    sink_drop(S);
    const char *r1, *r2; why_rows(S->rx.err, 2, &r1, &r2);
    if (S->rx.err == CF_SH_E_START && S->play_res == -2) r2 = "UPDATE  FIRST";
    result(S, 0, r1, r2);
}
/* the session over: its result (unless closed meanwhile: A: OK on a result that came just before the end, the bench's
   close) kept as a copy of the view, the session freed (m110s) */
static void session_end(sh_t *S)
{
    if (S->link_held && !CwlSh_On() && CwlSh_LinkHeld()) CwlSh_HoldLink(false);   /* (stopped before the radio: the link back) */
    portENTER_CRITICAL(&_Mux);
    S->task_done = 1;
    if (_S == S) {
        _S = NULL;
        if (!S->closed && (S->v.kind == CF_SHV_A_RESULT || S->v.kind == CF_SHV_B_RESULT)) { _Done = S->v; _DoneOn = 1; }
    }
    portEXIT_CRITICAL(&_Mux);
    free(S);
    _TaskOn = 0;
}
static void task(void *arg)
{
    sh_t *S = arg;
    S->t_start = now_ms();
    S->heap_start = (uint32_t)esp_get_free_heap_size();
    if (S->role == 1) run_a(S); else run_b(S);
    if (S->lib_let_go && !CfRc_LibHeld()) CfRc_LibHold(1);   /* (m110s: the page's hold back) */
    for (unsigned i = 0; i < CF_SH_NSEG; i++) { free(S->seg[i]); S->seg[i] = NULL; }
    free(S->sha); S->sha = NULL;
    free(S->acc); S->acc = NULL;                        /* (m110s: whatever way it ended) */
    free(S->ltx); S->ltx = NULL;
    printf("share: over in %lu ms (free heap %lu, lowest %lu)\n", (unsigned long)(now_ms() - S->t_start), (unsigned long)esp_get_free_heap_size(), (unsigned long)S->heap_low);
    session_end(S);
    vTaskDelete(NULL);
}
/* m110s: the starter (SH_GO_STACK): the heap made whole for the share's own task, which it then starts */
static void go_task(void *arg)
{
    sh_t *S = arg;
    const uint32_t f0 = (uint32_t)esp_get_free_heap_size(), b0 = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    int ok = 0;
    for (int t = 0; t < SH_GO_TRIES && !ok; t++) {
        if (t == 0) { (void)CwlOnline_Quiet(5000); (void)NW_DropClient(5000); CfRc_Trim(); }
        if (t == 10 && !CwlSh_LinkHeld()) { CwlSh_HoldLink(true); S->link_held = 1; }   /* (the link session ends: Wi-Fi off) */
        if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= SH_TASK_STACK + 512u && xTaskCreate(task, "share", SH_TASK_STACK, S, 5, NULL) == pdPASS) ok = 1;
        else vTaskDelay(pdMS_TO_TICKS(100));
    }
    printf("share: the heap in pieces (free %lu, largest %lu): its task %s (free %lu, largest %lu)\n", (unsigned long)f0, (unsigned long)b0,
           ok ? "started" : "COULD NOT START", (unsigned long)esp_get_free_heap_size(), (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (!ok) { result(S, 0, "NOT  ENOUGH  MEMORY", "TURN  IT  OFF  AND  ON"); session_end(S); }
    vTaskDelete(NULL);
}

/* ================================================================ the screen's side */
int CfShare_Active(void) { return _S != NULL || _DoneOn; }
int CfShare_Busy(void) { return _TaskOn; }

/* 0 = the page is up; -1 another share or job runs; -2 not even the starter's memory (the caller says so on its page) */
static int start(sh_t *S)
{
    if (_S || _TaskOn) { free(S); return -1; }
    portENTER_CRITICAL(&_Mux);
    _S = S;
    _DoneOn = 0;                                        /* (a new share: the last one's result goes) */
    portEXIT_CRITICAL(&_Mux);
    _TaskOn = 1;
    if (xTaskCreate(task, "share", SH_TASK_STACK, S, 5, NULL) == pdPASS) return 0;
    if (xTaskCreate(go_task, "share_go", SH_GO_STACK, S, 5, NULL) == pdPASS) return 0;
    printf("share: not started: no memory for its task (free heap %lu, largest %lu)\n", (unsigned long)esp_get_free_heap_size(),
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    _TaskOn = 0; portENTER_CRITICAL(&_Mux); _S = NULL; portEXIT_CRITICAL(&_Mux); free(S);
    return -2;
}
int CfShare_SendStart(int src, uint32_t uid, const char *path, const char *title)
{
    if (_S || CfShare_Busy() || CfDev_ShareOthersBusy()) return -1;
    sh_t *S = calloc(1, sizeof *S);
    if (!S) return -2;
    S->role = 1; S->src = src; S->uid = uid;
    snprintf(S->path, sizeof S->path, "%s", path ? path : "");
    snprintf(S->v.title, sizeof S->v.title, "%s", title ? title : "");
    snprintf(S->v.code, sizeof S->v.code, "0000");
    S->v.kind = CF_SHV_A_CODE;
    S->v.read_pct = -1;
    S->v.save_avail = 1;                         /* (a cartridge: offered before its header is read; a game with no save sends none) */
    printf("share: send %s (from %s)\n", S->v.title, src == CF_SH_SRC_CART ? "the cartridge" : src == CF_SH_SRC_LIB ? "this Chromatic" : "the card");
    return start(S);
}
int CfShare_RecvStart(void)
{
    if (_S || CfShare_Busy() || CfDev_ShareOthersBusy()) return -1;
    sh_t *S = calloc(1, sizeof *S);
    if (!S) return -2;
    S->role = 2;
    S->v.kind = CF_SHV_B_START;
    return start(S);
}
void CfShare_Close(void)
{
    /* (under the lock: the task frees its session when it ends; a result shown is then only a copy) */
    portENTER_CRITICAL(&_Mux);
    sh_t *S = _S;
    if (!S) _DoneOn = 0;
    else { S->stop = 1; if (S->v.kind == CF_SHV_A_RESULT || S->v.kind == CF_SHV_B_RESULT) S->closed = 1; }   /* (its result shown: none kept) */
    portEXIT_CRITICAL(&_Mux);
}
void CfShare_View(cf_share_view_t *v)
{
    memset(v, 0, sizeof *v);
    portENTER_CRITICAL(&_Mux);
    if (_S) *v = _S->v;
    else if (_DoneOn) *v = _Done;
    portEXIT_CRITICAL(&_Mux);
}
void CfShare_Button(int b)
{
    int close = 0;
    portENTER_CRITICAL(&_Mux);
    sh_t *S = _S;
    if (!S) { if (_DoneOn && (b == CF_SHB_A || b == CF_SHB_B)) _DoneOn = 0; portEXIT_CRITICAL(&_Mux); return; }   /* (a result: A: OK) */
    cf_share_view_t *v = &S->v;
    switch (v->kind) {
    case CF_SHV_A_CODE:
        if (b == CF_SHB_UP || b == CF_SHB_DOWN) {
            if (v->cur < 4) { const int d = v->code[v->cur] - '0'; v->code[v->cur] = (char)('0' + (d + (b == CF_SHB_UP ? 1 : 9)) % 10); }
            else v->with_save = !v->with_save;
        } else if (b == CF_SHB_LEFT) v->cur = v->cur > 0 ? v->cur - 1 : (v->save_avail ? 4 : 3);
        else if (b == CF_SHB_RIGHT) v->cur = v->cur < (v->save_avail ? 4 : 3) ? v->cur + 1 : 0;
        else if (b == CF_SHB_A) { S->go = 1; v->kind = S->prepared ? CF_SHV_A_FIND : CF_SHV_A_READ; }
        else if (b == CF_SHB_B) S->stop = 1;
        break;
    case CF_SHV_A_READ: case CF_SHV_A_FIND: case CF_SHV_A_ASKED: case CF_SHV_A_SEND: case CF_SHV_A_END:
    case CF_SHV_B_START: case CF_SHV_B_CODE: case CF_SHV_B_RECV:
        if (b == CF_SHB_B) S->stop = 1;
        break;
    case CF_SHV_B_OFFER:
        if (b == CF_SHB_UP || b == CF_SHB_DOWN) {
            for (int k = 1; k <= 3; k++) {
                const int t = b == CF_SHB_DOWN ? (v->sel + k) % 3 : (v->sel + 3 - k) % 3;
                if (v->place_ok[t]) { v->sel = t; break; }
            }
        } else if (b == CF_SHB_A) { if (v->place_ok[v->sel]) S->answer = 1; }
        else if (b == CF_SHB_B) S->answer = 2;
        break;
    case CF_SHV_A_RESULT: case CF_SHV_B_RESULT:
        if (b == CF_SHB_A || b == CF_SHB_B) close = 1;
        break;
    default: break;
    }
    portEXIT_CRITICAL(&_Mux);
    if (close) CfShare_Close();
}

/* ================================================================ the console bench */
static int cmd_share(int argc, char **argv)
{
    sh_t *S = _S;
    if (argc >= 2 && !strcmp(argv[1], "recv")) { const int e = CfShare_RecvStart(); printf("share: e=%d\n", e); return 0; }
    if (argc >= 4 && !strcmp(argv[1], "send")) {
        /* share send cart CODE [save] | share send lib UID CODE [save] | share send card PATH CODE [save] */
        int src = !strcmp(argv[2], "cart") ? CF_SH_SRC_CART : !strcmp(argv[2], "lib") ? CF_SH_SRC_LIB : !strcmp(argv[2], "card") ? CF_SH_SRC_CARD : -1;
        const int ai = src == CF_SH_SRC_CART ? 3 : 4;
        if (src < 0 || argc <= ai || !cf_sh_code_ok(argv[ai])) { printf("share: e=1 usage: share send cart|lib UID|card PATH CODE [save]\n"); return 0; }
        char t[17] = "BENCH";
        if (src == CF_SH_SRC_CARD) { const char *b = strrchr(argv[3], '/'); snprintf(t, sizeof t, "%s", b ? b + 1 : argv[3]); char *d = strrchr(t, '.'); if (d) *d = 0; }
        const int e = CfShare_SendStart(src, src == CF_SH_SRC_LIB ? (uint32_t)strtoul(argv[3], NULL, 0) : 0, src == CF_SH_SRC_CARD ? argv[3] : NULL, t);
        if (!e && _S) {
            portENTER_CRITICAL(&_Mux); snprintf(_S->v.code, sizeof _S->v.code, "%s", argv[ai]); _S->v.with_save = argc > ai + 1 && !strcmp(argv[ai + 1], "save"); portEXIT_CRITICAL(&_Mux);
            _S->go = 1;
        }
        printf("share: e=%d\n", e);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "take") && S) {
        const int d = !strcmp(argv[2], "lib") ? 0 : !strcmp(argv[2], "card") ? 1 : !strcmp(argv[2], "play") ? 2 : -1;
        if (d < 0) { S->answer = 2; printf("share: e=0 no\n"); return 0; }
        portENTER_CRITICAL(&_Mux); S->v.sel = d; portEXIT_CRITICAL(&_Mux);
        S->answer = 1;
        printf("share: e=0 take %d\n", d);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "btn")) {
        static const char *const k[6] = { "a", "b", "up", "down", "left", "right" };
        for (int i = 0; i < 6; i++) if (!strcmp(argv[2], k[i])) CfShare_Button(i + 1);
    }
    if (argc >= 3 && !strcmp(argv[1], "show")) NW_OpenPage(argv[2]);   /* bench: SYSTEM > DOWNLOAD / GAMES / PLAY on the screen */
    if (argc >= 2 && !strcmp(argv[1], "stop") && S) S->stop = 1;
    if (argc >= 2 && !strcmp(argv[1], "close")) CfShare_Close();
    if (argc >= 3 && !strcmp(argv[1], "rate")) CwlSh_SetRate((uint8_t)strtoul(argv[2], NULL, 0));
    if (argc >= 3 && !strcmp(argv[1], "mute")) CwlSh_Mute((uint32_t)strtoul(argv[2], NULL, 0));
    if (argc >= 3 && !strcmp(argv[1], "inair")) CwlSh_SetInAir(atoi(argv[2]));
    if (argc >= 3 && !strcmp(argv[1], "listen") && !_TaskOn) {
        /* bench: the radio on for MS, every frame counted (the other unit blasts) */
        const uint32_t ms = (uint32_t)strtoul(argv[2], NULL, 0);
        if (CwlSh_Start(argc >= 4 ? (uint8_t)atoi(argv[3]) : CwlSh_HomeChannel(), (int)CF_SH_QUEUE) == ESP_OK) {
            CwlShRx_t *f = malloc(sizeof *f);
            uint32_t n = 0, bytes = 0, t_first = 0, t_last = 0;
            const uint32_t t0 = now_ms();
            while (f && now_ms() - t0 < ms) if (CwlSh_Recv(f, 10)) { n++; bytes += f->len; if (!t_first) t_first = now_ms(); t_last = now_ms(); }
            free(f);
            CwlShStats_t st; CwlSh_GetStats(&st);
            printf("share: listen: %lu frames, %lu bytes, first to last %lu ms = %lu B/s, dropped %lu, rssi %d\n", (unsigned long)n, (unsigned long)bytes,
                   (unsigned long)(t_last - t_first), (unsigned long)(t_last > t_first ? (uint64_t)bytes * 1000u / (t_last - t_first) : 0), (unsigned long)st.rx_drop, st.last_rssi);
            CwlSh_Stop();
        }
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "blast") && !_TaskOn) {
        /* bench: N frames of 250 bytes to MAC (aa:bb:..) as fast as the radio takes them */
        unsigned m[6];
        if (sscanf(argv[2], "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) { printf("share: e=1 blast MAC N\n"); return 0; }
        uint8_t mac[6]; for (int i = 0; i < 6; i++) mac[i] = (uint8_t)m[i];
        const uint32_t n = (uint32_t)strtoul(argv[3], NULL, 0);
        if (CwlSh_Start(argc >= 5 ? (uint8_t)atoi(argv[4]) : CwlSh_HomeChannel(), 8) == ESP_OK) {
            uint8_t f[250]; memset(f, 0x5A, sizeof f); f[0] = 'S'; f[1] = 'h'; f[2] = 0; f[3] = 9;   /* (ours, a version nobody parses) */
            const uint32_t t0 = now_ms();
            uint32_t sent = 0, full = 0;
            while (sent < n && now_ms() - t0 < 60000u) { if (CwlSh_Send(mac, f, sizeof f) == 0) sent++; else full++; }
            vTaskDelay(pdMS_TO_TICKS(200));
            const uint32_t ms = now_ms() - t0;
            CwlShStats_t st; CwlSh_GetStats(&st);
            printf("share: blast: %lu frames in %lu ms = %lu B/s (acked %lu, not %lu, no room %lu, %lu us per frame in the air, in air %d, rate %u)\n", (unsigned long)sent, (unsigned long)ms,
                   (unsigned long)((uint64_t)sent * 250u * 1000u / (ms ? ms : 1)), (unsigned long)st.tx_ok, (unsigned long)st.tx_fail, (unsigned long)full, (unsigned long)st.lat_us, CwlSh_InAir(), CwlSh_Rate());
            CwlSh_Stop();
        }
        return 0;
    }
    vTaskDelay(pdMS_TO_TICKS(argc >= 4 && !strcmp(argv[1], "btn") ? atoi(argv[3]) : 100));
    cf_share_view_t v;
    CfShare_View(&v);
    CwlShStats_t st;
    CwlSh_GetStats(&st);
    printf("share: e=0 view %d code %s cur %d save %d/%d sel %d place %d%d%d pct %d kbps %lu paused %d title \"%s\" peer \"%s\" r \"%s|%s\" ok %d dest %d; task %d; radio %s ch %u rate %u tx %lu ok %lu fail %lu rx %lu drop %lu rssi %d; heap %lu largest %lu\n",
           v.kind, v.code, v.cur, v.save_avail, v.with_save, v.sel, v.place_ok[0], v.place_ok[1], v.place_ok[2], v.pct, (unsigned long)v.kbps, v.paused, v.title, v.peer,
           v.r1, v.r2, v.ok, v.dest, _TaskOn, st.on ? "on" : "off", st.channel, CwlSh_Rate(), (unsigned long)st.tx, (unsigned long)st.tx_ok, (unsigned long)st.tx_fail,
           (unsigned long)st.rx, (unsigned long)st.rx_drop, st.last_rssi, (unsigned long)esp_get_free_heap_size(), (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return 0;
}
void CfShare_Register(void)
{
    const esp_console_cmd_t c = { .command = "share", .help = "share with another Chromatic (bench): share recv | send cart CODE [save] | send lib UID CODE [save] | send card PATH CODE [save] | take lib|card|play|no | btn a|b|up|down|left|right [MS] | stop | close | show PAGE | rate N | inair N | mute MS | (state)", .func = &cmd_share };
    (void)esp_console_cmd_register(&c);
}
