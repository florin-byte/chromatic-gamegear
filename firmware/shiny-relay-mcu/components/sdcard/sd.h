/* sd.h: lane sdcard1: the Chromatic's internal microSD card (Shiny Relay). See sd_card.c, sd_lib.c,
 * sd_file.c, sd.c. Every call may block on the card (never call from an ISR or the esp_timer task);
 * the OSD's calls (SdLib_Count / SdLib_Get / Sd_View) never wait behind a long job. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "ff.h"
#include "sd_core.h"

enum { kSdSt_None, kSdSt_Ready, kSdSt_ReadOnly, kSdSt_NoFs, kSdSt_Error, kSdSt_Busy };

void     Sd_Register(void);                 /* boot: the lock, the console command, the first look for a card */
void     Sd_CardInit(void);
void     Sd_Lock(void);
int      Sd_TryLock(uint32_t ms);           /* 1 = taken */
void     Sd_Unlock(void);
int      Sd_Mount(void);                    /* 0 mounted, 1 no card, -2 no volume, else an error */
void     Sd_Unmount(void);
/* fwshrink1: the card lent (unmounted) for its ~1.4 KB of heap during a load / a report / receive mode; Sd_TakeBack mounts
   it again when the lender is done (only if it was mounted when lent). Was: nothing mounted it again until SYSTEM > GAMES
   or a console command, so after a library play the unit said NO CARD (and receive mode offered no card). */
void     Sd_Lend(void);
int      Sd_Lent(void);
int      Sd_LentInfo(int *writable, int *games, uint32_t *total_mb, uint32_t *free_mb);   /* 1 = lent: what it was then */
int      Sd_TakeBack(void);                 /* 0 = asked for (or nothing lent), -1 the card's task is busy: again later */
void     Sd_BenchOff(int off);              /* bench: as if no card were in (until `sd on` or a restart) */
int      Sd_Present(void);                  /* 1 = mounted and answering (a gone card is unmounted) */
int      Sd_State(void);
int      Sd_Mounted(void);
int      Sd_Writable(void);
int      Sd_IsExfat(void);
uint32_t Sd_Generation(void);               /* +1 at every mount / unmount / format */
void     Sd_Space(uint32_t *total_mb, uint32_t *free_mb);
void     Sd_RefreshSpace(void);
uint32_t Sd_CardMb(void);
int      Sd_Format(int exfat, void (*pct)(int));
const char *Sd_StateWord(void);
int      Sd_FileSize(const char *path, uint32_t *size);
int      Sd_RawRead(uint32_t lba, void *buf, uint32_t count);
int      Sd_RawWrite(uint32_t lba, const void *buf, uint32_t count);

/* the library (sd_lib.c) */
void     SdLib_SetCore(int (*core)(void)); /* cached running slot: -2 pending, -1 all, 0 GB, 1 GG */
int      SdLib_Total(void);
int      SdLib_GetAll(uint32_t i, sdc_rec_t *rec);
int      SdLib_CorePending(void);           /* cached core has not been read yet */
int      SdLib_Count(void);                 /* games in the index, -1 none, -2 busy */
int      SdLib_Get(uint32_t i, sdc_rec_t *rec);
int      SdLib_Find(const char *path);
int      SdLib_Refresh(int force);          /* 0 unchanged, 1 rebuilt, < 0 failed */
int      SdLib_Building(void);              /* a rebuild's percent, else -1 */
void     SdLib_Forget(void);
/* fr1: the folder view (the GAMES library page); -2 = busy, try again */
int      SdLib_Folders(void);               /* folders in FOLDERS.IDX (0: no folder view, A-Z only) */
int      SdLib_Folder(uint32_t id, sdc_folder_t *f);
int      SdLib_FolderChild(uint32_t pos, uint32_t *id);   /* FORDER.IDX[pos]: a folder's id */
int      SdLib_DirGet(uint32_t pos, sdc_rec_t *rec);      /* LIBDIR.IDX[pos]: the games in folder order */
int      SdLib_RecentAdd(const char *path, const char *title);   /* a game played: first in RECENT.TXT */
int      SdLib_Recent(uint32_t k, char *path, size_t pn, char *title, size_t tn);   /* 0 the newest; -1 none */

/* crash-safe writes, reads (sd_file.c) */
typedef struct { FIL f; char dst[SDC_PATH_MAX + 8]; char tmp[SDC_PATH_MAX + 12]; int open; } sd_wr_t;
int      Sd_WriteBegin(sd_wr_t *w, const char *dst, uint32_t size_hint);
int      Sd_WriteAt(sd_wr_t *w, uint32_t off, const void *b, uint32_t n);
int      Sd_WriteCommit(sd_wr_t *w);
void     Sd_WriteAbort(sd_wr_t *w);
int      Sd_ReadOpen(FIL *f, const char *path, uint32_t *size);   /* 0 open, 1 no such file */
int      Sd_Read(FIL *f, uint32_t off, void *b, uint32_t n, uint32_t *got);
void     Sd_Close(FIL *f);
int      Sd_Delete(const char *path);
int      Sd_Exists(const char *path);
int      Sd_FreeName(const char *dir, const char *file, char *out, size_t n);
int      Sd_Rename(const char *from, const char *to);   /* lane mygames1: 0 = renamed */
int      Sd_Move(const char *from, const char *to);     /* fr3j: a file or folder moved, the target's folders made */

/* the card's work task (sd.c): started when asked, gone when idle */
enum { kSdOp_None, kSdOp_Mount, kSdOp_Refresh, kSdOp_Format, kSdOp_FormatExfat, kSdOp_Delete, kSdOp_Backup, kSdOp_ToCart, kSdOp_Job,
       kSdOp_Look };   /* 1.1.6: still there? (a card that went in: its games read) */
int      Sd_Ask(int op, const char *path);  /* 0 = taken (the task does it), -1 another op runs */
/* lane ui110dev: SYSTEM > GAMES's jobs between the places (the cart hooks' job(), on this task): one at a time */
enum { kSdJob_None, kSdJob_Backup,          /* the cartridge -> the card (game + save) */
       kSdJob_CardToCart, kSdJob_LibToCart,  /* a game -> the cartridge (kSdJf_BackupFirst: the cartridge to the card first) */
       kSdJob_CardToLib, kSdJob_CartToLib,   /* a game kept ON CHROMATIC (the library) */
       kSdJob_LibToCard,                     /* a kept game (and its save) copied onto the card */
       kSdJob_SaveToCloud };                 /* lane savecloud1: a kept game's newest save up to the account's My games (no card needed) */
#define kSdJf_BackupFirst 1
int      Sd_AskJob(int kind, const char *path, uint32_t uid, int flags, const char *title);   /* as Sd_Ask */
int      Sd_JobKind(void);                  /* the job running (or the last one) */
const char *Sd_JobTitle(void);              /* the game it is about */
const char *Sd_JobStep(void);               /* its step in words ("" none) */
void     Sd_JobSetStep(const char *words);  /* (the job, for the page) */
int      Sd_SetJobTitle(const char *title); /* (the page: a delete's title) */
int      Sd_Busy(void);                     /* the op running (kSdOp_None: none) */
typedef struct {
    int      st;                            /* kSdSt_* */
    int      games;                         /* -1 no index */
    uint32_t total_mb, free_mb;
    int      exfat;
    int      op, pct;                       /* the op running and its percent (-1 none) */
    char     result[48];                    /* the last op's words "ROW1|ROW2" ("" none) */
} sd_view_t;
void     Sd_View(sd_view_t *v);
void     Sd_ClearResult(void);
/* for recv/start: the card summary as JSON members (",\"sd\":{...}" or ""), and the caps suffix (",sd1" or "") */
void     Sd_RecvSummary(char *json, size_t n, const char **caps);
/* the unit's clock for the MBC3 footers (0 = not known) */
uint64_t Sd_UnixTime(void);
/* hooks the cart code fills in (nw/cartflash): the cart backup / install jobs run on the card's task */
typedef struct {
    int (*backup)(void (*pct)(int), char *res, size_t n);            /* the cart in the slot -> /GAMES + /SAVES */
    int (*to_cart)(const char *path, void (*pct)(int), char *res, size_t n);   /* a game on the card -> the cart */
    int (*cart_in)(void);                                            /* 1 = a cart in the slot */
    /* lane ui110dev: a GAMES job (kSdJob_*): path (a card game) or uid (a kept game); res "ROW1|ROW2"; 0 = done */
    int (*job)(int kind, const char *path, uint32_t uid, int flags, const char *title, void (*pct)(int), char *res, size_t n);
} sd_cart_hooks_t;
void     Sd_SetCartHooks(const sd_cart_hooks_t *h);
/* lane gamecomp1: the SGZ1 unpacker (nw/cartflash cf_sgz.c, set by the cart code): the container at ESP32 flash addr..addr+len
   -> put(u, rom_off, bytes, n) in order; 0 = all of it, every check made; *rom_size the game's length. NULL: none (no z1) */
typedef int (*sd_unpack_fn)(uint32_t addr, uint32_t len, int (*put)(void *u, uint32_t off, const uint8_t *b, uint32_t n), void *u, uint32_t *rom_size);
void     Sd_SetUnpack(sd_unpack_fn f);
int      Sd_CartIn(void);
