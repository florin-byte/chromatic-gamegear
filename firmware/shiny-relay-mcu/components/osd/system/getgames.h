#pragma once

// SYSTEM > GET GAMES (lane ui110dev): our free games, got on the Chromatic itself (getgames.c).

#include <stddef.h>
#include "osd_shared.h"

OSD_Result_t GetGames_Draw(void *arg);
OSD_Result_t GetGames_OnButton(const Button_t Button, const ButtonState_t State, void *arg);
OSD_Result_t GetGames_OnTransition(void *arg);
void GetGames_Bench(char *out, size_t n);
void GetGames_BenchButton(int b);
int  GetGames_ParsePage(const char *text, void *page);   /* (host tests) */
void GetGames_Register(void);
