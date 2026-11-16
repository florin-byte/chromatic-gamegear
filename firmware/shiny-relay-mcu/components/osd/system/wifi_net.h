#pragma once

// SYSTEM > WI-FI: choose the network (scan, password keyboard, saved
// networks, phone setup). See wifi_net.c.

#include "osd_shared.h"

OSD_Result_t WifiNet_Draw(void *arg);
OSD_Result_t WifiNet_OnButton(const Button_t Button, const ButtonState_t State, void *arg);
OSD_Result_t WifiNet_OnTransition(void *arg);
