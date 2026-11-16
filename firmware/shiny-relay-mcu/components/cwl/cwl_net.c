// cwl_net.c: saved Wi-Fi networks, scans, joins, auto-join and phone setup.
// See cwl_net.h. Passwords never leave NVS and this firmware.
#include "cwl_net.h"
#include "cwl_priv.h"
#include "cwl_portal.h"

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char Ssid[kCwlNet_SsidMax + 1];
    char Pass[kCwlNet_PassMax + 1];
} Entry_t;

typedef struct {
    uint8_t Count;
    Entry_t E[kCwlNet_MaxSaved];
} SavedBlob_t;

enum {
    kBitGotIp = 1 << 0,
    kBitFail = 1 << 1,
    kBitStaStart = 1 << 2,
    kBitScanDone = 1 << 3,
    kJoinTimeout_ms = 25000,   // scan + association + DHCP (a home router answered DHCP after 10-20 s once)
    kAutoScanEvery_ms = 30000,
    kAutoAfterFails = 3,
    kPortalDoneLinger_ms = 5000,   // the phone's status page (refresh 1 s) shows "Connected" first (was 8 s)
    kOpStack = 4096,
};

static const char *kNs = "cwlnet";
static SavedBlob_t _Saved;                       // saved networks, newest first
static volatile CwlNetOp_t _Op = kCwlNetOp_Idle;
static volatile CwlNetResult_t _Result = kCwlNetRes_None;
static char _LastSsid[kCwlNet_SsidMax + 1];
static EventGroupHandle_t _Ev;
static bool _HandlersOn = false;
static volatile uint8_t _DiscReason = 0;
static volatile uint8_t _AuthFails = 0;
static volatile uint32_t _JoinT0, _AssocMs, _IpMs;   // last join: ms to association / to an address

static CwlNetAp_t _Scan[kCwlNet_MaxScan];
static uint8_t _ScanCh[kCwlNet_MaxScan];
static int _NumScan = 0;
static volatile uint32_t _ScanSeq = 0;

// join request (from the OSD, the console or the phone page)
static char _ReqSsid[kCwlNet_SsidMax + 1];
static char _ReqPass[kCwlNet_PassMax + 1];
static bool _ReqUseSaved;

// auto-join (link task)
static uint32_t _AutoLastMs = 0;
static bool _AutoPending = false;
static uint32_t _AutoSeq = 0;
static uint32_t _AutoSwitches = 0;

// portal
static CwlNetPortal_t _Portal;
static volatile bool _PortalStop = false;
static volatile bool _PortalReq = false;     // the phone sent a network
static bool _PortalLan = false;              // test knob (cwl net portal lan): the station stays on the
                                             // current network, so the page is also served on the LAN
static esp_netif_t *_ApNetif = NULL;

static uint32_t NowMs(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---------------------------------------------------------------- saved list

static void SaveList(void)
{
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_blob(h, "list", &_Saved, sizeof(_Saved));
    (void)nvs_commit(h);
    nvs_close(h);
}

static void LoadList(void)
{
    memset(&_Saved, 0, sizeof(_Saved));
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof(_Saved);
    if (nvs_get_blob(h, "list", &_Saved, &n) != ESP_OK || n != sizeof(_Saved) || _Saved.Count > kCwlNet_MaxSaved) memset(&_Saved, 0, sizeof(_Saved));
    nvs_close(h);
}

static int Find(const char *Ssid)
{
    for (int i = 0; i < _Saved.Count; i++)
        if (strcmp(_Saved.E[i].Ssid, Ssid) == 0) return i;
    return -1;
}

// Puts (Ssid, Pass) first; the oldest falls off a full list.
static void Remember(const char *Ssid, const char *Pass)
{
    const int at = Find(Ssid);
    Entry_t e;
    memset(&e, 0, sizeof(e));
    snprintf(e.Ssid, sizeof(e.Ssid), "%s", Ssid);
    snprintf(e.Pass, sizeof(e.Pass), "%s", Pass);
    int n = _Saved.Count;
    if (at >= 0) {
        memmove(&_Saved.E[1], &_Saved.E[0], (size_t)at * sizeof(Entry_t));
    } else {
        if (n == kCwlNet_MaxSaved) n--;
        memmove(&_Saved.E[1], &_Saved.E[0], (size_t)n * sizeof(Entry_t));
        _Saved.Count = (uint8_t)(n + 1);
    }
    _Saved.E[0] = e;
    memset(&e, 0, sizeof(e));
    SaveList();
}

void CwlNet_Init(void)
{
    LoadList();
    if (_Saved.Count == 0) {
        // 0.15 and older: one network, set from the console. It becomes the
        // first saved one (its NVS copy in "cwl" stays as it is).
        char s[kCwlNet_SsidMax + 1], p[kCwlNet_PassMax + 1];
        CWL_GetCurrentCredentials(s, sizeof(s), p, sizeof(p));
        if (s[0] != '\0') Remember(s, p);
        memset(p, 0, sizeof(p));
    }
}

int CwlNet_SavedCount(void) { return _Saved.Count; }

bool CwlNet_SavedSsid(int Index, char *Out, unsigned Len)
{
    if (Index < 0 || Index >= _Saved.Count || Out == NULL || Len == 0) return false;
    snprintf(Out, Len, "%s", _Saved.E[Index].Ssid);
    return true;
}

bool CwlNet_IsSaved(const char *Ssid) { return Find(Ssid) >= 0; }

void CwlNet_Forget(int Index)
{
    if (Index < 0 || Index >= _Saved.Count) return;
    char cur[kCwlNet_SsidMax + 1], p[kCwlNet_PassMax + 1];
    CWL_GetCurrentCredentials(cur, sizeof(cur), p, sizeof(p));
    const bool WasCurrent = strcmp(cur, _Saved.E[Index].Ssid) == 0;
    memmove(&_Saved.E[Index], &_Saved.E[Index + 1], (size_t)(_Saved.Count - Index - 1) * sizeof(Entry_t));
    _Saved.Count--;
    memset(&_Saved.E[_Saved.Count], 0, sizeof(Entry_t));
    SaveList();
    if (WasCurrent) {
        // The link moves to the newest one left (or to none).
        if (_Saved.Count > 0) CWL_SetCurrentNetwork(_Saved.E[0].Ssid, _Saved.E[0].Pass);
        else CWL_SetCurrentNetwork("", "");
    }
    memset(p, 0, sizeof(p));
}

// ---------------------------------------------------------------- driver

// Phone-setup timeline (ms after the setup network is up; 0 = not seen), printed
// when it closes: where a slow setup spends its time, on a real phone.
static struct {
    uint32_t T0, Joined, Dns, Page, Sent, Done, Assoc, Ip;
    uint32_t Pages, Dnses;
} _Tl;
static void TlMark(uint32_t *At)
{
    if (_Tl.T0 != 0 && *At == 0) *At = NowMs() - _Tl.T0 + 1;
}

static void OnEvent(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        xEventGroupSetBits(_Ev, kBitStaStart);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        TlMark(&_Tl.Joined);   // the phone associated with the setup network
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        _DiscReason = d->reason;
        if (d->reason == WIFI_REASON_AUTH_FAIL || d->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
            d->reason == WIFI_REASON_HANDSHAKE_TIMEOUT || d->reason == WIFI_REASON_AUTH_EXPIRE ||
            d->reason == WIFI_REASON_MIC_FAILURE) _AuthFails++;
        xEventGroupSetBits(_Ev, kBitFail);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        if (_AssocMs == 0) _AssocMs = NowMs() - _JoinT0;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        if (_IpMs == 0) _IpMs = NowMs() - _JoinT0;
        xEventGroupSetBits(_Ev, kBitGotIp);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        uint16_t n = 0;
        (void)esp_wifi_scan_get_ap_num(&n);
        if (n > 20) n = 20;
        wifi_ap_record_t *r = n ? calloc(n, sizeof(*r)) : NULL;
        if (r != NULL && esp_wifi_scan_get_ap_records(&n, r) == ESP_OK) {
            char cur[kCwlNet_SsidMax + 1], p[kCwlNet_PassMax + 1];
            CWL_GetCurrentCredentials(cur, sizeof(cur), p, sizeof(p));
            memset(p, 0, sizeof(p));
            int k = 0;
            for (int i = 0; i < n && k < kCwlNet_MaxScan; i++) {   // the driver sorts by RSSI
                const char *s = (const char *)r[i].ssid;
                if (s[0] == '\0') continue;                         // hidden
                bool dup = false;
                for (int j = 0; j < k; j++) if (strcmp(_Scan[j].Ssid, s) == 0) dup = true;
                if (dup) continue;
                CwlNetAp_t *a = &_Scan[k];
                snprintf(a->Ssid, sizeof(a->Ssid), "%s", s);
                a->Rssi = r[i].rssi;
                a->Secure = r[i].authmode != WIFI_AUTH_OPEN;
                a->Saved = Find(s) >= 0;
                a->Current = strcmp(s, cur) == 0;
                _ScanCh[k] = r[i].primary;
                k++;
            }
            _NumScan = k;
        } else {
            (void)esp_wifi_clear_ap_list();
        }
        free(r);
        _ScanSeq++;
        xEventGroupSetBits(_Ev, kBitScanDone);
    }
}

static esp_err_t DriverUp(void)
{
    esp_err_t err = CWL_WifiDriverInit();
    if (err != ESP_OK) return err;
    if (!_HandlersOn) {
        (void)esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, OnEvent, NULL);
        (void)esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, OnEvent, NULL);
        _HandlersOn = true;
    }
    return ESP_OK;
}

// The station on its own (the link session is held).
static esp_err_t StaStart(void)
{
    if (DriverUp() != ESP_OK) return ESP_FAIL;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta != NULL) (void)esp_netif_dhcpc_start(sta);   // the link may have left it on a static lease
    xEventGroupClearBits(_Ev, 0xff);
    (void)esp_wifi_set_mode(WIFI_MODE_STA);
    wifi_config_t wc = {0};
    (void)esp_wifi_set_config(WIFI_IF_STA, &wc);       // no auto-connect to the old network
    if (esp_wifi_start() != ESP_OK) return ESP_FAIL;
    (void)xEventGroupWaitBits(_Ev, kBitStaStart, pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    return ESP_OK;
}

static void StaStop(void)
{
    (void)esp_wifi_disconnect();
    (void)esp_wifi_stop();
}

static void ScanBlocking(void)
{
    xEventGroupClearBits(_Ev, kBitScanDone);
    const wifi_scan_config_t sc = {.show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE};
    if (esp_wifi_scan_start(&sc, false) == ESP_OK) (void)xEventGroupWaitBits(_Ev, kBitScanDone, pdTRUE, pdFALSE, pdMS_TO_TICKS(8000));
}

static CwlNetResult_t ReasonResult(uint8_t r)
{
    switch (r) {
    case WIFI_REASON_NO_AP_FOUND: return kCwlNetRes_NotFound;
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_MIC_FAILURE: return kCwlNetRes_WrongPassword;
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_ASSOC_EXPIRE:
    case WIFI_REASON_ASSOC_TOOMANY:
    case WIFI_REASON_NOT_AUTHED:
    case WIFI_REASON_NOT_ASSOCED:
    case WIFI_REASON_ASSOC_LEAVE: return kCwlNetRes_Refused;
    default: return kCwlNetRes_Failed;
    }
}

static uint8_t _JoinedCh = 0;   // channel of the last successful TryJoin (for the link session)

static void StaConfig(const char *Ssid, const char *Pass, uint8_t Ch)
{
    wifi_config_t wc = {0};
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", Ssid);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", Pass);
    wc.sta.threshold.authmode = Pass[0] ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    wc.sta.channel = Ch;
    wc.sta.scan_method = Ch ? WIFI_FAST_SCAN : WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    (void)esp_wifi_set_config(WIFI_IF_STA, &wc);
    memset(&wc, 0, sizeof(wc));
}

// Joins (Ssid, Pass) on a started station; waits for an address.
static CwlNetResult_t TryJoin(const char *Ssid, const char *Pass)
{
    wifi_ap_record_t was;
    if (esp_wifi_sta_get_ap_info(&was) == ESP_OK) {
        // Joined elsewhere (the phone-setup LAN test): leave first, or the old
        // association answers for the new network.
        xEventGroupClearBits(_Ev, kBitFail);
        (void)esp_wifi_disconnect();
        (void)xEventGroupWaitBits(_Ev, kBitFail, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
    }
    // 0.17.2: the channel from the last scan when the network was in it.
    // The station then probes that one channel instead of all 13 (~2 s less),
    // and in phone setup the setup network (already moved to that channel)
    // stays on the air, so the phone keeps its connection while it waits.
    // Not found there: one more try over all channels.
    uint8_t ch = 0;
    for (int i = 0; i < _NumScan; i++) if (strcmp(_Scan[i].Ssid, Ssid) == 0) { ch = _ScanCh[i]; break; }
    StaConfig(Ssid, Pass, ch);
    _JoinedCh = 0;
    _AuthFails = 0;
    _DiscReason = 0;
    const uint32_t t0 = NowMs();
    _JoinT0 = t0;
    _AssocMs = _IpMs = 0;
    bool Associated = false;
    for (int attempt = 0; attempt < 3; attempt++) {
        xEventGroupClearBits(_Ev, kBitGotIp | kBitFail);
        if (esp_wifi_connect() != ESP_OK) return kCwlNetRes_Failed;
        const uint32_t left = kJoinTimeout_ms - (NowMs() - t0);
        const EventBits_t b = xEventGroupWaitBits(_Ev, kBitGotIp | kBitFail, pdTRUE, pdFALSE, pdMS_TO_TICKS(left));
        if (b & kBitGotIp) {
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) _JoinedCh = ap.primary;
            return kCwlNetRes_Ok;
        }
        if (!(b & kBitFail)) {
            // Neither: associated (no failure) but DHCP never answered, or still scanning.
            wifi_ap_record_t ap;
            Associated = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
            return Associated ? kCwlNetRes_NoAddress : kCwlNetRes_Timeout;
        }
        if (_DiscReason == WIFI_REASON_NO_AP_FOUND && ch != 0) {
            ch = 0;   // not on the scanned channel (moved, or a band-steering twin): all channels
            StaConfig(Ssid, Pass, 0);
            attempt--;
            continue;
        }
        // Two password failures are enough; not found once is enough.
        if (_AuthFails >= 2 || _DiscReason == WIFI_REASON_NO_AP_FOUND) break;
        if (NowMs() - t0 > kJoinTimeout_ms - 1000) break;
    }
    return ReasonResult(_DiscReason);
}

// ---------------------------------------------------------------- operations

static void OpScan(void)
{
    if (StaStart() == ESP_OK) ScanBlocking();
    StaStop();
}

static void OpJoin(void)
{
    char pass[kCwlNet_PassMax + 1];
    snprintf(pass, sizeof(pass), "%s", _ReqPass);
    memset(_ReqPass, 0, sizeof(_ReqPass));
    if (_ReqUseSaved) {
        const int i = Find(_ReqSsid);
        if (i < 0) { _Result = kCwlNetRes_Failed; return; }
        snprintf(pass, sizeof(pass), "%s", _Saved.E[i].Pass);
    }
    CwlNetResult_t r = kCwlNetRes_Failed;
    if (StaStart() == ESP_OK) r = TryJoin(_ReqSsid, pass);
    if (r == kCwlNetRes_Ok) {
        Remember(_ReqSsid, pass);
        CWL_SetCurrentNetwork(_ReqSsid, pass);
        CWL_SetChannelHint(_JoinedCh);   // the link session rejoins on it: no full scan (0.17.2)
    }
    memset(pass, 0, sizeof(pass));
    StaStop();
    _Result = r;
}

static void OpPortal(void);

static void OpTask(void *arg)
{
    const CwlNetOp_t op = (CwlNetOp_t)(intptr_t)arg;
    CWL_NetHold(true);
    if (op == kCwlNetOp_Scanning) OpScan();
    else if (op == kCwlNetOp_Joining) OpJoin();
    else if (op == kCwlNetOp_Portal) OpPortal();
    CWL_NetHold(false);
    _Op = kCwlNetOp_Idle;
    vTaskDelete(NULL);
}

static bool StartOp(CwlNetOp_t op)
{
    if (_Op != kCwlNetOp_Idle) return false;
    if (_Ev == NULL) _Ev = xEventGroupCreate();
    _Op = op;
    if (xTaskCreatePinnedToCore(OpTask, "cwlnet", kOpStack, (void *)(intptr_t)op, 5, NULL, 0) != pdPASS) {
        _Op = kCwlNetOp_Idle;
        return false;
    }
    return true;
}

CwlNetOp_t CwlNet_Op(void) { return _Op; }
bool CwlNet_ScanStart(void) { return StartOp(kCwlNetOp_Scanning); }
uint32_t CwlNet_ScanSeq(void) { return _ScanSeq; }

int CwlNet_ScanResults(CwlNetAp_t *Out, int Max)
{
    int n = _NumScan < Max ? _NumScan : Max;
    for (int i = 0; i < n; i++) {
        Out[i] = _Scan[i];
        Out[i].Saved = Find(Out[i].Ssid) >= 0;
    }
    return n;
}

const CwlNetAp_t *CwlNet_ScanList(int *Count)
{
    for (int i = 0; i < _NumScan; i++) _Scan[i].Saved = Find(_Scan[i].Ssid) >= 0;
    *Count = _NumScan;
    return _Scan;
}

bool CwlNet_Join(const char *Ssid, const char *Pass)
{
    if (Ssid == NULL || Ssid[0] == '\0' || _Op != kCwlNetOp_Idle) return false;
    snprintf(_ReqSsid, sizeof(_ReqSsid), "%s", Ssid);
    snprintf(_LastSsid, sizeof(_LastSsid), "%s", Ssid);
    _ReqUseSaved = Pass == NULL;
    snprintf(_ReqPass, sizeof(_ReqPass), "%s", Pass ? Pass : "");
    _Result = kCwlNetRes_None;
    if (!StartOp(kCwlNetOp_Joining)) {
        memset(_ReqPass, 0, sizeof(_ReqPass));
        return false;
    }
    return true;
}

CwlNetResult_t CwlNet_LastResult(void) { return _Result; }
const char *CwlNet_LastSsid(void) { return _LastSsid; }

const char *CwlNet_ResultText(CwlNetResult_t r)
{
    switch (r) {
    case kCwlNetRes_Ok:            return "CONNECTED";
    case kCwlNetRes_WrongPassword: return "WRONG  PASSWORD";
    case kCwlNetRes_NotFound:      return "NETWORK  NOT  FOUND";
    case kCwlNetRes_Refused:       return "NETWORK  REFUSED  US";
    case kCwlNetRes_NoAddress:     return "NO  IP  ADDRESS";
    case kCwlNetRes_Timeout:       return "NO  ANSWER";
    case kCwlNetRes_Failed:        return "COULD  NOT  JOIN";
    default:                       return "";
    }
}

// ---------------------------------------------------------------- auto-join

void CwlNet_AutoJoinPoll(uint32_t Now, bool Associated, int ConnFails)
{
    if (Associated || _Op != kCwlNetOp_Idle) { _AutoPending = false; return; }
    if (_AutoPending) {
        if (_ScanSeq == _AutoSeq) {
            if (Now - _AutoLastMs > 10000) _AutoPending = false;   // the scan never finished
            return;
        }
        _AutoPending = false;
        // Strongest saved network in range (the scan list is strongest first).
        char cur[kCwlNet_SsidMax + 1], p[kCwlNet_PassMax + 1];
        CWL_GetCurrentCredentials(cur, sizeof(cur), p, sizeof(p));
        memset(p, 0, sizeof(p));
        for (int i = 0; i < _NumScan; i++) {
            const int s = Find(_Scan[i].Ssid);
            if (s < 0) continue;
            if (strcmp(_Saved.E[s].Ssid, cur) != 0) {
                CWL_SetCurrentNetwork(_Saved.E[s].Ssid, _Saved.E[s].Pass);
                _AutoSwitches++;
                printf("cwl net: auto-join %s\n", _Saved.E[s].Ssid);
            }
            CWL_RejoinNow();
            return;
        }
        return;
    }
    if (ConnFails < kAutoAfterFails || _Saved.Count < 2 || Now - _AutoLastMs < kAutoScanEvery_ms) return;
    if (_Ev == NULL) _Ev = xEventGroupCreate();
    if (DriverUp() != ESP_OK) return;
    _AutoLastMs = Now;
    _AutoSeq = _ScanSeq;
    (void)esp_wifi_disconnect();
    const wifi_scan_config_t sc = {.show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE};
    _AutoPending = esp_wifi_scan_start(&sc, false) == ESP_OK;
}

// ---------------------------------------------------------------- phone setup

static const char kPageHead[] =
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Chromatic Wi-Fi</title><style>body{font-family:sans-serif;margin:16px;max-width:440px}"
    "label{display:block;margin:12px 0 4px}input,select,button{font-size:18px;width:100%;padding:10px;box-sizing:border-box}"
    "button{margin-top:16px}</style></head><body><h2>Chromatic Wi-Fi</h2>";

static void HtmlEsc(char *out, size_t len, const char *in)
{
    size_t o = 0;
    for (; *in && o + 7 < len; in++) {
        const char c = *in;
        const char *r = c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '&' ? "&amp;" : c == '"' ? "&quot;" : c == '\'' ? "&#39;" : NULL;
        if (r) { size_t k = strlen(r); memcpy(out + o, r, k); o += k; }
        else out[o++] = c;
    }
    out[o] = '\0';
}

// A minimal HTTP/1.0 server inside the portal task (no server task, one
// connection at a time, < 2 KB of buffers): esp_http_server cost ~9 KB of
// heap here, where only ~20 KB are left with the setup network up.
static void Out(int c, const char *str)
{
    size_t n = strlen(str), off = 0;
    while (off < n) {
        const int w = send(c, str + off, n - off, 0);
        if (w <= 0) return;
        off += (size_t)w;
    }
}

static void Head(int c, const char *status, const char *extra)
{
    Out(c, "HTTP/1.0 ");
    Out(c, status);
    Out(c, "\r\nConnection: close\r\nCache-Control: no-store\r\n");
    if (extra) Out(c, extra);
    Out(c, "\r\n");
}

static void PageRoot(int c)
{
    Head(c, "200 OK", "Content-Type: text/html\r\n");
    Out(c, kPageHead);
    Out(c, "<form method=post action=/join><label>Network</label><select name=s>");
    char esc[6 * kCwlNet_SsidMax + 8], line[2 * sizeof(esc) + 64];
    for (int i = 0; i < _NumScan; i++) {
        HtmlEsc(esc, sizeof(esc), _Scan[i].Ssid);
        const int bars = _Scan[i].Rssi > -55 ? 4 : _Scan[i].Rssi > -67 ? 3 : _Scan[i].Rssi > -75 ? 2 : 1;
        snprintf(line, sizeof(line), "<option value=\"%s\">%s (%d/4%s)</option>", esc, esc, bars, _Scan[i].Secure ? ", locked" : "");
        Out(c, line);
    }
    Out(c, "</select><label>Or type the name</label><input name=o maxlength=32 autocapitalize=off>"
           "<label>Password</label><input name=p type=password maxlength=64>"
           "<button>Join</button></form></body></html>");
}

static int Hex(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

// Form field `key` from an urlencoded body.
static void FormField(const char *body, const char *key, char *out, size_t len)
{
    out[0] = '\0';
    const size_t kl = strlen(key);
    for (const char *p = body; p && *p;) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            p += kl + 1;
            size_t o = 0;
            while (*p && *p != '&' && o + 1 < len) {
                if (*p == '+') { out[o++] = ' '; p++; }
                else if (*p == '%' && Hex(p[1]) >= 0 && Hex(p[2]) >= 0) { out[o++] = (char)(Hex(p[1]) * 16 + Hex(p[2])); p += 3; }
                else out[o++] = *p++;
            }
            out[o] = '\0';
            return;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
}

static void PageJoin(int c, const char *body)
{
    char s[kCwlNet_SsidMax + 1], o[kCwlNet_SsidMax + 1];
    FormField(body, "s", s, sizeof(s));
    FormField(body, "o", o, sizeof(o));
    FormField(body, "p", _ReqPass, sizeof(_ReqPass));
    snprintf(_ReqSsid, sizeof(_ReqSsid), "%s", o[0] ? o : s);
    snprintf(_Portal.JoinSsid, sizeof(_Portal.JoinSsid), "%s", _ReqSsid);
    snprintf(_LastSsid, sizeof(_LastSsid), "%s", _ReqSsid);
    _ReqUseSaved = false;
    _Portal.Result = kCwlNetRes_None;
    _Portal.Joining = _ReqSsid[0] != '\0';
    _PortalReq = _Portal.Joining;
    Head(c, "303 See Other", "Location: /status\r\n");
}

static void PageStatus(int c)
{
    Head(c, "200 OK", "Content-Type: text/html\r\n");
    char esc[6 * kCwlNet_SsidMax + 8], line[sizeof(esc) + 200];
    HtmlEsc(esc, sizeof(esc), _Portal.JoinSsid);
    Out(c, kPageHead);
    if (_Portal.Joining || _PortalReq) {
        snprintf(line, sizeof(line), "<meta http-equiv=refresh content=1><p>Joining <b>%s</b>... This page may stop for a"
                 " moment while the Chromatic changes channel.</p>", esc);
    } else if (_Portal.Result == kCwlNetRes_Ok) {
        snprintf(line, sizeof(line), "<p>Connected to <b>%s</b>. It is saved on the Chromatic.</p>"
                 "<p>This setup network closes now. You can close this page.</p>", esc);
    } else if (_Portal.Result != kCwlNetRes_None) {
        static const char *const kWhy[] = {"", "", "The password is wrong.", "The network was not found.",
                                           "The network refused the Chromatic.", "The network gave no address.",
                                           "The network did not answer.", "It could not join."};
        snprintf(line, sizeof(line), "<p>%s (%s)</p><p><a href=/>Try again</a></p>", kWhy[_Portal.Result < 8 ? _Portal.Result : 7], esc);
    } else {
        snprintf(line, sizeof(line), "<p><a href=/>Choose a network</a></p>");
    }
    Out(c, line);
    Out(c, "</body></html>");
}

enum { kReqBuf = 1024, kMaxCli = 3, kCliIdle_ms = 10000 };
static uint32_t _Requests = 0, _DnsAnswers = 0;
static char *_ReqBuf = NULL;   // allocated only while the setup network runs

// One request on an accepted connection.
static void Serve(int c)
{
    // Called once the request has started to arrive: the rest of a phone's
    // request follows within milliseconds.
    struct timeval tv = {.tv_sec = 0, .tv_usec = 500000};
    (void)setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char *req = _ReqBuf;
    if (req == NULL) return;
    int n = 0;
    char *hdr_end = NULL;
    while (n < kReqBuf - 1) {
        const int r = recv(c, req + n, (size_t)(kReqBuf - 1 - n), 0);
        if (r <= 0) break;
        n += r;
        req[n] = '\0';
        if ((hdr_end = strstr(req, "\r\n\r\n")) != NULL) {
            const char *cl = strstr(req, "Content-Length:");
            if (cl == NULL) cl = strstr(req, "content-length:");
            const int want = cl ? atoi(cl + 15) : 0;
            const int have = n - (int)(hdr_end + 4 - req);
            if (have >= want) break;
        }
    }
    req[n] = '\0';
    if (hdr_end == NULL) return;
    const bool post = strncmp(req, "POST ", 5) == 0;
    const char *path = strchr(req, ' ');
    if (path == NULL) return;
    path++;
    _Requests++;
    TlMark(&_Tl.Page);
    _Tl.Pages++;
    if (!post && strncmp(path, "/ ", 2) == 0) PageRoot(c);
    else if (!post && strncmp(path, "/status ", 8) == 0) PageStatus(c);
    else if (post && strncmp(path, "/join ", 6) == 0) PageJoin(c, hdr_end + 4);
    else Head(c, "302 Found", "Location: http://192.168.4.1/\r\n");   // phones' captive-portal probes
    memset(req, 0, kReqBuf);   // the password was in it
}

// Every A question gets 192.168.4.1, so the phone opens the setup page;
// other types get an empty answer at once (cwl_portal.c).
static void DnsAnswer(int sock)
{
    uint8_t buf[300];
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    const int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
    static const uint8_t kIp[4] = {192, 168, 4, 1};
    const int r = CwlPortal_DnsReply(buf, n, (int)sizeof(buf), kIp);
    if (r <= 0) return;
    (void)sendto(sock, buf, (size_t)r, 0, (struct sockaddr *)&from, fl);
    _DnsAnswers++;
    TlMark(&_Tl.Dns);
    _Tl.Dnses++;
}

static void NoteHeap(void)
{
    const uint32_t f = esp_get_free_heap_size();
    _Portal.HeapFree = f;
    if (_Portal.HeapMin == 0 || f < _Portal.HeapMin) _Portal.HeapMin = f;
}

// The setup network must sit on the channel of the network the station
// joins: the ESP32 has one radio, and an access point elsewhere makes the
// station miss its beacons (it dropped with reason 200 after ~10 s).
static void ApChannelFor(const char *Ssid)
{
    for (int i = 0; i < _NumScan; i++) {
        if (strcmp(_Scan[i].Ssid, Ssid) != 0) continue;
        wifi_config_t cur;
        if (esp_wifi_get_config(WIFI_IF_AP, &cur) == ESP_OK && cur.ap.channel != _ScanCh[i]) {
            cur.ap.channel = _ScanCh[i];
            (void)esp_wifi_set_config(WIFI_IF_AP, &cur);
        }
        memset(&cur, 0, sizeof(cur));
        return;
    }
}

static void OpPortal(void)
{
    // 1. What is around (for the page), on the station alone.
    printf("cwl net: portal heap: held %lu\n", (unsigned long)esp_get_free_heap_size());
    if (StaStart() != ESP_OK) { StaStop(); return; }
    ScanBlocking();
    printf("cwl net: portal heap: station + scan %lu\n", (unsigned long)esp_get_free_heap_size());
    // 2. The setup network beside the station.
    if (_ApNetif == NULL) _ApNetif = esp_netif_create_default_wifi_ap();
    uint8_t mac[6];
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(_Portal.Ssid, sizeof(_Portal.Ssid), "ShinyRelay-%02X%02X", mac[4], mac[5]);
    // 0.17.1: 15 random characters (62 bits; 8 letters were ~40 bits, which a
    // recorded handshake gives up offline, and with it the home network's
    // password the phone sends). Format and alphabet: cwl_sec.h.
    (void)CwlSec_MakeSetupPass(_Portal.Pass, sizeof(_Portal.Pass), esp_random);
    wifi_config_t ap = {0};
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", _Portal.Ssid);
    ap.ap.ssid_len = (uint8_t)strlen(_Portal.Ssid);
    snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s", _Portal.Pass);
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;   // WPA2-AES only (no TKIP mix a phone may flag or try first)
    ap.ap.beacon_interval = 100;                     // explicit: the {0} config left it 0
    ap.ap.channel = _NumScan > 0 ? _ScanCh[0] : 1;   // near the strongest network: little channel change on joining it
    // (The station joins only later, on the phone's request; the setup network
    // then follows its channel: one radio, one channel.)
    ap.ap.max_connection = 2;
    (void)esp_wifi_set_mode(WIFI_MODE_APSTA);
    (void)esp_wifi_set_config(WIFI_IF_AP, &ap);
    memset(&ap, 0, sizeof(ap));
    // (A mode change drops a joined station: join only now.)
    if (_PortalLan) {
        char cs[kCwlNet_SsidMax + 1], cp[kCwlNet_PassMax + 1];
        CWL_GetCurrentCredentials(cs, sizeof(cs), cp, sizeof(cp));
        ApChannelFor(cs);
        const CwlNetResult_t r = TryJoin(cs, cp);
        memset(cp, 0, sizeof(cp));
        printf("cwl net: portal test: station on %s (%s)\n", cs, CwlNet_ResultText(r));
    }
    printf("cwl net: portal heap: + setup network %lu\n", (unsigned long)esp_get_free_heap_size());
    // 3. The page (TCP 80) and the DNS answerer (UDP 53), served from this task.
    _ReqBuf = malloc(kReqBuf);
    int web = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (web >= 0) {
        int yes = 1;
        (void)setsockopt(web, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(80), .sin_addr.s_addr = htonl(INADDR_ANY)};
        if (bind(web, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(web, 4) != 0) { close(web); web = -1; }
    }
    int dns = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (dns >= 0) {
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
        if (bind(dns, (struct sockaddr *)&a, sizeof(a)) != 0) { close(dns); dns = -1; }
    }
    _Portal.Active = true;
    memset(&_Tl, 0, sizeof(_Tl));
    _Tl.T0 = NowMs();
    printf("cwl net: phone setup on %s (heap %lu, web %d, dns %d)\n", _Portal.Ssid, (unsigned long)esp_get_free_heap_size(), web, dns);
    // 4. Serve until a network works, the user stops it, or 5 minutes pass.
    const uint32_t t0 = NowMs();
    uint32_t done_at = 0;
    int cli[kMaxCli];
    uint32_t cli_t[kMaxCli] = {0};
    for (int i = 0; i < kMaxCli; i++) cli[i] = -1;
    while (!_PortalStop) {
        const uint32_t now = NowMs();
        const uint32_t el = (now - t0) / 1000;
        _Portal.SecondsLeft = el >= kCwlNet_PortalTimeout_s ? 0 : kCwlNet_PortalTimeout_s - el;
        wifi_sta_list_t sl;
        if (esp_wifi_ap_get_sta_list(&sl) == ESP_OK) _Portal.Clients = (uint8_t)sl.num;
        NoteHeap();
        if (done_at ? (now - done_at) > kPortalDoneLinger_ms : _Portal.SecondsLeft == 0) break;
        if (_PortalReq) {
            _PortalReq = false;
            TlMark(&_Tl.Sent);
            char pass[kCwlNet_PassMax + 1];
            snprintf(pass, sizeof(pass), "%s", _ReqPass);
            memset(_ReqPass, 0, sizeof(_ReqPass));
            // The setup network follows the station's channel when it joins:
            // move it there first, so the phone (which rejoins) can see the result.
            ApChannelFor(_ReqSsid);
            const CwlNetResult_t r = TryJoin(_ReqSsid, pass);
            if (r == kCwlNetRes_Ok) {
                Remember(_ReqSsid, pass);
                CWL_SetCurrentNetwork(_ReqSsid, pass);
                CWL_SetChannelHint(_JoinedCh);   // the link session rejoins on it: no full scan (0.17.2)
                done_at = NowMs();
                TlMark(&_Tl.Done);
                _Tl.Assoc = _AssocMs;
                _Tl.Ip = _IpMs;
            } else {
                (void)esp_wifi_disconnect();
                if (_PortalLan) {
                    char cs[kCwlNet_SsidMax + 1], cp[kCwlNet_PassMax + 1];
                    CWL_GetCurrentCredentials(cs, sizeof(cs), cp, sizeof(cp));
                    ApChannelFor(cs);
                    (void)TryJoin(cs, cp);   // back on the LAN for the test page
                    memset(cp, 0, sizeof(cp));
                }
            }
            memset(pass, 0, sizeof(pass));
            _Portal.Result = r;
            _Result = r;
            _Portal.Joining = false;
        }
        // Up to kMaxCli open connections, each served when its request
        // arrives (0.17.2). Before, one accepted connection was read with a 2 s
        // timeout while every other waited: a phone's captive-portal sheet
        // opens speculative connections that send nothing at first, and each
        // one held the page (and the probe answers) up for 2 s.
        fd_set rd;
        FD_ZERO(&rd);
        int mx = -1;
        if (web >= 0) { FD_SET(web, &rd); mx = web; }
        if (dns >= 0) { FD_SET(dns, &rd); if (dns > mx) mx = dns; }
        for (int i = 0; i < kMaxCli; i++) {
            if (cli[i] < 0) continue;
            if (now - cli_t[i] > kCliIdle_ms) { close(cli[i]); cli[i] = -1; continue; }   // never sent a request
            FD_SET(cli[i], &rd);
            if (cli[i] > mx) mx = cli[i];
        }
        struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
        if (mx < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        if (select(mx + 1, &rd, NULL, NULL, &tv) > 0) {
            if (dns >= 0 && FD_ISSET(dns, &rd)) DnsAnswer(dns);
            for (int i = 0; i < kMaxCli; i++) {
                if (cli[i] < 0 || !FD_ISSET(cli[i], &rd)) continue;
                Serve(cli[i]);
                shutdown(cli[i], SHUT_RDWR);
                close(cli[i]);
                cli[i] = -1;
            }
            if (web >= 0 && FD_ISSET(web, &rd)) {
                const int c = accept(web, NULL, NULL);
                if (c >= 0) {
                    int slot = -1, oldest = 0;
                    for (int i = 0; i < kMaxCli; i++) {
                        if (cli[i] < 0) { slot = i; break; }
                        if ((int32_t)(cli_t[i] - cli_t[oldest]) < 0) oldest = i;
                    }
                    if (slot < 0) { close(cli[oldest]); slot = oldest; }   // full: the oldest silent one goes
                    cli[slot] = c;
                    cli_t[slot] = NowMs();
                }
            }
        }
    }
    for (int i = 0; i < kMaxCli; i++) if (cli[i] >= 0) close(cli[i]);
    // 5. Close it all and give the memory back.
    _PortalStop = true;
    if (web >= 0) close(web);
    if (dns >= 0) close(dns);
    free(_ReqBuf);
    _ReqBuf = NULL;
    StaStop();
    (void)esp_wifi_set_mode(WIFI_MODE_STA);
    _Portal.Active = false;
    _PortalStop = false;
    printf("cwl net: phone setup timeline ms: phone joined %lu, first dns %lu (%lu), first page %lu (%lu), "
           "network sent %lu, connected %lu (join: assoc %lu, address %lu), closed %lu\n",
           (unsigned long)_Tl.Joined, (unsigned long)_Tl.Dns, (unsigned long)_Tl.Dnses, (unsigned long)_Tl.Page,
           (unsigned long)_Tl.Pages, (unsigned long)_Tl.Sent, (unsigned long)_Tl.Done, (unsigned long)_Tl.Assoc,
           (unsigned long)_Tl.Ip, (unsigned long)(NowMs() - _Tl.T0));
    _Tl.T0 = 0;
    printf("cwl net: phone setup closed (heap during it: free %lu, min %lu)\n", (unsigned long)_Portal.HeapFree,
           (unsigned long)_Portal.HeapMin);
}

bool CwlNet_PortalStart(void)
{
    if (_Op != kCwlNetOp_Idle) return false;
    memset(&_Portal, 0, sizeof(_Portal));
    _PortalStop = false;
    _PortalReq = false;
    return StartOp(kCwlNetOp_Portal);
}

void CwlNet_PortalStop(void)
{
    if (_Op == kCwlNetOp_Portal) _PortalStop = true;
}

void CwlNet_PortalState(CwlNetPortal_t *Out) { *Out = _Portal; }

// ---------------------------------------------------------------- console

static void Backup(bool restore)
{
    nvs_handle_t h;
    if (!restore) {
        char s[kCwlNet_SsidMax + 1], p[kCwlNet_PassMax + 1];
        CWL_GetCurrentCredentials(s, sizeof(s), p, sizeof(p));
        if (nvs_open("cwlbak", NVS_READWRITE, &h) == ESP_OK) {
            (void)nvs_set_str(h, "ssid", s);
            (void)nvs_set_str(h, "pass", p);
            (void)nvs_set_blob(h, "list", &_Saved, sizeof(_Saved));
            (void)nvs_commit(h);
            nvs_close(h);
            printf("cwl net: backed up the current network (%s) and %u saved, passwords included (not shown)\n", s, _Saved.Count);
        }
        memset(p, 0, sizeof(p));
        return;
    }
    if (nvs_open("cwlbak", NVS_READONLY, &h) != ESP_OK) { printf("cwl net: no backup\n"); return; }
    char s[kCwlNet_SsidMax + 1] = "", p[kCwlNet_PassMax + 1] = "";
    size_t n = sizeof(s);
    (void)nvs_get_str(h, "ssid", s, &n);
    n = sizeof(p);
    (void)nvs_get_str(h, "pass", p, &n);
    SavedBlob_t b;
    n = sizeof(b);
    const bool HaveList = nvs_get_blob(h, "list", &b, &n) == ESP_OK && n == sizeof(b) && b.Count <= kCwlNet_MaxSaved;
    nvs_close(h);
    if (HaveList) { _Saved = b; SaveList(); }
    memset(&b, 0, sizeof(b));
    if (s[0]) CWL_SetCurrentNetwork(s, p);
    memset(p, 0, sizeof(p));
    printf("cwl net: restored %s and %u saved\n", s, _Saved.Count);
}

int CwlNet_StatusExtra(char *Buf, int Len)
{
    char s[kCwlNet_SsidMax + 1], p[kCwlNet_PassMax + 1];
    CWL_GetCurrentCredentials(s, sizeof(s), p, sizeof(p));
    memset(p, 0, sizeof(p));
    for (char *c = s; *c; c++) if (*c == ' ') *c = '_';
    return snprintf(Buf, (size_t)Len, " net=%s saved=%u", s, _Saved.Count);
}

int CwlNet_Console(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "status";
    if (strcmp(sub, "status") == 0) {
        char s[kCwlNet_SsidMax + 1], p[kCwlNet_PassMax + 1];
        CWL_GetCurrentCredentials(s, sizeof(s), p, sizeof(p));
        const bool HavePass = p[0] != '\0';
        memset(p, 0, sizeof(p));
        static const char *const kOp[] = {"idle", "scanning", "joining", "portal"};
        char ipt[20] = "-";
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ii;
        if (sta && esp_netif_get_ip_info(sta, &ii) == ESP_OK) snprintf(ipt, sizeof(ipt), IPSTR, IP2STR(&ii.ip));
        wifi_ap_record_t apr;
        const int assoc = _Op != kCwlNetOp_Idle && esp_wifi_sta_get_ap_info(&apr) == ESP_OK;
        printf("cwl net: station %s, ip %s, disconnect reason %u, web requests %lu, dns answers %lu\n", assoc ? "joined" : "not joined",
               ipt, _DiscReason, (unsigned long)_Requests, (unsigned long)_DnsAnswers);
        printf("{ \"current\": \"%s\", \"password_set\": %d, \"saved\": %u, \"op\": \"%s\", \"last_join\": \"%s\", "
               "\"result\": \"%s\", \"assoc_ms\": %lu, \"ip_ms\": %lu, \"auto_switches\": %lu, \"portal\": %d, \"portal_ssid\": \"%s\", \"portal_left_s\": %lu, "
               "\"portal_clients\": %u, \"portal_heap_free\": %lu, \"portal_heap_min\": %lu, \"free_heap\": %lu, \"min_free_heap\": %lu }\n",
               s, HavePass, _Saved.Count, kOp[_Op % 4], _LastSsid, CwlNet_ResultText(_Result), (unsigned long)_AssocMs,
               (unsigned long)_IpMs, (unsigned long)_AutoSwitches,
               _Portal.Active, _Portal.Ssid, (unsigned long)_Portal.SecondsLeft, _Portal.Clients, (unsigned long)_Portal.HeapFree,
               (unsigned long)_Portal.HeapMin, (unsigned long)esp_get_free_heap_size(), (unsigned long)esp_get_minimum_free_heap_size());
    } else if (strcmp(sub, "list") == 0) {
        for (int i = 0; i < _Saved.Count; i++) printf("  %d  %s\n", i, _Saved.E[i].Ssid);
        printf("cwl net: %u saved (newest first)\n", _Saved.Count);
    } else if (strcmp(sub, "scan") == 0) {
        if (_Op == kCwlNetOp_Idle) {
            const uint32_t seq = _ScanSeq;
            if (!CwlNet_ScanStart()) { printf("cwl net: busy\n"); return 1; }
            for (int i = 0; i < 120 && (_ScanSeq == seq || _Op != kCwlNetOp_Idle); i++) vTaskDelay(pdMS_TO_TICKS(100));
        }
        for (int i = 0; i < _NumScan; i++)
            printf("  %-32s %4d dBm ch %2u %s%s%s\n", _Scan[i].Ssid, _Scan[i].Rssi, _ScanCh[i], _Scan[i].Secure ? "locked" : "open",
                   Find(_Scan[i].Ssid) >= 0 ? " saved" : "", _Scan[i].Current ? " current" : "");
        printf("cwl net: %d networks\n", _NumScan);
    } else if (strcmp(sub, "join") == 0 && argc >= 3) {
        const int i = atoi(argv[2]);
        if (i < 0 || i >= _Saved.Count) { printf("cwl net join <saved index>\n"); return 1; }
        char s[kCwlNet_SsidMax + 1];
        snprintf(s, sizeof(s), "%s", _Saved.E[i].Ssid);
        printf("cwl net: joining %s: %s\n", s, CwlNet_Join(s, NULL) ? "started" : "busy");
    } else if (strcmp(sub, "try") == 0 && argc >= 4) {
        // Test a network + password typed on the console (it is in the console echo: test networks only).
        printf("cwl net: trying %s: %s\n", argv[2], CwlNet_Join(argv[2], argv[3]) ? "started" : "busy");
    } else if (strcmp(sub, "forget") == 0 && argc >= 3) {
        CwlNet_Forget(atoi(argv[2]));
        printf("cwl net: %u saved\n", _Saved.Count);
    } else if (strcmp(sub, "portal") == 0) {
        _PortalLan = argc >= 3 && strcmp(argv[2], "lan") == 0;
        printf("cwl net: phone setup %s\n", CwlNet_PortalStart() ? "starting" : "busy");
    } else if (strcmp(sub, "stop") == 0) {
        CwlNet_PortalStop();
    } else if (strcmp(sub, "backup") == 0) {
        Backup(false);
    } else if (strcmp(sub, "restore") == 0) {
        Backup(true);
    } else {
        printf("usage: cwl net [status | list | scan | join <saved index> | try <ssid> <password> | forget <index> |\n"
               "                portal [lan] | stop | backup | restore]\n");
        return 1;
    }
    return 0;
}
