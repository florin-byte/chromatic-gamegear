// cwl_time.c: see cwl_time.h
#include "cwl_time.h"
#include "cwl_civil.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>

#include "cwl.h"
#include "cwl_net.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "cwl_time"
enum { kSet_After = 1735689600, kBootWindow_ms = 45000, kAskWindow_ms = 30000, kWaitStep_ms = 50 };

static volatile bool _Blocked;
static bool _SntpUp;
static volatile bool _SntpDone;          // the time came by SNTP: SNTP stops (CwlTime_Poll), it is not needed again this boot
static volatile uint8_t _Source;          // 0 none, 1 SNTP, 2 HTTP Date, 3 set by hand (the clock page)
static volatile int64_t _AskUntil_ms;     // a game load asked for the radio until then
static volatile bool _LoadWaitFailed;     // a load waited and no time came: the next ones do not wait (no network in reach)

static int64_t NowMs(void) { return esp_timer_get_time() / 1000; }

bool CwlTime_IsSet(void) { return time(NULL) > kSet_After; }
// the time came from the network (Wi-Fi time wins over one set by hand)
static bool FromNetwork(void) { return (_Source == 1 || _Source == 2) && CwlTime_IsSet(); }
uint64_t CwlTime_Unix(void) { return CwlTime_IsSet() ? (uint64_t)time(NULL) : 0; }
bool CwlTime_ByHand(void) { return _Source == 3 && CwlTime_IsSet(); }
bool CwlTime_FromWifi(void) { return FromNetwork(); }

// The last real time the unit knew, kept across power-offs (no clock runs while it is off): the place the clock page starts
// from, and the day count for a time kept with a game when no real time is known yet.
static uint64_t _Last;
static bool _LastRead;
static uint64_t LastRead(void)
{
    if (_LastRead) return _Last;
    nvs_handle_t h;
    uint64_t v = 0;
    if (nvs_open("cwltime", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u64(h, "last", &v) != ESP_OK) v = 0;
        nvs_close(h);
    }
    _Last = v > kSet_After ? v : 0;
    _LastRead = true;
    return _Last;
}
static void Remember(void)
{
    const uint64_t now = CwlTime_Unix();
    if (!now) return;
    const uint64_t was = LastRead();
    if (now > was && now - was < 3600) return;          // (written at most about once an hour)
    nvs_handle_t h;
    if (nvs_open("cwltime", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_u64(h, "last", now) == ESP_OK) (void)nvs_commit(h);
    nvs_close(h);
    _Last = now;
}
uint64_t CwlTime_Last(void) { return LastRead(); }
uint64_t CwlTime_Anchor(void) { const uint64_t n = CwlTime_Unix(); return n ? n : LastRead(); }

bool CwlTime_SetByHand(uint64_t t)
{
    if (FromNetwork() || t <= kSet_After) return false;
    const struct timeval tv = {.tv_sec = (time_t)t, .tv_usec = 0};
    if (settimeofday(&tv, NULL) != 0) return false;
    _Source = 3;
    ESP_LOGI(TAG, "time set by hand: %lld", (long long)t);
    Remember();
    return true;
}

void CwlTime_Block(bool on)
{
    _Blocked = on;
    _LoadWaitFailed = false;
    if (on) {                                            // as a power-on with no network: no time, no radio for it
        if (_SntpUp) { esp_netif_sntp_deinit(); _SntpUp = false; }
        const struct timeval tv = {.tv_sec = 0, .tv_usec = 0};
        (void)settimeofday(&tv, NULL);
        _Source = 0;
    } else if (CWL_IsOnline()) {
        CwlTime_OnGotIp();
    }
}
bool CwlTime_Blocked(void) { return _Blocked; }

static void SyncCb(struct timeval *tv)
{
    (void)tv;
    _Source = 1;
    _SntpDone = true;
    ESP_LOGI(TAG, "time set by SNTP: %lld", (long long)time(NULL));
    Remember();
}

void CwlTime_OnGotIp(void)
{
    if (_Blocked || _SntpUp || FromNetwork()) return;   // (once the time is known SNTP never runs again this boot)
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST("time.cloudflare.com", "pool.ntp.org"));
    cfg.sync_cb = SyncCb;
    cfg.renew_servers_after_new_IP = true;
    cfg.ip_event_to_renew = IP_EVENT_STA_GOT_IP;
    cfg.index_of_first_server = 0;
    if (esp_netif_sntp_init(&cfg) == ESP_OK) _SntpUp = true;
    else ESP_LOGW(TAG, "sntp init failed");
}

// 1.1.11: SNTP stops once the time is known (by SNTP or our server's Date header). Left running it kept its state and its hourly resync for the whole power-on:
// the heap SAVE TO MY GAMES's handshake needs (unit 1: 1.1.10 lowest 3.2 KB, 1.1.11 2.2 KB). The clock keeps time itself,
// and our server's Date header sets it again when needed. Called from the Wi-Fi task's power decision.
void CwlTime_Poll(void)
{
    if ((_SntpDone || FromNetwork()) && _SntpUp) {
        esp_netif_sntp_deinit();
        _SntpUp = false;
        printf("clock: set from the network, SNTP stopped\n");
    }
}

bool CwlTime_Wants(void)
{
    if (_Blocked || FromNetwork()) return false;
    const int64_t now = NowMs();
    return now < kBootWindow_ms || now < _AskUntil_ms;
}

bool CwlTime_WaitForLoad(uint32_t ms)
{
    if (CwlTime_IsSet()) return true;
    if (_Blocked || CwlNet_SavedCount() <= 0) return false;
    const bool joining = CwlTime_Wants() || CWL_IsOnline();
    if (!joining && _LoadWaitFailed) return false;                 // (it waited once for nothing: no network in reach)
    const int64_t now = NowMs();
    if (_AskUntil_ms < now + kAskWindow_ms) _AskUntil_ms = now + kAskWindow_ms;
    CWL_Wake();
    printf("clock: no real time yet: the game waits up to %lu ms for Wi-Fi\n", (unsigned long)ms);
    for (uint32_t t = 0; t < ms && !CwlTime_IsSet(); t += kWaitStep_ms) vTaskDelay(pdMS_TO_TICKS(kWaitStep_ms));
    const bool ok = CwlTime_IsSet();
    if (!ok) _LoadWaitFailed = true;
    printf("clock: %s\n", ok ? "the real time arrived" : "no real time: the game starts without it");
    return ok;
}

static int Month(const char *m)
{
    static const char *const kM[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (int i = 0; i < 12; i++) if (strncasecmp(m, kM[i], 3) == 0) return i;
    return -1;
}

bool CwlTime_FromHttpDate(const char *date)
{
    if (!date || _Blocked || FromNetwork()) return false;
    int d, y, hh, mm, ss;
    char mon[4] = {0};
    const char *p = strchr(date, ',');
    p = p ? p + 1 : date;
    if (sscanf(p, " %d %3s %d %d:%d:%d", &d, mon, &y, &hh, &mm, &ss) != 6) return false;
    const int m = Month(mon);
    if (m < 0 || d < 1 || d > 31 || y < 2025 || y > 2100 || hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 60) return false;
    const int64_t t = CwlCivil_Unix(y, m + 1, d, hh, mm, ss);
    if (t <= kSet_After) return false;
    const struct timeval tv = {.tv_sec = (time_t)t, .tv_usec = 0};
    if (settimeofday(&tv, NULL) != 0) return false;
    _Source = 2;
    ESP_LOGI(TAG, "time set by an HTTP Date: %lld", (long long)t);
    Remember();
    return true;
}

void CwlTime_Print(void)
{
    printf("clock: %s, unix %lld, source %s%s, sntp %s\n", CwlTime_IsSet() ? "set" : "NOT set", (long long)time(NULL),
           _Source == 1 ? "sntp" : _Source == 2 ? "http date" : _Source == 3 ? "by hand" : "none", _Blocked ? ", BLOCKED (bench)" : "", _SntpUp ? "started" : "not started");
}
