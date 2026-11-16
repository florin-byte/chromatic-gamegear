#pragma once

// SHARE / RECEIVE FROM A CHROMATIC (lane share1): one Chromatic sends a game straight to another nearby one
// (nw/cartflash/cf_share_dev.c owns the session; this file turns its view into the panel's seven rows).
//
// The small action table the GAMES screens hook into (until lane ui110dev's one GAMES list lands, today's pages call it):
//   Share_SendStart(src, ...)   a SHARE action on a game row: src kShareSrc_Cart / _OnChromatic / _Card
//   Share_RecvStart(...)        RECEIVE FROM A CHROMATIC (SYSTEM > DOWNLOAD, B)
//   Share_Rows(&rows)           true while a share's page is up: the host page draws these rows instead of its own
//   Share_HostLeft()            the host page went away (the share goes on; SYSTEM > DOWNLOAD shows it again; a finished
//                               result already seen, or a PLAY NOW that plays, is not shown again: m110s)
// The share takes every button (the code's digits, the place) while its page is up and gives them back to `back` (the
// host's own capture, or none) when its result is seen.

#include <stdbool.h>
#include <stdint.h>
#include "osd_shared.h"

typedef enum { kShareSrc_Cart = 0, kShareSrc_OnChromatic = 1, kShareSrc_Card = 2 } ShareSrc_t;

enum { kShareRows = 7, kShareRowLen = 32 };
typedef struct {
    char T[kShareRows][kShareRowLen];
    uint8_t White;                 // bit i: row i white
    uint8_t Mode[kShareRows];      // 0 as is, 1 cut with dots, 2 scrolling (a game's name)
} ShareRows_t;

bool Share_Available(void);        // this build shares (the Wi-Fi radio)
// m110s: why a share did not start (the page that asked says so: before, a SHARE that could not start did nothing at all)
enum { kShare_Ok = 0, kShare_Busy = 1, kShare_NoMemory = 2 };
int Share_SendStart(ShareSrc_t Src, uint32_t Uid, const char *Path, const char *Title, fnOSDButtonCb_t Back);
int Share_RecvStart(fnOSDButtonCb_t Back);
bool Share_Rows(ShareRows_t *Out);
void Share_HostLeft(void);
