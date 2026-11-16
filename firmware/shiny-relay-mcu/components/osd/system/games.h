#pragma once

// SYSTEM > GAMES (lane sdcard1): the games on the microSD card, played with no cartridge (games.c).

#include <stddef.h>
#include "osd_shared.h"

OSD_Result_t Games_Draw(void *arg);
OSD_Result_t Games_OnButton(const Button_t Button, const ButtonState_t State, void *arg);
OSD_Result_t Games_OnTransition(void *arg);
void Games_Bench(char *out, size_t n);
void Games_BenchButton(int b);
void Games_Register(void);
// fr1: the GAMES tab (the same page; its library over the whole screen) and SELECT in the menu
OSD_Result_t GamesTab_Draw(void *arg);
OSD_Result_t GamesTab_OnButton(const Button_t Button, const ButtonState_t State, void *arg);
OSD_Result_t GamesTab_OnTransition(void *arg);
void Games_OpenLibrary(void);

OSD_Result_t Core_Draw(void *arg);
OSD_Result_t Core_OnButton(const Button_t Button, const ButtonState_t State, void *arg);
