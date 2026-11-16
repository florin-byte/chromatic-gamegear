// cwl_ble.h: PARKED (2026-10-10, owner: Wi-Fi only). Not compiled. See
// parked/README.md. Declarations for parked/cwl_ble.c.
#pragma once
#include "../cwl_radio.h"

// ---------------------------------------------------------------- BLE

// GATT service 43574C32-B1E0-4C49-4E4B-000000000000 ("CWL2" "LINK"):
//   ...0001  C2P  write / write without response: frames central -> peripheral
//   ...0002  P2C  notify: frames peripheral -> central
//   ...0003  STAT read: "CWB ..." BLE fields + the CWS? status line (<= 512 bytes)
typedef struct {
    bool up;                 // stack synced
    bool connected;
    bool ready;              // frames can flow (central: peer's chars found + subscribed;
                             // peripheral: the central subscribed to P2C)
    bool central;            // our role on the current connection
    bool advertising, scanning, initiating;
    uint8_t own[6], peer[6]; // LSB first (NimBLE order)
    uint16_t itvl;           // connection interval, 1.25 ms units
    uint16_t latency, timeout, mtu;
    int8_t rssi;
    bool low_power;
    uint32_t connects, disconnects, last_reason, adv_seen, adv_ignored;
    uint32_t tx_frames, tx_fail, rx_frames, rx_drop, rx_bad;
    uint32_t starts, start_ms;       // last start: init -> synced
    uint32_t connect_ms;             // last connect: search start -> ready
    uint32_t param_updates;
} CwlBleStatus_t;

// Call once at boot, before anything else allocates: keeps the BT controller
// memory (BLE only) or gives it all back to the heap (`keep` false: Bluetooth
// is then unavailable until the next boot).
void CwlBle_BootMemory(bool keep);
bool CwlBle_MemoryKept(void);

esp_err_t CwlBle_Start(QueueHandle_t rx, CwlLxRole_t role, const uint8_t *fixed_peer /* LSB first, NULL: any */);
void CwlBle_Stop(void);
bool CwlBle_On(void);        // started (maybe not synced yet)
bool CwlBle_Ready(void);
// Usable frame size (ATT MTU - 3, at most kCwlLx_MaxFrame).
unsigned CwlBle_FrameLimit(void);
bool CwlBle_Send(const uint8_t *f, size_t n);
// Link task, every pass: restarts advertising/scanning when idle, applies the
// power level (connection parameters).
void CwlBle_Poll(uint32_t now_ms, bool low_power);
void CwlBle_GetStatus(CwlBleStatus_t *st);
// Full-power connection parameters (console `cwl ble params`): interval
// min/max (1.25 ms units), latency, supervision timeout (10 ms units).
void CwlBle_SetParams(uint16_t itvl_min, uint16_t itvl_max, uint16_t latency, uint16_t timeout);
// What the status characteristic returns after the BLE fields.
void CwlBle_SetStatusFn(int (*fn)(char *buf, int len));
void CwlBle_AddrText(const uint8_t a[6], char *buf, unsigned len);   // "AA:BB:CC:DD:EE:FF" MSB first
bool CwlBle_ParseAddr(const char *s, uint8_t out[6]);

