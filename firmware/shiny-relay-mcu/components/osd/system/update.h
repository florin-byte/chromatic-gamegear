#pragma once

// SYSTEM > UPDATE (1.1.0): Shiny Relay's own update over Wi-Fi. Only in a build with the Wi-Fi
// install (CONFIG_NW_CARTFLASH_WIFI); the update machine (nw/cartflash/cf_ota.c) owns the page,
// this item draws it and passes A / B on.

#include "osd_shared.h"
#include "nw_dev.h"

#include <stdbool.h>

enum { kUpdateTextLen = 24, kUpdateRows = 7 };

OSD_Result_t Update_Draw(void *arg);
OSD_Result_t Update_OnButton(const Button_t Button, const ButtonState_t State, void *arg);
OSD_Result_t Update_OnTransition(void *arg);
// 1.1.12: the rows of a running flow (check, download, READY, result), shared with SYSTEM > CUSTOM (custom.c)
bool Update_FlowRows(const NwFwView_t *v, char S[4][kUpdateTextLen + 16], const char *R[kUpdateRows], bool W[kUpdateRows]);
