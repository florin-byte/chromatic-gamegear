#pragma once
/* The light-sleep timer wake choice (pwrmgr.c), pure so a Mac test reaches it (cartflash/host/cf_sleep_test.c). */
#include <stdint.h>

/* Two wishes for a timer wake (0 = none): the smaller non-zero one; 0 only when neither wants one. */
static inline uint32_t PwrMgr_PickTimerMs(uint32_t a_ms, uint32_t b_ms)
{
    if (a_ms == 0) return b_ms;
    if (b_ms == 0) return a_ms;
    return a_ms < b_ms ? a_ms : b_ms;
}
