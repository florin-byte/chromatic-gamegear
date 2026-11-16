#pragma once

// SYSTEM > LINK CABLE: the link cable mode (AUTO / NEARBY / ONLINE / OFF) and
// the nearby partner (pick list), plus the "LINKED" toast widget. Wi-Fi
// credentials stay console-set (`cwl wifi <ssid> <password>`).

#include "osd_shared.h"

OSD_Result_t WifiLink_Draw(void* arg);
OSD_Result_t WifiLink_OnButton(const Button_t Button, const ButtonState_t State, void *arg);
OSD_Result_t WifiLink_OnTransition(void* arg);
// Always-drawn widget (like the battery icon): the pairing toast.
OSD_Result_t LinkToast_Init(OSD_Widget_t *const pWidget);
