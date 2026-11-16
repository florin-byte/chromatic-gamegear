/* cf_sleep_test.c (lane savefix1): Mac test of the light-sleep timer decision (main/pwrmgr_logic.h, ../cf_rc_sleep.h; the SAME
 * code pwrmgr.c and cf_rc_dev.c use). Bug measured on a unit: a game plays from RAM, Wi-Fi dormant, the MCU sleeps with no timer
 * wake (116 s of 180 asleep, 0 FPGA wakes), the save poll in rc_task is frozen, and a save stays in PSRAM only.
 *   - a playing game puts a timer wake of about 1 s on light sleep, even when the relay seat keeper (cwl) wants none;
 *   - the smaller non-zero of the two wishes wins; none wanted = no timer;
 *   - a save copy pending keeps the unit awake (no sleep between the first dirty sighting and the copy done);
 *   - after a timer wake the poll is overdue by the clock that runs through sleep: the unit stays awake until it ran. */
#include <stdio.h>
#include <stdint.h>

#include "pwrmgr_logic.h"
#include "cf_rc_sleep.h"

static int pass, fail;
#define CHECK(c, ...) do { if (c) pass++; else { fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* the sleep entry as pwrmgr.c computes it: cwl's wish (CWL_SleepTimerMs) and the RAM cart's */
static uint32_t timer_for(uint32_t cwl_ms, int playing) { return PwrMgr_PickTimerMs(cwl_ms, cf_rc_sleep_timer_ms(playing)); }

int main(void)
{
    /* the bug: playing from RAM, Wi-Fi dormant with no seat keeper timer: asleep with no timer wake */
    CHECK(timer_for(0, 1) > 0, "a playing game: a timer wake (got %u ms)", (unsigned)timer_for(0, 1));
    CHECK(timer_for(0, 1) == 1000, "a playing game: about every 1000 ms (got %u)", (unsigned)timer_for(0, 1));
    CHECK(timer_for(0, 0) == 0, "nothing plays, no seat keeper: no timer (stock)");
    /* the seat keeper alone is untouched */
    CHECK(timer_for(240000, 0) == 240000, "seat keeper alone: its own wake (got %u)", (unsigned)timer_for(240000, 0));
    /* both: the smaller non-zero */
    CHECK(timer_for(240000, 1) == 1000, "seat keeper later than the poll: the poll (got %u)", (unsigned)timer_for(240000, 1));
    CHECK(timer_for(500, 1) == 500, "seat keeper sooner than the poll: the seat keeper (got %u)", (unsigned)timer_for(500, 1));
    CHECK(timer_for(1000, 1) == 1000, "equal: that");

    /* a save copy pending keeps the unit awake, between the first dirty sighting and the copy done */
    CHECK(cf_rc_keep_awake(0, 0, 0, 1, 1, 0), "pend: awake (just polled)");
    CHECK(cf_rc_keep_awake(0, 0, 0, 1, 1, 400), "pend: awake (mid-wait)");
    CHECK(!cf_rc_keep_awake(0, 0, 0, 0, 1, 0), "playing, nothing pending, just polled: may sleep");
    CHECK(!cf_rc_keep_awake(0, 0, 0, 0, 0, 999999), "no game: may sleep however old the poll");
    /* the earlier reasons still hold */
    CHECK(cf_rc_keep_awake(1, 0, 0, 0, 0, 0), "loading: awake");
    CHECK(cf_rc_keep_awake(0, 1, 0, 0, 0, 0), "a request: awake");
    CHECK(cf_rc_keep_awake(0, 0, 1, 0, 0, 0), "a library job: awake");

    /* a timer wake: the unit slept the timer's span since its last poll, so the poll is overdue and the unit stays awake
       (the idle timer would otherwise put it straight back to sleep; rc_task's own delay is tick-based and frozen asleep) */
    const uint32_t span = timer_for(0, 1);
    CHECK(cf_rc_keep_awake(0, 0, 0, 0, 1, span + 100), "after a timer wake: awake until the poll ran");
    CHECK(!cf_rc_keep_awake(0, 0, 0, 0, 1, 0), "poll ran: may sleep again");

    printf("cf_sleep_test: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
