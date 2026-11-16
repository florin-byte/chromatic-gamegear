#pragma once

// SYSTEM > NETWORK: switches the network window service on/off (NVS "nw"
// namespace): with it off, games see the window without a service (SVC = 0)
// and play offline. The gateway is console-set (`nw url <base>`).

#include "osd_shared.h"

OSD_Result_t Network_Draw(void* arg);
OSD_Result_t Network_OnButton(const Button_t Button, const ButtonState_t State, void *arg);
OSD_Result_t Network_OnTransition(void* arg);

// The game asked for its room menu (lk_ask, lane chromshlink1): it opens on
// the next draw, in editing mode.
void Network_RequestRoomMenu(void);
