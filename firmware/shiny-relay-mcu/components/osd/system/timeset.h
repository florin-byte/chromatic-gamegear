#pragma once

#include "osd_shared.h"

OSD_Result_t TimeSet_Draw(void *arg);
OSD_Result_t TimeSet_OnButton(const Button_t Button, const ButtonState_t State, void *arg);
OSD_Result_t TimeSet_OnTransition(void *arg);
