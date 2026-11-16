#pragma once

// SYSTEM > ONLINE: Shiny Link internet play. Pairs the Chromatic with a Shiny
// Gen account (QR + code for shinygen.ai/link), shows the relay status and
// room, and edits the room (mine / host / join a friend's code), the path
// (auto / LAN / relay), force relay and unpair. See online.c.

#include "osd_shared.h"

OSD_Result_t Online_Draw(void* arg);
OSD_Result_t Online_OnButton(const Button_t Button, const ButtonState_t State, void *arg);
OSD_Result_t Online_OnTransition(void* arg);
