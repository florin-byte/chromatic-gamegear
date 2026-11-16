// Mac test of cwl_logic.h (the ESP32 glue's pure decisions).
#include "cwl_logic.h"

#include <stdio.h>

static int checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { failed++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main(void)
{
    uint16_t w = 1, i = 1;
    // low power without an AP: the half-time schedule
    CwlLogic_WakeSchedule(true, &w, &i);
    CHECK(w == 50 && i == 100);
    // full power: always awake again (the schedule must not survive from low)
    CwlLogic_WakeSchedule(false, &w, &i);
    CHECK(w == 65535);
    CHECK(i == 0);
    // low -> full -> low -> full never leaves a window shorter than the interval at full
    for (int k = 0; k < 4; k++) {
        CwlLogic_WakeSchedule(k % 2 == 0, &w, &i);
        if (k % 2 == 1) CHECK(i == 0 || w >= i);
    }
    // the link task: never block right after a session ended; block when nothing ran
    CHECK(!CwlLogic_TaskMayBlock(true));
    CHECK(CwlLogic_TaskMayBlock(false));
    // a cart job holds the link (online session open when it starts): no session while held,
    // the same session again once released, whatever the mode wanted (online or nearby)
    CHECK(!CwlLogic_LinkMayRun(true, true, false, true));    // ONLINE, network wanted, held
    CHECK(!CwlLogic_LinkMayRun(true, false, false, true));   // NEARBY, held
    CHECK(!CwlLogic_LinkMayRun(false, true, false, true));   // OFF but the network window wanted, held
    CHECK(CwlLogic_LinkMayRun(true, true, false, false));    // released: ONLINE comes back
    CHECK(CwlLogic_LinkMayRun(true, false, false, false));   // released: NEARBY comes back
    CHECK(!CwlLogic_LinkMayRun(false, false, false, false)); // OFF stays off
    CHECK(!CwlLogic_LinkMayRun(true, true, true, false));    // cwl_net's hold as before
    printf("logic_test: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
