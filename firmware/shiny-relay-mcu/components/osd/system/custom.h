#pragma once

// SYSTEM > CUSTOM (1.1.12): custom firmware made at shinyrelay.com/custom-firmware. Not linked to an
// account: the page links this Chromatic (the pairing SYSTEM > ONLINE makes, shown as a code to type on
// the website). Linked: A asks the server for the build the account sent to this Chromatic and shows it
// (BUILD WAITING, A: DOWNLOAD, then A: INSTALL); on a custom build, B goes back to the official release.
// The update machine (nw/cartflash/cf_ota.c, its second channel) does the work; UPDATE stays official only.
// Only in a build with the Wi-Fi install (CONFIG_NW_CARTFLASH_WIFI).

#include "osd_shared.h"

OSD_Result_t Custom_Draw(void *arg);
OSD_Result_t Custom_OnButton(const Button_t Button, const ButtonState_t State, void *arg);
OSD_Result_t Custom_OnTransition(void *arg);
void Custom_Register(void);   // the bench console command `cpage`
