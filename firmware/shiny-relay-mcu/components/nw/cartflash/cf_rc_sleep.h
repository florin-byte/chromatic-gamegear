#pragma once
/* The RAM cart's say in light sleep (cf_rc_dev.c, pwrmgr.c), pure so a Mac test reaches it (host/cf_sleep_test.c).
   A game plays from PSRAM with Wi-Fi dormant and nothing wakes the unit but the FPGA's UART: the save write-back poll
   (rc_task: the FPGA dirty flag every 250 ms, a copy 1.5 s after the last write) was frozen while it slept, and a save
   stayed in PSRAM only. */
#include <stdint.h>

enum {
    kCfRcSleep_PollMs = 1000,   /* a playing game: the unit wakes on its own this often, so the save poll runs */
};

/* The timer wake the RAM cart wants (0 = none: nothing plays). */
static inline uint32_t cf_rc_sleep_timer_ms(int playing)
{
    return playing ? (uint32_t)kCfRcSleep_PollMs : 0u;
}

/* No light sleep now: a load / a request / a library job (fwshrink1), a save copy pending (the first dirty sighting to the
   copy done), or the poll overdue by the clock that runs through sleep (a timer wake lands here, so the task polls before
   the unit sleeps again: its own 250 ms delay is tick-based and ticks do not advance asleep). */
static inline int cf_rc_keep_awake(int loading, int req, int lib_busy, int pend, int playing, uint32_t poll_age_ms)
{
    return loading || req != 0 || lib_busy || pend || (playing && poll_age_ms >= (uint32_t)kCfRcSleep_PollMs);
}
