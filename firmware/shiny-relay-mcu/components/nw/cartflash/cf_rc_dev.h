/* cf_rc_dev.h: Shiny Relay 1.1.0 (FPGA 18.30): the FPGA's boot flash and the RAM cart (cf_rc_dev.c). */
#pragma once
#include <stdint.h>
#include "cf_fota.h"

/* cart_xs ops (fpga cart_xs.v) */
#define CF_RC_O_FID        16u
#define CF_RC_O_FERASE     17u
#define CF_RC_O_FPROG      18u
#define CF_RC_O_FCRC       19u
#define CF_RC_O_FHDR       20u
#define CF_RC_O_FHDRERASE  21u   /* (18.31 refuses it) */
#define CF_RC_O_FREAD      22u   /* 18.31: boot flash bytes into the read window */
#define CF_RC_O_FCOPY      23u   /* 18.31: boot flash bytes straight into PSRAM (the full design only) */
#define CF_RC_O_FCDST      24u   /* 18.31: where FCOPY writes in PSRAM */
/* PSRAM above the RAM cart's ROM area (0x100000 + 4 MiB): free scratch, 3 MiB */
#define CF_RC_SCRATCH_BASE 0x500000u
#define CF_RC_SCRATCH_SIZE 0x300000u
#define CF_RC_O_RCFG       32u
#define CF_RC_O_RREAD      33u
#define CF_RC_O_RRTC       34u
/* RCFG arg */
#define CF_RC_A_ON         0x01u
#define CF_RC_A_HOLD       0x02u   /* the GB core held in reset */
#define CF_RC_A_INVAL      0x04u   /* drop the cache (a new game) */
#define CF_RC_A_MRESET     0x10u   /* reset mapper state without dropping ON (older FPGA ignores it) */
#define CF_RC_A_CLEAN      0x08u   /* clear the save RAM dirty flag */
/* the PSRAM map (fpga ramcart.v) */
#define CF_RC_RAM_BASE     0x040000u
#define CF_RC_RAM_MAX      0x020000u
#define CF_RC_ROM_BASE     0x100000u
#define CF_RC_ROM_MAX      0x400000u
/* the first FPGA with it */
#define CF_RC_FPGA_MINOR   30u
/* the first FPGA that writes its boot flash in the rf layout (cf_fota.h) */
#define CF_FOTA_FPGA_MINOR 31u

/* The running FPGA (cf_dev.c); the booted slot stays fixed until power-off. */
int      CfDev_FpgaVersion(unsigned *maj, unsigned *min);
int      CfDev_FpgaRecovery(void);
/* Device-side read availability, distinct from a failed flash write. */
#define CF_CORE_NOT_READY (-100)
int      CfDev_FpgaOther(void);          /* cached: 0 present, E_EMPTY absent, NOT_READY retry */
int      CfDev_FpgaNext(void);           /* -1 unchanged, else selected for next power-on */
int      CfDev_FpgaSwitch(int slot);     /* blocking: 0 switched / already selected, CF_FOTA_E_* on failure */
int      CfDev_FpgaSlotPending(void);    /* not read yet, during the first 10 s only; no flash ops */
int      CfDev_FpgaSlot(void);           /* 0 GB, 1 GG, -1 unknown / recovery / legacy */
int      CfDev_FpgaSwitchesLeft(void);   /* remaining switches, -1 unknown / not ready */

void     CfRc_Register(void);
void     CfRc_Init(void);
void     CfRc_GameGone(void);           /* the idle slot is about to be written: the kept game is no more */
void     CfRc_Keep(uint32_t base, uint32_t size, uint32_t crc, const char *title);
int      CfRc_Kept(void);
int      CfRc_Playing(void);
int      CfRc_Loading(void);
int      CfRc_KeepAwake(void);          /* fwshrink1: the MCU stays awake (no light sleep) while this is true */            /* fwshrink1: a load under way or asked for (not a game that just plays) */
uint32_t CfRc_SleepTimerMs(void);       /* lane savefix1: a game plays from RAM: the timer wake (ms) light sleep must carry so the save poll runs; 0 = none */
void     CfRc_Trim(void);
int      CfRc_Yield(void);              /* receive mode: a RAM game stops (save kept), its heap comes back */               /* the window buffer back to the heap (receive mode), unless it plays */
void     CfRc_PlayKept(void);
/* lane sdcard1: a game on the microSD card played with no cartridge (its save on the card) */
void     CfRc_PlaySd(const char *path);     /* NULL: the path set before */
void     CfRc_SdSetPath(const char *path);
int      CfRc_CartIn(void);
int      CfRc_SlotBack(void);                /* a RAM cart left on by a restart: off (a cart job can reach the slot) */                  /* 1 = a cartridge in the slot */
const char *CfRc_SdPath(void);
int      CfRc_SdPlaying(void);
/* 1.1.0: a game bigger than the staging area streamed into PSRAM (HTTP task), then played from there (not kept) */
int      CfRc_RamBegin(void);
int      CfRc_RamPut(uint32_t off, const uint8_t *b, uint32_t n);
int      CfRc_RamEnd(uint32_t size);
void     CfRc_PlayInRam(uint32_t size, uint32_t crc, const char *title);
void     CfRc_Stop(void);
uint32_t CfRc_StopDone(void);           /* counts stops completed after the stop:end log */
int      CfRc_TakeReleased(void);       /* 1.1.2: 1 once after the core was handed back to the cartridge (a keep left it held) */
int      CfRc_State(void);
void     CfRc_Status(const uint8_t *p, uint8_t n);
int      CfRc_Op(uint8_t op, uint32_t f, uint32_t n, uint8_t arg, uint32_t ms);
int      CfRc_Reset(void);
int      CfRc_ResetIdle(uint32_t ms);       /* RESET until idle with no error (after an ESP32 restart mid-op) */
uint32_t CfRc_Crc(void);
int      CfRc_AllFF(void);
int      CfRc_PsramRead(uint32_t addr, uint8_t *dst, uint32_t n);
int      CfRc_LoadFromFlash(uint32_t src, uint32_t dst, uint32_t len, uint32_t *crc_out);
int      CfRc_Cfg(uint8_t mbc, uint16_t rom_banks, uint8_t ram_banks, int rtc, uint8_t flags);
/* the FPGA's boot flash (FPGA updates over Wi-Fi, cf_fota.h layout rf, FPGA 18.31); 0 = done. The FPGA itself refuses
   erase / program below 0x05C000 (headers + recovery design) and header writes at 0x7C0 and up. */
int      CfRc_RingBegin(void);                                   /* ring positions to 0 (before the writes below) */
int      CfRc_FlashErase64(uint32_t a);                          /* a 64 KiB block, its start >= 0x05C000 */
int      CfRc_FlashProg(uint32_t a, const uint8_t *b, uint32_t n);   /* n <= 4096 bytes, a >= 0x05C000 */
int      CfRc_FlashHdr(uint32_t a, const uint8_t *b, uint32_t n);    /* the header area: a + n <= 0x7C0, FF bytes left alone */
int      CfRc_FlashCrc(uint32_t a, uint32_t n, uint32_t *crc, int *all_ff);
int      CfRc_FlashRead(uint32_t a, uint8_t *dst, uint32_t n);       /* FREAD, each KiB checked by the FPGA's crc32 */
const cf_fota_host_t *CfRc_FotaHost(void);              /* cf_fota.c on this unit's boot flash */
/* the GAME AREA (cf_fota.h: CF_FOTA_GAME_BASE 0x05C000 up to the full design's start, ~3.04 MB), for the firmware's game
   store: anything outside it is refused (-10). CfRc_GameArea: its place (0 = this boot flash has one); it is read from the
   header area once and again after an FPGA install (CfRc_GameAreaRescan). Erase: 4 KiB aligned (64 KiB blocks where
   they fit). Program: inside erased flash. They run through the FPGA (cart_xs ops over UART 0x12 + the QSPI ring /
   window): one caller at a time, not while a cart job or an FPGA update runs. */
int      CfRc_GameArea(uint32_t *base, uint32_t *size);
int      CfRc_LibSettle(uint32_t ms);   /* after a keep: the library task gone and the library closed (up to ms), the window buffer freed */
void     CfRc_GameAreaRescan(void);
int      CfRc_GameAreaErase(uint32_t a, uint32_t n);
int      CfRc_GameAreaProg(uint32_t a, const uint8_t *b, uint32_t n);
int      CfRc_GameAreaRead(uint32_t a, uint8_t *dst, uint32_t n);
int      CfRc_GameAreaCrc(uint32_t a, uint32_t n, uint32_t *crc, int *all_ff);
/* FCOPY: n (even) game area bytes at a straight into PSRAM at pa (even, 0x040000-0x7FFFFF; the RAM cart must be off),
   by the FPGA itself (~1 MB/s); *crc = zlib crc32 of the bytes it copied. 0 = done. Full design 18.31 only (the recovery
   design refuses it: 6). Copy into the scratch area (CF_RC_SCRATCH_*) or straight into the ROM area (CF_RC_ROM_BASE). */
int      CfRc_GameAreaCopy(uint32_t a, uint32_t pa, uint32_t n, uint32_t *crc);
int      CfRc_FlashCopy(uint32_t a, uint32_t pa, uint32_t n, uint32_t *crc);

/* lane gamecomp1: the game library on the FPGA's boot flash (cf_lib.c; SYSTEM > PLAY) */
typedef struct { uint32_t uid, rom_size, stored_size, save_size; uint8_t flags; char title[24]; uint32_t rom_crc32; } cf_lib_view_ent_t;   /* (rom_crc32: lane share1) */
int      CfRc_LibThere(void);                 /* -1 not known yet, 0 this unit has no game area, 1 it has */
void     CfRc_LibHold(int on);                /* SYSTEM > PLAY open: the library read and kept open */
int      CfRc_LibHeld(void);                             /* m110s: 1 = a page holds the library open (CfRc_LibHold) */
int      CfRc_LibCount(uint32_t *free_b, uint32_t *total_b, uint32_t *gen);   /* games; -1 no library, -2 being read */
int      CfRc_LibGet(int i, cf_lib_view_ent_t *o);
int      CfRc_LibBusy(void);
int      CfRc_LibResult(void);
int      CfRc_LibDelete(uint32_t uid);
void     CfRc_LibPlay(uint32_t uid);
int      CfRc_LibPlayingUid(void);
void     CfRc_PlayAfterStop(int card, uint32_t uid, const char *path);   /* lane ui110dev: the one playing stops (save kept), then this */
/* lane ui110dev: SYSTEM > GAMES's copies (async: CfRc_LibBusy / CfRc_LibResult): what 0 the kept game uid onto the card (its
   save too; CfRc_LibCopyPath: its file), 1 into the staging area (erased first; CfRc_LibCopyStaged: its size / crc32 / sha256),
   2 a save from the card (path) into game uid, 3 a save from this ESP32's flash (src) into game uid. Why (page words) on a failure. */
int      CfRc_LibCopy(int what, uint32_t uid, const char *path, uint32_t src);
const char *CfRc_LibCopyWhy(void);
int      CfRc_LibCopyPct(void);
const char *CfRc_LibCopyPath(void);
void     CfRc_LibCopyStaged(uint32_t *size, uint32_t *crc, uint8_t sha[32]);
int      CfRc_LibKeepStaged(uint32_t src, uint32_t len, uint32_t crc, const char *title, int play);
#include "cf_lib.h"
int      CfRc_LibPlanStream(uint32_t stored_size, const char *title, cf_lib_tx_t *tx);
int      CfRc_LibTxWrite(cf_lib_tx_t *tx, const uint8_t *b, uint32_t n);
int      CfRc_LibStreamEnd(cf_lib_tx_t *tx);  /* async (CfRc_LibBusy / CfRc_LibResult / CfRc_LibUid) */
uint32_t CfRc_LibUid(void);
void     CfRc_LibSummary(char *json, size_t n, const char **caps);

/* lane share1: Chromatic to Chromatic sharing (cf_share_dev.c) */
int      CfRc_RamRegion(uint32_t base, uint32_t max);    /* the PSRAM writer (CfRc_RamBegin / Put) moves to another region */
int      CfRc_ScratchRead(uint32_t off, uint8_t *b, uint32_t n);
int      CfRc_ScratchReadFast(uint32_t off, uint8_t *b, uint32_t n, uint32_t len);   /* timed windows: may be wrong now and then */
void     CfRc_ScratchFastDone(void);
int      CfRc_ScratchReadBegin(void);                  /* before reading scratch back: the RAM cart off, the core let go */
void     CfRc_RamRelease(void);                          /* PSRAM written for something that does not play: back to the cartridge */
void     CfRc_PlayScratch(uint32_t len, const char *title, int save_given);           /* an SGZ1 container in scratch: unpacked, played */
void     CfRc_PlayInRamSave(uint32_t size, uint32_t crc, const char *title, int save_given);   /* a raw game in the ROM area */
uint32_t CfRc_PlayDone(void);                            /* m110s: counts the plays the task has finished (started or not): a caller waits for it to move */
int      CfRc_LibShareOpen(uint32_t uid, cf_lib_ent_t *e, cf_lib_save_t *sv);
int      CfRc_LibShareRead(const cf_lib_ent_t *e, const cf_lib_save_t *sv, int save, uint32_t off, uint8_t *b, uint32_t n);
int      CfRc_LibSaveKnown(uint32_t uid);                /* lane savecloud1: 1 the kept game has a stored save, 0 none, -1 not known (asked) */
void     CfRc_LibSaveForget(void);
int      CfRc_LibNetClose(void);                         /* the library closed for a TLS handshake's heap (CfRc_LibHold(1) opens it again) */
int      CfRc_LibSavePutFromRam(uint32_t uid);           /* async: CfRc_LibBusy / CfRc_LibResult */
void     CfRc_SaveComesWith(int on);                     /* lane mygames1: the next keep / play / stream end: the save RAM holds its save */
uint32_t CfRc_LibRoomNeed(uint32_t stored_size, uint32_t save_size);
int      CfRc_LibHasGame(uint32_t rom_crc32, uint32_t rom_size);   /* 1 = this game is ON CHROMATIC already (the last look at the store) */
