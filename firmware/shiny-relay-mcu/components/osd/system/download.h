#pragma once

// SYSTEM > DOWNLOAD (lane wifirecv1): RECEIVE GAME, "Send to my Chromatic" by a short code.
// Only in a build with the Wi-Fi install (CONFIG_NW_CARTFLASH_WIFI); the install machine
// (nw/cartflash/cf_install.c) owns the page, this item draws it and passes A / B on.

#include "osd_shared.h"

OSD_Result_t Download_Draw(void *arg);
OSD_Result_t Download_OnButton(const Button_t Button, const ButtonState_t State, void *arg);
OSD_Result_t Download_OnTransition(void *arg);
