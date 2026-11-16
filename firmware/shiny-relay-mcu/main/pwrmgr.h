#pragma once

#include <stdbool.h>
#include <stdint.h>

// Keep the MCU awake for a bounded operation, without changing the console window.
void PwrMgr_KeepAwakeFor(uint32_t Duration_ms);

void PwrMgr_Task(void *arg);
void PwrMgr_TriggerLightSleep(void);
void PwrMgr_IdleTimerPet(void);
bool PwrMgr_IsLPMActive(void);
void PwrMgr_SetLPM(const bool Active);
void PwrMgr_RegisterCommands(void);
