// Mac test of cwl_wdt.c's pure core (0.17.2): which idle cores the task
// watchdog watches while long TLS sections run.  make -C components/cwl/host test
#include "cwl_wdt.h"

#include <stdio.h>

static int _Fails, _Checks;
#define CHECK(c) do { _Checks++; if (!(c)) { _Fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void)
{
    CwlWdtState_t s = {.BaseMask = 3};
    CHECK(CwlWdt_Enter(&s, 1) == 1);     // core 1 handshake: only IDLE0 watched
    CHECK(CwlWdt_Enter(&s, 1) == -1);    // nested on core 1: no change
    CHECK(CwlWdt_Enter(&s, 0) == 0);     // gateway handshake on core 0 too: none watched
    CHECK(CwlWdt_Leave(&s, 1) == -1);    // core 1 still has one open
    CHECK(CwlWdt_Leave(&s, 0) == 1);     // core 0 back: IDLE0
    CHECK(CwlWdt_Leave(&s, 1) == 3);     // all back
    CHECK(CwlWdt_Leave(&s, 1) == -1);    // unbalanced leave ignored
    CHECK(s.Depth[0] == 0 && s.Depth[1] == 0);
    CHECK(CwlWdt_Enter(&s, 2) == -1 && CwlWdt_Enter(&s, -1) == -1);   // bad core
    // a build that watches only IDLE0: core 1 sections never change the mask
    CwlWdtState_t t = {.BaseMask = 1};
    CHECK(CwlWdt_Enter(&t, 1) == -1);
    CHECK(CwlWdt_Enter(&t, 0) == 0);
    CHECK(CwlWdt_Leave(&t, 0) == 1);
    CHECK(CwlWdt_Leave(&t, 1) == -1);
    // depth saturates instead of wrapping
    CwlWdtState_t u = {.BaseMask = 3};
    for (int i = 0; i < 300; i++) (void)CwlWdt_Enter(&u, 0);
    CHECK(u.Depth[0] == 255);
    printf("wdt_test: %d checks, %d failed\n", _Checks, _Fails);
    return _Fails ? 1 : 0;
}
