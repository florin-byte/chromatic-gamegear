#pragma once

// SYSTEM > PLAY (1.1.0, FPGA 18.30): the game kept in this Chromatic, played with no cartridge
// (nw/cartflash/cf_rc_dev.c owns the state, NW_PlayView / NW_PlayButton); this item draws it.

#include "osd_shared.h"

OSD_Result_t Play_Draw(void *arg);
OSD_Result_t Play_OnButton(const Button_t Button, const ButtonState_t State, void *arg);
OSD_Result_t Play_OnTransition(void *arg);
void Play_Register(void);   // lane gamecomp1: the bench console `ppage`
