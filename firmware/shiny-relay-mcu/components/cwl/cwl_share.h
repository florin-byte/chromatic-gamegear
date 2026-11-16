// cwl_share.h: the radio for Chromatic to Chromatic game sharing (lane share1; the protocol: nw/cartflash/cf_share.h).
//
// While a share runs, the link session is held (CWL_NetHold: no game link, relay seat, LAN, or AP join) and this file
// runs the Wi-Fi radio on its own: station mode with no access point, one channel, ESP-NOW frames of up to 250 bytes.
// Received frames go into a queue (the share task drains it); sends are limited to a few frames in the air at once
// (each unicast frame is ACKed by the other radio, and retried by it).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t src[6];
    int8_t rssi;
    uint8_t len;
    uint8_t f[250];
} CwlShRx_t;

typedef struct {
    bool on;
    uint8_t channel, rate;
    uint32_t tx, tx_ok, tx_fail, tx_full, rx, rx_drop, rx_queue_low;
    int8_t last_rssi;
    uint32_t heap_at_start, heap_low;
    uint32_t lat_us, wait_ms, rate_downs;            // a frame's mean time from the hand-over to the radio's report; time waiting for room
} CwlShStats_t;

// queue_len frames are held for the share task. The link session ends first (up to ~4 s). ESP_OK = on.
// channel 0: the least busy of 1 / 6 / 11 (one scan, ~1.5 s: a receiver's choice; the sender sweeps)
esp_err_t CwlSh_Start(uint8_t channel, int queue_len);
uint8_t CwlSh_Channel(void);
void CwlSh_Stop(void);                       // ESP-NOW and Wi-Fi off, the link session the mode wants again
bool CwlSh_On(void);
// m110s: the link session held off before the radio starts (a share that could not get its task's heap: the link's
// Wi-Fi buffers back first). Off: released only while the radio is off (CwlSh_Stop releases its own hold).
void CwlSh_HoldLink(bool on);
bool CwlSh_LinkHeld(void);
// The channel a receiver listens on: the last access point's, else the link's own ESP-NOW channel (both units on the
// same home Wi-Fi then meet at once; the sender sweeps the others).
uint8_t CwlSh_HomeChannel(void);
void CwlSh_SetChannel(uint8_t channel);
// 1 = a frame in *out (waits up to wait_ms)
int CwlSh_Recv(CwlShRx_t *out, uint32_t wait_ms);
// 0 = handed to the radio; -1 = no room right now (too many in the air): try again shortly
int CwlSh_Send(const uint8_t dst[6], const uint8_t *f, size_t n);
void CwlSh_OwnMac(uint8_t mac[6]);
void CwlSh_GetStats(CwlShStats_t *st);
// the unicast PHY rate to start with (wifi_phy_rate_t; 0xFF = the driver's default, 1 Mbps; default 24 Mbps), stepping down
// to 11 and 1 Mbps while frames fail. Kept in RAM; `share rate N` on the console
void CwlSh_SetRate(uint8_t rate);
uint8_t CwlSh_Rate(void);
// bench: the radio neither hears nor sends for ms (an out-of-reach cut; 0 ends it)
void CwlSh_Mute(uint32_t ms);
// bench: frames in the air at once (1..32; the next share takes it)
void CwlSh_SetInAir(int n);
// ESP-NOW paused (its frame pool freed; Wi-Fi and the link hold kept) / resumed on the same channel with a 4-frame pool
void CwlSh_Pause(int on);
int CwlSh_InAir(void);

#ifdef __cplusplus
}
#endif
