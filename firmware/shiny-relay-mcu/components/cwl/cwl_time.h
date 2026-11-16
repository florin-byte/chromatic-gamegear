// cwl_time.h: the real time (1.1.7). The unit keeps no clock while it is off (the power switch cuts the ESP32's power), so a
// game's clock (MBC3: Pokemon Gold / Silver / Crystal) is caught up from the network: SNTP once Wi-Fi has joined, or the
// Date of any answer from our server. Nothing else in the unit sets the system clock.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// The system clock was set (after 2025-01-01).
bool CwlTime_IsSet(void);
// SNTP starts (once; later joins renew its servers by themselves). Called when Wi-Fi has an address.
void CwlTime_OnGotIp(void);
// The radio is wanted for the time: not set yet, and the first minute after power-on (or a game load asked for it).
bool CwlTime_Wants(void);
void CwlTime_Poll(void);                 // SNTP stopped once it has set the time (the Wi-Fi task calls this)
// A game is loading: wait up to ms for the time while Wi-Fi is joining. true = the time is set.
bool CwlTime_WaitForLoad(uint32_t ms);
// "Tue, 15 Nov 1994 08:12:31 GMT" (an HTTP Date header): sets the clock when it is not set yet. true = it was set by this call.
bool CwlTime_FromHttpDate(const char *date);
// The real time in unix seconds (UTC), 0 = not set. The unit's time is UTC throughout (SNTP and a server's Date are UTC; a time set by
// hand is UTC too), so the game clocks, which only need the DIFFERENCE between two times, are right whichever way the time came.
uint64_t CwlTime_Unix(void);
bool CwlTime_FromWifi(void);   // the time came from the network (SNTP or a server's Date)
bool CwlTime_ByHand(void);     // the time was set on the clock page and no network time has replaced it
// Set the time by hand (the clock page). false when the network already gave it (it wins), or the time is before 2025.
// A network time that arrives later replaces it.
bool CwlTime_SetByHand(uint64_t t);
// The last real time the unit knew, kept across power-offs (0 = never): where the clock page starts when there is no time.
uint64_t CwlTime_Last(void);
// The time to read a game's saved time against: now, else the last one known (0 = neither)
uint64_t CwlTime_Anchor(void);
// Bench: no real time for the game clocks and no radio for it (until unblocked or restarted); the system clock goes back to unset.
void CwlTime_Block(bool on);
bool CwlTime_Blocked(void);
// One line: set / source / blocked
void CwlTime_Print(void);
