// cwl_logic.h: small decisions of the ESP32 glue kept as pure functions so that the Mac
// tests (host/logic_test.c) pin them. No ESP-IDF types here.
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
    kCwlWake_LowWindow_ms = 50,     // connectionless modem sleep: awake 50 ms of every 100
    kCwlWake_LowInterval_ms = 100,
    kCwlWake_AlwaysWindow_ms = 65535,   // esp_now_set_wake_window: "default value is the maximum"
    kCwlWake_AlwaysInterval_ms = 0,     // ESP_WIFI_CONNECTIONLESS_INTERVAL_DEFAULT_MODE
};

// The connectionless wake schedule (ESP-NOW without an AP) for a power level. It must be set
// on EVERY change: until 0.17.10 only the low level set it, and full power only switched the
// power-save type to none. Without an AP the schedule is what the radio follows
// (CONFIG_ESP_WIFI_STA_DISCONNECTED_PM_ENABLE), so two units with no Wi-Fi network each
// listened 50 ms of every 100 at "full power": a third of the frames lost, hops of 16-66 ms
// (two units, 2026-10-11: 71 of 100 pings; with the schedule restored 100 of 100).
// The link task's outer loop, after it looked at what is wanted: may it block until someone
// notifies it? Not right after a session ended. A session ends because the mode or its
// generation changed (LINK CABLE in the OSD, `cwl on`, a test knob) or cwl_net took the
// radio, and the notification that announced it was already consumed by the session's own
// wait (since 0.17.4 the session waits on the task notification). Blocking then left the
// link with no session and the radio off until a link-port event: on 0.17.10, LINK CABLE
// OFF -> AUTO with the network service on showed STANDBY, ESP-NOW off, no path (two units,
// 2026-10-11). After a session the loop must look again at once.
static inline bool CwlLogic_TaskMayBlock(bool session_just_ended) { return !session_just_ended; }

// May the link task run a session (radio up, relay, ESP-NOW, the network window, pairing)?
// Not while cwl_net has the radio (scan, join, phone setup) and not while a cart job holds it:
// usb-cart-5 on an ONLINE unit (unit 2, 2026-10-12) refused every cart job with
// "busy memory", the associated radio and the online HTTPS traffic leaving under the job's
// 6 KiB floor. The cart hold is RAM only and is lifted by the cart code when the job is over;
// then the same rule lets the session start again by itself (no menu change).
static inline bool CwlLogic_LinkMayRun(bool link_can, bool net_can, bool net_hold, bool cart_hold)
{
    return (link_can || net_can) && !net_hold && !cart_hold;
}

static inline void CwlLogic_WakeSchedule(bool low, uint16_t *window_ms, uint16_t *interval_ms)
{
    *window_ms = low ? kCwlWake_LowWindow_ms : kCwlWake_AlwaysWindow_ms;
    *interval_ms = low ? kCwlWake_LowInterval_ms : kCwlWake_AlwaysInterval_ms;
}
