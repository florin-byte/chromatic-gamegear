// Task watchdog and long TLS handshakes (0.17.2, lane fwux1).
//
// A TLS handshake to api.shinyrelay.com is ~6 s of ECC math on a 160 MHz
// ESP32 (three P-384 ECDSA verifications for the Let's Encrypt YE1 / Root YE /
// ISRG Root X2 chain, X25519, the P-256 ServerKeyExchange signature). mbedTLS
// never blocks or yields inside it, so the core's IDLE task cannot run for
// those 6 s and the task watchdog (5 s, idle tasks of both cores watched)
// fired: the "Backtrace:" dumps on unit 2 during pairing polls and ticket
// fetches (cwl_online, core 1) - and the gateway's TLS (nw_http, core 0)
// would do the same on core 0.
//
// Lowering the TLS task to idle priority would halve its speed (the IDF idle
// task waits for an interrupt in every slice it gets), so instead the core's
// idle task is taken off the watchdog ONLY for the handshake (the
// esp_http_client_open call) and put back right after, through the official
// esp_task_wdt_reconfigure. The other core stays watched; nested / concurrent
// sections are counted per core.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Once at boot, before the tasks that use it start (CwlOnline_Init).
void CwlWdt_Init(void);
// Enter / leave a long CPU-bound section on the calling task's core. Begin
// returns that core and End takes it back: an unpinned caller (the console
// task running `nw http`) may move to the other core in between, and End
// must release the core Begin took. Always pair them.
int CwlWdt_LongBegin(void);
void CwlWdt_LongEnd(int Core);

// ---- pure core (host-tested): the idle-core mask for a given state
typedef struct {
    uint8_t Depth[2];      // open sections per core
    uint32_t BaseMask;     // idle cores the build watches (CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPUx)
} CwlWdtState_t;
// Returns the new idle-core mask, or -1 when it does not change.
int32_t CwlWdt_Enter(CwlWdtState_t *S, int Core);
int32_t CwlWdt_Leave(CwlWdtState_t *S, int Core);   // an unbalanced leave is ignored

#ifdef __cplusplus
}
#endif
