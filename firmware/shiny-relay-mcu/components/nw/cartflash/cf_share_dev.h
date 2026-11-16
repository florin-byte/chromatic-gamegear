/* cf_share_dev.h: Chromatic to Chromatic sharing on the unit (lane share1). The protocol is cf_share.c, the radio
 * components/cwl/cwl_share.c; this file owns a share's session: where A's game comes from (the CARTRIDGE read into the
 * staging area, an ON CHROMATIC game read from the FPGA flash's game store, a CARD file), where B puts it (ON CHROMATIC: the
 * store, kept as it came; CARD: a file, unpacked when it came packed; PLAY NOW: PSRAM, not kept), the task that runs it, and
 * what the screen shows (the OSD's share.c draws the view and passes the buttons). One share at a time, either side. */
#ifndef CF_SHARE_DEV_H
#define CF_SHARE_DEV_H
#include <stdint.h>

/* what the screen shows */
enum {
    CF_SHV_IDLE = 0,
    /* A, the sender */
    CF_SHV_A_CODE,       /* type B's code (+ WITH SAVE); a cartridge is read meanwhile (read_pct) */
    CF_SHV_A_READ,       /* A: SEND pressed, the cartridge still being read */
    CF_SHV_A_FIND,       /* looking for the Chromatic with that code */
    CF_SHV_A_ASKED,      /* found: it asks its person where to keep it */
    CF_SHV_A_SEND,       /* sending NN% */
    CF_SHV_A_END,        /* all sent: the other one checks and keeps it */
    CF_SHV_A_RESULT,
    /* B, the receiver */
    CF_SHV_B_START,      /* getting the radio ready */
    CF_SHV_B_CODE,       /* the code, waiting for a sender */
    CF_SHV_B_OFFER,      /* who sends what: where should it go? */
    CF_SHV_B_RECV,       /* receiving NN% */
    CF_SHV_B_KEEP,       /* all came: checked, being kept */
    CF_SHV_B_AFTER,      /* the radio off: unpacked onto the card / loaded to play (pct) */
    CF_SHV_B_RESULT,
};
enum { CF_SHB_A = 1, CF_SHB_B, CF_SHB_UP, CF_SHB_DOWN, CF_SHB_LEFT, CF_SHB_RIGHT };

typedef struct {
    int      kind;
    char     title[17];
    char     peer[16];                   /* "CHROMATIC 1A2B" */
    char     code[5];                    /* B: its code; A: the code typed so far */
    int      cur;                        /* A_CODE: the digit (0..3) or 4 = the WITH SAVE row */
    int      save_avail, with_save;      /* A: the game has a save to send; WITH SAVE chosen. B: one comes with it */
    int      read_pct;                   /* A_CODE / A_READ: the cartridge read, -1 not reading */
    int      pct;                        /* the transfer, or B_AFTER's step */
    uint32_t kbps;                       /* KB/s so far */
    int      paused;                     /* the other side not heard for a while: waiting (seconds) */
    uint32_t size, save_size;            /* bytes of the game (as it travels) and of the save */
    uint32_t rom_size;
    int      sel;                        /* B_OFFER: the place picked (CF_SH_DEST_*) */
    int      place_ok[3];                /* B_OFFER: 1 = possible */
    const char *place_why[3];            /* ... else why not ("NO  ROOM", "NO  CARD", ...) */
    int      dest;                       /* B: where it went */
    int      ok;                         /* *_RESULT: 1 = it worked */
    char     r1[24], r2[24];             /* *_RESULT: two rows of why / where */
} cf_share_view_t;

void CfShare_Register(void);             /* the console bench `share` */
int  CfShare_Active(void);               /* 1 = a share page is up (a share runs, or its result is shown) */
int  CfShare_Busy(void);                 /* 1 = the radio or a job runs (no light sleep, no update meanwhile) */
/* A: src CF_SH_SRC_*; uid (ON CHROMATIC), path (CARD); title as the row shows it. 0 = the code page is up */
int  CfShare_SendStart(int src, uint32_t uid, const char *path, const char *title);
int  CfShare_RecvStart(void);            /* B. 0 = started */
void CfShare_Button(int b);              /* CF_SHB_* */
void CfShare_View(cf_share_view_t *v);
void CfShare_Close(void);                /* the result seen (A: OK): the page goes */
#endif
