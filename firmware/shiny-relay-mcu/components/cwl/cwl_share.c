// cwl_share.c: the radio for Chromatic to Chromatic game sharing (lane share1). See cwl_share.h.
#include "cwl_share.h"
#include "cwl_priv.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <string.h>

static const uint8_t kAll[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static int _InAir = 12;              // frames handed to the radio and not yet reported sent (`share inair N`; 12 measured best, NOTES)

static volatile bool _On;
static QueueHandle_t _Q, _Free;          // frames in (pointers) / buffers free: a pool in chunks of 4 (a heap in pieces: no 6 KB block)
enum { kChunk = 4, kMaxChunks = 8 };
static CwlShRx_t *_Chunk[kMaxChunks];
static SemaphoreHandle_t _Slots;
static CwlShStats_t _St;
static uint8_t _Rate = WIFI_PHY_RATE_24M;     // units 1 + 2, a quiet channel: 24 Mbps 247-269 KB/s, 54 Mbps 260, 11 Mbps 216 (NOTES); `share rate`
static uint8_t _RateNow;                       // the rate in use: steps down (24 -> 11 -> 1 Mbps) when frames fail, back up when they go
static volatile uint16_t _FailRun, _OkRun;
static volatile bool _RateStep;
static uint8_t _Peer[6];
static bool _PeerAdded;
static volatile int64_t _MuteUntil;
static int64_t _SentAt[32];              // when each frame in the air was handed over (callbacks come in order)
static volatile uint32_t _SendSeq, _DoneSeq;
static volatile int64_t _LatSum, _WaitSum;      // bench: nothing in or out until then (an out-of-reach cut)

static void OnRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!_On || info == NULL || data == NULL || len <= 0 || len > 250) return;
    if (len < 12 || data[0] != 'S' || data[1] != 'h') return;   // (only ours: a nearby link's frames are not queued)
    if (_MuteUntil && esp_timer_get_time() < _MuteUntil) { _St.rx_drop++; return; }
    CwlShRx_t *r = NULL;
    if (xQueueReceive(_Free, &r, 0) != pdTRUE || !r) { _St.rx_drop++; return; }
    memcpy(r->src, info->src_addr, 6);
    r->rssi = info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : 0;
    r->len = (uint8_t)len;
    memcpy(r->f, data, (size_t)len);
    _St.rx++;
    _St.last_rssi = r->rssi;
    if (xQueueSend(_Q, &r, 0) != pdTRUE) { _St.rx_drop++; (void)xQueueSend(_Free, &r, 0); }
}

static void OnSent(const uint8_t *mac, esp_now_send_status_t status)
{
    (void)mac;
    if (status == ESP_NOW_SEND_SUCCESS) { _St.tx_ok++; _FailRun = 0; if (_OkRun < 60000) _OkRun++; }
    else { _St.tx_fail++; _OkRun = 0; if (++_FailRun >= 6) _RateStep = true; }   // (the radio retried each already: far apart)
    if (_DoneSeq != _SendSeq) { _LatSum += esp_timer_get_time() - _SentAt[_DoneSeq & 31u]; _DoneSeq++; }
    if (_Slots) (void)xSemaphoreGive(_Slots);
}

static void SetPeerRate(const uint8_t mac[6], uint8_t rate)
{
    if (rate == 0xFF) return;
    esp_now_rate_config_t rc = { 0 };
    rc.phymode = (rate >= WIFI_PHY_RATE_48M && rate <= WIFI_PHY_RATE_9M) ? WIFI_PHY_MODE_11G : WIFI_PHY_MODE_11B;
    if (rate >= WIFI_PHY_RATE_MCS0_LGI) rc.phymode = WIFI_PHY_MODE_HT20;
    rc.rate = (wifi_phy_rate_t)rate;
    if (esp_now_set_peer_rate_config(mac, &rc) != ESP_OK) printf("share: radio: rate %u refused, the default rate\n", rate);
}
// frames failing (the radio retried each): one step down; 2000 in a row that went: one step up again (never above _Rate)
static void RateAdapt(void)
{
    static const uint8_t kSteps[3] = { WIFI_PHY_RATE_24M, WIFI_PHY_RATE_11M_S, WIFI_PHY_RATE_1M_L };
    if (!_PeerAdded || _Rate == 0xFF) return;
    int i = 0;
    while (i < 3 && kSteps[i] != _RateNow) i++;
    if (_RateStep) {
        _RateStep = false; _FailRun = 0;
        if (i != 2) { _RateNow = i == 3 ? WIFI_PHY_RATE_11M_S : kSteps[i + 1]; SetPeerRate(_Peer, _RateNow); _St.rate_downs++; printf("share: radio: frames failing: rate %u\n", _RateNow); }
    } else if (_OkRun >= 2000 && _RateNow != _Rate && i > 0 && i < 3) {
        _OkRun = 0; _RateNow = kSteps[i - 1]; SetPeerRate(_Peer, _RateNow); printf("share: radio: frames going again: rate %u\n", _RateNow);
    }
}

static esp_err_t AddPeer(const uint8_t mac[6])
{
    if (esp_now_is_peer_exist(mac)) return ESP_OK;
    if (memcmp(mac, kAll, 6) != 0 && _PeerAdded && memcmp(mac, _Peer, 6) != 0) { (void)esp_now_del_peer(_Peer); _PeerAdded = false; }
    esp_now_peer_info_t p = { 0 };
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0;
    p.ifidx = WIFI_IF_STA;
    p.encrypt = false;
    esp_err_t e = esp_now_add_peer(&p);
    if (e == ESP_OK && memcmp(mac, kAll, 6) != 0) {
        memcpy(_Peer, mac, 6);
        _PeerAdded = true;
        _RateNow = _Rate;
        SetPeerRate(mac, _RateNow);
    }
    return e;
}

uint8_t CwlSh_HomeChannel(void) { return CWL_ShareChannel(); }

// the least busy of channels 1, 6 and 11, measured: on each, 24 broadcast frames of 250 bytes sent one at a time, each timed
// from the hand-over to the radio's report (a busy channel makes the radio wait for air). Units 1 + 2, 2026-10-24: on channel
// 6 (the home network's) ~55 KB/s whatever the rate, on channel 1 / 11: 224-301 KB/s. A scan's signal levels did not tell
// them apart (three scans chose 1, 11, 6). ~0.3 s.
static uint8_t QuietChannel(void)
{
    static const uint8_t kCand[3] = { 1, 6, 11 };
    uint8_t f[250];
    memset(f, 0, sizeof f);
    f[0] = 'S'; f[1] = 'h'; f[2] = 0; f[3] = 0;          // (ours, version 0: nobody takes it)
    uint32_t us[3];
    for (int c = 0; c < 3; c++) {
        (void)esp_wifi_set_channel(kCand[c], WIFI_SECOND_CHAN_NONE);
        vTaskDelay(pdMS_TO_TICKS(5));
        int64_t sum = 0; int n = 0;
        for (int i = 0; i < 24; i++) {
            while (xSemaphoreTake(_Slots, 0) == pdTRUE) { }   // (every slot taken: the next report gives one back)
            const int64_t t = esp_timer_get_time();
            if (esp_now_send(kAll, f, sizeof f) != ESP_OK) { for (int k = 0; k < _InAir; k++) (void)xSemaphoreGive(_Slots); break; }
            const BaseType_t got = xSemaphoreTake(_Slots, pdMS_TO_TICKS(50));
            sum += esp_timer_get_time() - t; n++;
            for (int k = 0; k < _InAir; k++) (void)xSemaphoreGive(_Slots);
            if (got != pdTRUE) sum += 50000;
        }
        us[c] = n ? (uint32_t)(sum / n) : 999999u;
    }
    int best = 0;
    for (int c = 1; c < 3; c++) if (us[c] < us[best]) best = c;
    printf("share: radio: a frame's wait on channel 1 / 6 / 11: %lu / %lu / %lu us: channel %u\n", (unsigned long)us[0], (unsigned long)us[1], (unsigned long)us[2], kCand[best]);
    return kCand[best];
}

esp_err_t CwlSh_Start(uint8_t channel, int queue_len)
{
    if (_On) return ESP_OK;
    const bool quiet = channel == 0;
    if (channel < 1 || channel > 13) channel = CWL_ShareChannel();
    memset(&_St, 0, sizeof _St);
    _SendSeq = _DoneSeq = 0; _LatSum = _WaitSum = 0;
    _FailRun = _OkRun = 0; _RateStep = false; _RateNow = 0;
    CWL_NetHold(true);                       // the link session ends, Wi-Fi off
    int qn = queue_len > 0 ? queue_len : 24;
    if (qn > kChunk * kMaxChunks) qn = kChunk * kMaxChunks;
    qn = (qn + kChunk - 1) / kChunk * kChunk;
    _Q = xQueueCreate(qn, sizeof(CwlShRx_t *));
    _Free = xQueueCreate(qn, sizeof(CwlShRx_t *));
    _Slots = xSemaphoreCreateCounting(_InAir, _InAir);
    const char *why = "the queue";
    if (!_Q || !_Free || !_Slots) goto fail;
    for (int c = 0; c < qn / kChunk; c++) {
        if (!(_Chunk[c] = malloc(sizeof(CwlShRx_t) * kChunk))) goto fail;
        for (int k = 0; k < kChunk; k++) { CwlShRx_t *p = &_Chunk[c][k]; (void)xQueueSend(_Free, &p, 0); }
    }
    why = "the Wi-Fi driver";
    if (CWL_WifiDriverInit() != ESP_OK) goto fail;
    (void)esp_wifi_set_mode(WIFI_MODE_STA);
    wifi_config_t wc = { 0 };
    (void)esp_wifi_set_config(WIFI_IF_STA, &wc);   // no access point
    why = "Wi-Fi start";
    if (esp_wifi_start() != ESP_OK) goto fail;
    vTaskDelay(pdMS_TO_TICKS(100));
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    (void)esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    why = "ESP-NOW";
    if (esp_now_init() != ESP_OK) { (void)esp_wifi_stop(); goto fail; }
    (void)esp_now_register_recv_cb(OnRecv);
    (void)esp_now_register_send_cb(OnSent);
    // awake all the time (a link session before may have left connectionless modem sleep: 50 ms of every 100)
    (void)esp_now_set_wake_window(65535);
    (void)esp_wifi_connectionless_module_set_wake_interval(0);
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    _PeerAdded = false;
    if (AddPeer(kAll) != ESP_OK) { (void)esp_now_deinit(); (void)esp_wifi_stop(); goto fail; }
    if (quiet) { _On = true; channel = QuietChannel(); _On = false; (void)esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE); }
    _St.channel = channel;
    _St.rate = _Rate;
    _St.heap_at_start = _St.heap_low = (uint32_t)esp_get_free_heap_size();
    _St.rx_queue_low = (uint32_t)(queue_len > 0 ? queue_len : 24);
    _On = true;
    _St.on = true;
    printf("share: radio on, channel %u, free heap %lu (largest %lu)\n", channel, (unsigned long)esp_get_free_heap_size(),
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return ESP_OK;
fail:
    if (_Q) { vQueueDelete(_Q); _Q = NULL; }
    if (_Free) { vQueueDelete(_Free); _Free = NULL; }
    for (int c = 0; c < kMaxChunks; c++) { free(_Chunk[c]); _Chunk[c] = NULL; }
    if (_Slots) { vSemaphoreDelete(_Slots); _Slots = NULL; }
    CWL_NetHold(false);
    printf("share: the radio did not start (%s; free heap %lu, largest %lu)\n", why, (unsigned long)esp_get_free_heap_size(), (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return ESP_FAIL;
}

void CwlSh_Stop(void)
{
    if (!_On) return;
    _On = false;
    _St.on = false;
    (void)esp_now_unregister_recv_cb();
    (void)esp_now_unregister_send_cb();
    (void)esp_now_deinit();
    (void)esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(20));
    if (_Q) { vQueueDelete(_Q); _Q = NULL; }
    if (_Free) { vQueueDelete(_Free); _Free = NULL; }
    for (int c = 0; c < kMaxChunks; c++) { free(_Chunk[c]); _Chunk[c] = NULL; }
    if (_Slots) { vSemaphoreDelete(_Slots); _Slots = NULL; }
    _PeerAdded = false;
    CWL_NetHold(false);                      // the session the link mode wants starts again by itself
    printf("share: radio off, free heap %lu\n", (unsigned long)esp_get_free_heap_size());
}

bool CwlSh_On(void) { return _On; }
void CwlSh_HoldLink(bool on) { if (on || !_On) CWL_NetHold(on); }
bool CwlSh_LinkHeld(void) { return CWL_NetHeld(); }
uint8_t CwlSh_Channel(void) { return _St.channel; }

void CwlSh_SetChannel(uint8_t channel)
{
    if (!_On || channel < 1 || channel > 13) return;
    if (esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) == ESP_OK) _St.channel = channel;
}

int CwlSh_Recv(CwlShRx_t *out, uint32_t wait_ms)
{
    if (!_On || !_Q) { vTaskDelay(pdMS_TO_TICKS(wait_ms ? wait_ms : 1)); return 0; }
    const UBaseType_t room = uxQueueMessagesWaiting(_Free);
    if (room < _St.rx_queue_low) _St.rx_queue_low = room;
    CwlShRx_t *p = NULL;
    if (xQueueReceive(_Q, &p, pdMS_TO_TICKS(wait_ms)) != pdTRUE || !p) return 0;
    memcpy(out, p, sizeof *out);
    (void)xQueueSend(_Free, &p, 0);
    return 1;
}

int CwlSh_Send(const uint8_t dst[6], const uint8_t *f, size_t n)
{
    if (!_On || n == 0 || n > ESP_NOW_MAX_DATA_LEN) return -1;
    if (_MuteUntil && esp_timer_get_time() < _MuteUntil) { vTaskDelay(1); return 0; }   // (bench cut: as if it went nowhere)
    const int64_t tw = esp_timer_get_time();
    const BaseType_t got = xSemaphoreTake(_Slots, pdMS_TO_TICKS(2));
    _WaitSum += esp_timer_get_time() - tw;
    if (got != pdTRUE) { _St.tx_full++; return -1; }
    if (AddPeer(dst) != ESP_OK) { (void)xSemaphoreGive(_Slots); _St.tx_fail++; return -1; }
    RateAdapt();
    _SentAt[_SendSeq & 31u] = esp_timer_get_time();
    _SendSeq++;
    const esp_err_t e = esp_now_send(dst, f, n);
    if (e != ESP_OK) { _SendSeq--; (void)xSemaphoreGive(_Slots); _St.tx_full++; return -1; }
    _St.tx++;
    const uint32_t fh = (uint32_t)esp_get_free_heap_size();
    if (fh < _St.heap_low) _St.heap_low = fh;
    return 0;
}

void CwlSh_OwnMac(uint8_t mac[6])
{
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
}

void CwlSh_GetStats(CwlShStats_t *st) { *st = _St; st->rate = _RateNow ? _RateNow : _Rate; st->lat_us = _DoneSeq ? (uint32_t)(_LatSum / _DoneSeq) : 0; st->wait_ms = (uint32_t)(_WaitSum / 1000); }
void CwlSh_SetRate(uint8_t rate) { _Rate = rate; _PeerAdded = false; }
uint8_t CwlSh_Rate(void) { return _Rate; }
void CwlSh_Mute(uint32_t ms) { _MuteUntil = ms ? esp_timer_get_time() + (int64_t)ms * 1000 : 0; printf("share: radio deaf and silent for %lu ms (bench)\n", (unsigned long)ms); }
void CwlSh_SetInAir(int n) { if (n >= 1 && n <= 32) _InAir = n; }
int CwlSh_InAir(void) { return _InAir; }

// the radio paused (on = 1): ESP-NOW down and its frame pool freed, Wi-Fi and the link hold kept (the heap for a job the
// share waits on: the store's index); on = 0: ESP-NOW up again on the same channel with a small pool (4 frames)
void CwlSh_Pause(int on)
{
    static uint8_t ch;
    if (on) {
        if (!_On) return;
        _On = false;
        (void)esp_now_unregister_recv_cb();
        (void)esp_now_unregister_send_cb();
        (void)esp_now_deinit();
        vTaskDelay(pdMS_TO_TICKS(20));
        if (_Q) { vQueueDelete(_Q); _Q = NULL; }
        if (_Free) { vQueueDelete(_Free); _Free = NULL; }
        for (int c = 0; c < kMaxChunks; c++) { free(_Chunk[c]); _Chunk[c] = NULL; }
        if (_Slots) { vSemaphoreDelete(_Slots); _Slots = NULL; }
        _PeerAdded = false;
        ch = _St.channel;
        printf("share: radio paused, free heap %lu (largest %lu)\n", (unsigned long)esp_get_free_heap_size(), (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return;
    }
    if (_On || !ch) return;
    _Q = xQueueCreate(kChunk, sizeof(CwlShRx_t *));
    _Free = xQueueCreate(kChunk, sizeof(CwlShRx_t *));
    _Slots = xSemaphoreCreateCounting(_InAir, _InAir);
    if (!_Q || !_Free || !_Slots || !(_Chunk[0] = malloc(sizeof(CwlShRx_t) * kChunk))) { printf("share: radio: no memory to resume\n"); return; }
    for (int k = 0; k < kChunk; k++) { CwlShRx_t *p = &_Chunk[0][k]; (void)xQueueSend(_Free, &p, 0); }
    (void)esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    if (esp_now_init() != ESP_OK) { printf("share: radio: ESP-NOW did not come back\n"); return; }
    (void)esp_now_register_recv_cb(OnRecv);
    (void)esp_now_register_send_cb(OnSent);
    (void)esp_now_set_wake_window(65535);
    (void)esp_wifi_connectionless_module_set_wake_interval(0);
    if (AddPeer(kAll) != ESP_OK) { (void)esp_now_deinit(); return; }
    _On = true;
    printf("share: radio resumed, channel %u\n", ch);
}
