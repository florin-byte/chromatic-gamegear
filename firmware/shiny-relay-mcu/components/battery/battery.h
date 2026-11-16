#pragma once

#include "osd_shared.h"
#include <stdint.h>
#include <stdbool.h>

typedef enum BatteryKind {
    kBatteryKind_AA,
    kBatteryKind_LiPo,
    kNumBatteryKind,
} BatteryKind_t;

typedef enum LiPoChargeStatus {
    kLipoChargeStatus_Disabled,
    kLipoChargeStatus_Enabled,
} LipoChargeStatus_t;

OSD_Result_t Battery_Init(OSD_Widget_t *const pWidget);
void Battery_UpdateVoltage(const float batt_V, const BatteryKind_t eKind);
void Battery_SetChargingStatus(const bool IsCharging);

// lane cartflash1: the state the cart flash power check reads. Level_V is the displayed
// (filtered) level, 0 until the first valid reading after the boot settling time.
typedef struct BatteryState {
    float Level_V;
    BatteryKind_t Kind;
    bool Charging;
    bool Valid;
    float Mid_V;      // this kind's "mid" mark (AA 3.15 V, LiPo 3.40 V)
    float Low_V;      // this kind's "low" mark, where the icon turns red (AA 2.80 V, LiPo 3.10 V)
    bool NoBattery;   // settled, and the rail reads below 2.0 V: the unit cannot be running on it (USB power, no batteries)
} BatteryState_t;
void Battery_GetState(BatteryState_t *Out);
