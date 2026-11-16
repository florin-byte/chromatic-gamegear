// cwl_espnow.c: CWL2 over ESP-NOW (Wi-Fi radio, no access point). See
// cwl_radio.h, PROTOCOL.md "Local radios".
//
// Frames are cwl_lx frames, one per ESP-NOW action frame. Unlocked, a side
// broadcasts (no MAC ACK); once cwl_lx's peer lock has a partner it unicasts
// (MAC ACK + hardware retries). Both sides must use the same channel
// (console `cwl espnow channel N`, default 1).
#include "cwl_radio.h"
#include "cwl_logic.h"

#include "esp_log.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include <string.h>

static const char *TAG = "CWLEN";
static const uint8_t kBroadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

enum {
    kMaxUnicastPeers = 4,      // the partner, plus a few recent candidates
};

static volatile bool _On = false;
static QueueHandle_t _Rx;
static CwlEnStatus_t _St;
static uint8_t _Peers[kMaxUnicastPeers][6];
static int _NumPeers = 0, _NextEvict = 0;
static TaskHandle_t _Notify = NULL;   // woken on each received frame (the link task)

void CwlEn_SetNotifyTask(TaskHandle_t Task) { _Notify = Task; }

static uint32_t NowMs(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void OnRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!_On || info == NULL || data == NULL) return;
    CwlRadioRx_t r = {.medium = kCwlMed_EspNow};
    if (len <= 0 || len > (int)sizeof(r.f)) { _St.rx_drop++; return; }
    memcpy(r.src, info->src_addr, 6);
    r.unicast = info->des_addr != NULL && memcmp(info->des_addr, kBroadcast, 6) != 0;
    r.rssi = info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : 0;
    r.len = (uint8_t)len;
    memcpy(r.f, data, (size_t)len);
    _St.rx_frames++;
    if (_Rx == NULL || xQueueSend(_Rx, &r, 0) != pdTRUE) _St.rx_drop++;
    else if (_Notify != NULL) xTaskNotifyGive(_Notify);
}

static void OnSent(const uint8_t *mac, esp_now_send_status_t status)
{
    (void)mac;
    if (status == ESP_NOW_SEND_SUCCESS) _St.tx_ok++;
    else _St.tx_fail++;
}

static esp_err_t AddPeer(const uint8_t mac[6])
{
    if (esp_now_is_peer_exist(mac)) return ESP_OK;
    if (memcmp(mac, kBroadcast, 6) != 0) {
        if (_NumPeers >= kMaxUnicastPeers) {
            (void)esp_now_del_peer(_Peers[_NextEvict]);
            memcpy(_Peers[_NextEvict], mac, 6);
            _NextEvict = (_NextEvict + 1) % kMaxUnicastPeers;
        } else {
            memcpy(_Peers[_NumPeers++], mac, 6);
        }
    }
    esp_now_peer_info_t p = {0};
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0;              // the current channel
    p.ifidx = WIFI_IF_STA;
    p.encrypt = false;
    return esp_now_add_peer(&p);
}

esp_err_t CwlEn_Start(QueueHandle_t rx, uint8_t channel)
{
    if (_On) return ESP_OK;
    _Rx = rx;
    _NumPeers = _NextEvict = 0;
    if (channel < 1 || channel > 13) channel = 1;
    esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) ESP_LOGW(TAG, "set_channel %d", err);
    err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init %d", err);
        return err;
    }
    (void)esp_now_register_recv_cb(OnRecv);
    (void)esp_now_register_send_cb(OnSent);
    err = AddPeer(kBroadcast);
    if (err != ESP_OK) {
        (void)esp_now_deinit();
        return err;
    }
    (void)esp_wifi_get_mac(WIFI_IF_STA, _St.own);
    _St.channel = channel;
    _St.start_ms = NowMs();
    _St.low_power = false;
    (void)esp_now_set_wake_window(kCwlWake_AlwaysWindow_ms);   // a schedule left by an earlier start must not survive
    (void)esp_wifi_connectionless_module_set_wake_interval(kCwlWake_AlwaysInterval_ms);
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    _On = true;
    _St.on = true;
    return ESP_OK;
}

void CwlEn_Stop(void)
{
    if (!_On) return;
    _On = false;
    _St.on = false;
    (void)esp_now_unregister_recv_cb();
    (void)esp_now_unregister_send_cb();
    (void)esp_now_deinit();
}

bool CwlEn_On(void) { return _On; }

bool CwlEn_Send(const uint8_t dest[6], const uint8_t *f, size_t n)
{
    if (!_On || n == 0 || n > ESP_NOW_MAX_DATA_LEN) return false;
    if (AddPeer(dest) != ESP_OK) { _St.tx_err++; return false; }
    const esp_err_t err = esp_now_send(dest, f, n);
    if (err == ESP_OK) _St.tx_frames++;
    else _St.tx_err++;
    return err == ESP_OK;
}

void CwlEn_SetLowPower(bool low)
{
    if (!_On || low == _St.low_power) return;
    _St.low_power = low;
    // Connectionless modem sleep (IDF 5.x): at low power the radio wakes for a
    // window every interval and the partner's resends (every 20 ms) land in it;
    // at full power it must be told to stay awake again (cwl_logic.h).
    uint16_t window, interval;
    CwlLogic_WakeSchedule(low, &window, &interval);
    (void)esp_now_set_wake_window(window);
    (void)esp_wifi_connectionless_module_set_wake_interval(interval);
    (void)esp_wifi_set_ps(low ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
}

void CwlEn_GetStatus(CwlEnStatus_t *st)
{
    *st = _St;
    uint8_t ch = 0;
    wifi_second_chan_t sec;
    if (_On && esp_wifi_get_channel(&ch, &sec) == ESP_OK && ch) st->channel = ch;   // the AP's once joined
}
