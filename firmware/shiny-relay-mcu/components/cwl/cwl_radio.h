// cwl_radio.h: the local-radio transport of CWL2 on the ESP32 (no router):
// ESP-NOW (cwl_espnow.c). Frames are cwl_lx.h frames; received frames go to
// the link task's queue. See PROTOCOL.md "Nearby (ESP-NOW)". (Bluetooth LE is
// parked: parked/cwl_ble.c, not built.)
#pragma once

#include "cwl_lx.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { kCwlMed_Ble = 1, kCwlMed_EspNow = 2 } CwlMedium_t;   // Ble: parked

// One received frame (queued by the radio's task, drained by the link task).
typedef struct {
    uint8_t medium;          // CwlMedium_t
    uint8_t len;
    uint8_t unicast;         // ESP-NOW: addressed to us (not broadcast)
    int8_t rssi;
    uint8_t src[6];
    uint8_t f[kCwlLx_MaxFrame + 8];   // we send at most 48; a little room for a newer peer
} CwlRadioRx_t;

// ---------------------------------------------------------------- ESP-NOW

typedef struct {
    bool on;
    uint8_t own[6];
    uint8_t channel;
    bool low_power;
    uint32_t tx_frames, tx_ok, tx_fail, tx_err, rx_frames, rx_drop;
    uint32_t start_ms;
} CwlEnStatus_t;

// Wi-Fi must be initialised and started in STA mode (not associated).
esp_err_t CwlEn_Start(QueueHandle_t rx, uint8_t channel);
void CwlEn_Stop(void);
bool CwlEn_On(void);
// dest all-FF = broadcast (no MAC ACK); unicast peers are added on demand.
bool CwlEn_Send(const uint8_t dest[6], const uint8_t *f, size_t n);
void CwlEn_SetLowPower(bool low);
// The task to notify (xTaskNotifyGive) after each received frame is queued.
void CwlEn_SetNotifyTask(TaskHandle_t Task);
void CwlEn_GetStatus(CwlEnStatus_t *st);

#ifdef __cplusplus
}
#endif
