// cwl_online.c: Shiny Link pairing + tickets over HTTPS. See cwl_online.h.
#include "cwl_online.h"
#include "cwl_time.h"
#include "cwl.h"
#include "cwl_sec.h"
#include "cwl_wdt.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "cwl_tls.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/priv/tcpip_priv.h"
#include "lwip/tcp.h"
#include "nvs.h"

#include <stdio.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>

static const char *kNvs = "cwlrel";
static const char *kDefaultApi = "https://api.shinyrelay.com";

enum {
    kTaskStack = 8192,
    kTaskPriority = 2,
    kHttpTimeout_ms = 8000,
    kRespMax = 2048,
    kTokenMax = 768,     // link device JWT ~450 chars
    kTicketFresh_ms = 90000,     // spare tickets are refreshed after this (they live 120 s)
    kTicketRetryMin_ms = 2000,
    kTicketRetryMax_ms = 60000,
    // 0.17.6: the TLS handshake needs about 21 KB of free heap (two units, 2026-10-11:
    // it worked from 22 KB, bottoming near 850 B, and failed 72 of 72 times from 18 KB,
    // each try taking the heap down to 1-4 KB). Below this no handshake is started; the
    // call counts as failed, so the callers back off as for any failure.
    kTlsMinHeap = 21000,
};

typedef struct {
    // NVS
    char Api[96];
    char Token[kTokenMax];     // device token (secret)
    char DeviceId[24];         // 16 hex
    char RelayHost[64];        // from pairing / tickets ("host:port")
    uint32_t RelayIp;          // last resolved (host order)
    uint16_t RelayPort;
    uint8_t Room, Force;
    uint8_t RelayOverride;     // console relayhost: keep it whatever tickets say
    char Code[kSl_CodeLen + 1];
} OnlineSettings_t;

static OnlineSettings_t _Cfg;
static uint8_t _LegacyVia = 0xff;   // NVS "xport" before 0.17.0 (0xff = never set)
static SemaphoreHandle_t _Mx;
static TaskHandle_t _Task;
static volatile uint32_t _Gen = 1;

// ticket
static CwlTicket_t _Ticket;
static bool _TicketValid = false;
static volatile bool _TicketWanted = false;
static uint32_t _TicketRetryAt = 0, _TicketBackoff = kTicketRetryMin_ms;

// 0.17.7: the account's answer about public party matches, from the last
// ticket answer (-1 not known, 0 not given, 1 given), and the device's own
// POST of the player's answer (PARTY_ROOMS.md 8.4).
static volatile int _PartyConsent = -1;
static volatile bool _ConsentWanted = false;
// One device-API call for another component (lane cartflash1: the pending cart install poll
// and its result), made on THIS task so the one HTTP client is never shared.
static volatile bool _DevCallWanted = false;
static volatile int _DevCallCode = 0;            // 0 = none / running
static bool _DevCallGet;
static char _DevCallPath[128], _DevCallBody[256];   /* 1.1.0: the update check's body is about 190 bytes */
static char *_DevCallOut;
static int _DevCallCap;
// wifirecv1: receive mode's calls go with the unit's REGISTRATION token (NVS "rtok", an
// account-less device credential from POST /link_device/register), never the pairing's: a unit
// that was never paired can receive a game. The token is read from NVS for each call (no RAM copy
// beside the call's own) and never printed.
static bool _DevCallReg;
static volatile bool _RegHave;
static bool _ConsentGeneric = false;
static volatile int _ConsentResult = 0;      // 0 pending / none, else the HTTP status (-1 no network)
static volatile bool _ConsoleTicket = false; // `cwl relay ticket`: the radio comes up for it

// the app's last room instruction (lane chromshlink1)
static char _InstrMode = 'm';
static char _InstrArg[40];
static uint32_t _InstrMs;

// host code report
static char _ReportCode[kSl_CodeLen + 1];
static char _ReportedCode[kSl_CodeLen + 1];
static volatile bool _ReportWanted = false;

// pairing
static volatile CwlPairState_t _Pair = kCwlPair_Idle;
static char _PairErr[24] = "";
static char _UserCode[16] = "";
static char _PairUrl[96] = "";
static char _DeviceCode[96] = "";
static uint32_t _PairInterval_ms = 3000, _PairExpires_ms = 0, _PairNextPoll = 0;
static volatile bool _PairStartReq = false, _PairCancelReq = false;

// unpair: the token waits in RAM until the revoke call goes through
static char _RevokeToken[kTokenMax];
static volatile bool _RevokeWanted = false;

// diagnostics
static int _LastHttp = 0;
static char _LastPath[32] = "";
static uint32_t _LastHttpMs = 0, _HttpCalls = 0, _HttpErrors = 0, _Tickets = 0, _HeapHolds = 0;
static uint32_t _LastOpenMs = 0, _LastHdrMs = 0;   // connect+TLS, then request+response headers

static uint32_t NowMs(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void Lock(void) { xSemaphoreTake(_Mx, portMAX_DELAY); }
static void Unlock(void) { xSemaphoreGive(_Mx); }
static void Kick(void) { if (_Task) xTaskNotifyGive(_Task); }

// ---------------------------------------------------------------- NVS

static void LoadSettings(void)
{
    memset(&_Cfg, 0, sizeof(_Cfg));
    strcpy(_Cfg.Api, kDefaultApi);
    nvs_handle_t h;
    if (nvs_open(kNvs, NVS_READONLY, &h) != ESP_OK) return;
    size_t n;
    n = sizeof(_Cfg.Api); (void)nvs_get_str(h, "api", _Cfg.Api, &n);
    n = sizeof(_Cfg.Token); (void)nvs_get_str(h, "dtok", _Cfg.Token, &n);
    n = sizeof(_Cfg.DeviceId); (void)nvs_get_str(h, "did", _Cfg.DeviceId, &n);
    n = sizeof(_Cfg.RelayHost); (void)nvs_get_str(h, "relay", _Cfg.RelayHost, &n);
    n = sizeof(_Cfg.Code); (void)nvs_get_str(h, "code", _Cfg.Code, &n);
    (void)nvs_get_u32(h, "rip", &_Cfg.RelayIp);
    (void)nvs_get_u16(h, "rport", &_Cfg.RelayPort);
    (void)nvs_get_u8(h, "xport", &_LegacyVia);   // retired in 0.17.0, see CwlOnline_TakeLegacyVia
    (void)nvs_get_u8(h, "room", &_Cfg.Room);
    (void)nvs_get_u8(h, "force", &_Cfg.Force);
    (void)nvs_get_u8(h, "rovr", &_Cfg.RelayOverride);
    n = 0;
    _RegHave = nvs_get_str(h, "rtok", NULL, &n) == ESP_OK && n > 1;
    nvs_close(h);
    if (!CwlSec_ApiUrlOk(_Cfg.Api)) {
        // e.g. an http:// mock stored by a debug build: a release build never
        // sends a token over it (0.17.1). Use the default until set again.
        printf("cwl relay: stored api is not https://, using %s\n", kDefaultApi);
        strcpy(_Cfg.Api, kDefaultApi);
    }
    if (_Cfg.Room >= kCwlNumRoom) _Cfg.Room = kCwlRoom_App;
}

static void SaveSettings(void)
{
    nvs_handle_t h;
    if (nvs_open(kNvs, NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_str(h, "api", _Cfg.Api);
    if (_Cfg.Token[0]) (void)nvs_set_str(h, "dtok", _Cfg.Token);
    else (void)nvs_erase_key(h, "dtok");
    (void)nvs_set_str(h, "did", _Cfg.DeviceId);
    (void)nvs_set_str(h, "relay", _Cfg.RelayHost);
    (void)nvs_set_str(h, "code", _Cfg.Code);
    (void)nvs_set_u32(h, "rip", _Cfg.RelayIp);
    (void)nvs_set_u16(h, "rport", _Cfg.RelayPort);
    (void)nvs_set_u8(h, "room", _Cfg.Room);
    (void)nvs_set_u8(h, "force", _Cfg.Force);
    (void)nvs_set_u8(h, "rovr", _Cfg.RelayOverride);
    (void)nvs_commit(h);
    nvs_close(h);
}

// ---------------------------------------------------------------- HTTPS

// One client handle for the task's lifetime. A full handshake costs ~6 s on this ESP32 and takes
// the heap down by ~25 KB: api.shinyrelay.com's Let's Encrypt Gen Y chain (YE1 -> Root YE -> Root
// X2) needs three P-384 ECDSA verifications. TLS resumption would avoid it, but with
// CONFIG_MBEDTLS_DYNAMIC_BUFFER (needed: ~20 KB free heap) a resumed session corrupts the heap on
// close (IDF 5.3, tlsf assert in gcm_ctx_free), so the link task never waits on HTTPS: spare
// tickets + the seat keeper.
//
// wifimem1: receive mode KEEPS its connection between its calls (HTTP/1.1 keep-alive: the edge
// answers "Connection: keep-alive"). register, recv/start and every recv/poll that is not an offer
// leave it open; one handshake per receive session instead of one per 4-second poll. Unit 1,
// 2026-10-15, a fresh connection per poll: lowest heap 4.7-7 KB during each handshake, and every
// close by this side left a TCP PCB in TIME-WAIT for 2 x MSL (120 s), ~0.3 KB a poll that came
// back only two minutes later (the "slide"). The kept connection is closed: by any other call
// (an offer, resume, cancel, the acks), after kKeepIdle_ms without a receive call, when a cart job
// holds TLS (CwlOnline_TlsHold), on CwlOnline_Quiet (receive mode's end), and when the server
// says "Connection: close". A kept connection the server has dropped meanwhile fails before any
// answer: the call goes once more on a new connection (nothing reached the server).
static esp_http_client_handle_t _Http = NULL;
static char _HttpApi[96] = "";
static bool _HttpOpen;                 // the client holds an open connection that may be reused
static uint32_t _HttpUsedMs;           // its last call
static volatile bool _ConnClose;       // this answer said "Connection: close"
static uint32_t _Conns, _Reuses, _Retries;   // handshakes, calls over a kept connection, stale retries
static uint32_t _LastLow, _LastBefore, _LastAfter, _LastBig;   // heap of the last call (status, probe)
enum {
    kKeepIdle_ms = 12000,      // a kept connection with no receive call this long is closed (polls: every 4 s)
    kReuseMinHeap = 8000,      // a call over a kept connection (no handshake): records only
};

// POST JSON to the device API. Returns the HTTP status (or -1), body in out.
__attribute__((weak)) bool CwlOnline_TlsHold(void) { return false; }

static int Call(bool get, const char *path, const char *body, const char *bearer, char *out, int cap);
static int CallKeep(bool get, const char *path, const char *body, const char *bearer, char *out, int cap, bool keep);
static int Post(const char *path, const char *body, const char *bearer, char *out, int cap)
{
    return Call(false, path, body, bearer, out, cap);
}

// GET (no body) or POST JSON to the device API, on the online task. Returns the HTTP status
// (or -1), body in out.
static volatile int _InCall;            // calls in flight
static volatile bool _DropReq;
static int CallInner(bool get, const char *path, const char *body, const char *bearer, char *out, int cap, bool keep);
static int Call(bool get, const char *path, const char *body, const char *bearer, char *out, int cap)
{
    return CallKeep(get, path, body, bearer, out, cap, false);
}
static int CallKeep(bool get, const char *path, const char *body, const char *bearer, char *out, int cap, bool keep)
{
    _InCall++;
    const int r = CallInner(get, path, body, bearer, out, cap, keep);
    _InCall--;
    return r;
}
// The client and its connection go (the online task only, no call in flight).
static void HttpDrop(void)
{
    if (_Http != NULL && _InCall == 0) {
        esp_http_client_cleanup(_Http);   // closes the connection (TLS close_notify, socket)
        _Http = NULL;
        _HttpOpen = false;
    }
}
// A cart job: let the call in flight end (up to wait_ms), then free the kept client (its
// buffers and any kept connection) on the online task. 1 = no call in flight any more.
bool CwlOnline_Quiet(uint32_t wait_ms)
{
    if (_Task == NULL) return _InCall == 0;   // no online task: nothing kept, nothing to wait for
    _DropReq = true;
    Kick();
    uint32_t t = 0;
    for (; (_InCall > 0 || _DropReq) && t < wait_ms; t += 10) vTaskDelay(pdMS_TO_TICKS(10));
    return _InCall == 0;
}
static esp_err_t HttpEvent(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && e->header_value && !strcasecmp(e->header_key, "Date"))
        (void)CwlTime_FromHttpDate(e->header_value);   // 1.1.7: the real time from our server when nothing set the clock yet
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && e->header_value && !strcasecmp(e->header_key, "Connection") &&
        !strcasecmp(e->header_value, "close"))
        _ConnClose = true;
    return ESP_OK;
}

// TCP PCBs in TIME-WAIT now (counted on the lwIP thread): each connection this side closes
// leaves one for 2 x MSL. Diagnostics for the per-call heap line.
struct TwCall {
    struct tcpip_api_call_data call;   // first: tcpip_api_call hands this pointer back
    int n;
};
static err_t TwCount(struct tcpip_api_call_data *d)
{
    int n = 0;
    for (const struct tcp_pcb *p = tcp_tw_pcbs; p != NULL && n < 64; p = p->next) n++;
    ((struct TwCall *)d)->n = n;
    return ERR_OK;
}
static int TimeWaitPcbs(void)
{
    struct TwCall c = {.n = -1};
    (void)tcpip_api_call(TwCount, &c.call);
    return c.n;
}

// One request on the client; *reached = the server's answer started (a status line came).
static int Exchange(bool get, const char *path, const char *body, char *out, int cap, bool *reached)
{
    esp_http_client_handle_t c = _Http;
    const int blen = get ? 0 : (int)strlen(body);
    const uint32_t t0 = NowMs();
    int code = -1;
    *reached = false;
    _ConnClose = false;
    const int wdt = CwlWdt_LongBegin();   // the TLS handshake: ~6 s of ECC math without a yield (cwl_wdt.h)
    const esp_err_t oe = esp_http_client_open(c, blen);
    CwlWdt_LongEnd(wdt);
    _LastOpenMs = NowMs() - t0;
    if (oe == ESP_OK) {
        if (blen == 0 || esp_http_client_write(c, body, blen) == blen) {
            const int64_t h = esp_http_client_fetch_headers(c);
            _LastHdrMs = NowMs() - t0 - _LastOpenMs;
            code = esp_http_client_get_status_code(c);
            if (h >= 0 && code > 0) {
                *reached = true;
                int n = 0;
                while (n < cap - 1) {
                    const int r = esp_http_client_read(c, out + n, cap - 1 - n);
                    if (r <= 0) break;
                    n += r;
                }
                out[n] = '\0';
            } else {
                code = -1;
            }
        }
    }
    (void)path;
    return code;
}

static int CallInner(bool get, const char *path, const char *body, const char *bearer, char *out, int cap, bool keep)
{
    if (CwlOnline_TlsHold()) { out[0] = '\0'; return -1; }   // a cart-flash job: no TLS now
    static char url[160];
    snprintf(url, sizeof(url), "%s%s", _Cfg.Api, path);
    out[0] = '\0';
    // Every call here carries or returns a token: https:// only (0.17.1).
    if (!CwlSec_ApiUrlOk(_Cfg.Api)) return -1;
    if (_Http != NULL && strcmp(_HttpApi, _Cfg.Api) != 0) {
        esp_http_client_cleanup(_Http);
        _Http = NULL;
        _HttpOpen = false;
    }
    if (_Http != NULL && !_HttpOpen && esp_get_free_heap_size() < (uint32_t)kTlsMinHeap) {
        // gamecomp1: a closed connection's client (its buffers) is no use to the new handshake; let it go before the gate
        // (unit 1 2026-10-22: 20.6-20.8 KB free after a download, the ack held for a minute 164-400 bytes short)
        esp_http_client_cleanup(_Http);
        _Http = NULL;
    }
    const uint32_t Free = esp_get_free_heap_size();
    if (Free < (_HttpOpen ? (uint32_t)kReuseMinHeap : (uint32_t)kTlsMinHeap)) {
        if ((_HeapHolds++ & 7) == 0)
            printf("cwl relay: POST %s held: %lu bytes of free heap, the %s needs %d\n", path, (unsigned long)Free,
                   _HttpOpen ? "kept connection" : "handshake", _HttpOpen ? kReuseMinHeap : kTlsMinHeap);
        _LastHttp = -2;
        snprintf(_LastPath, sizeof(_LastPath), "%s", path);
        return -1;
    }
    if (_Http == NULL) {
        esp_http_client_config_t cfg = {
            .url = url,
            .timeout_ms = kHttpTimeout_ms,
            .buffer_size = 1024,
            .buffer_size_tx = 1536,   // the Authorization header carries a ~450-byte JWT
            .keep_alive_enable = false,   // (TCP keepalive probes: not needed, the connection is short-lived)
            .method = get ? HTTP_METHOD_GET : HTTP_METHOD_POST,
            .event_handler = HttpEvent,
        };
        if (strncmp(url, "https:", 6) == 0) cfg.crt_bundle_attach = CwlTls_Attach;
        _Http = esp_http_client_init(&cfg);
        _HttpOpen = false;
        snprintf(_HttpApi, sizeof(_HttpApi), "%s", _Cfg.Api);
    } else {
        (void)esp_http_client_set_url(_Http, url);
        (void)esp_http_client_set_method(_Http, get ? HTTP_METHOD_GET : HTTP_METHOD_POST);
    }
    esp_http_client_handle_t c = _Http;
    if (c == NULL) return -1;
    (void)esp_http_client_set_header(c, "Content-Type", "application/json");
    (void)esp_http_client_delete_header(c, "Authorization");
    if (bearer && bearer[0]) {
        const size_t al = strlen(bearer) + 8;
        char *auth = malloc(al);
        if (auth != NULL) {
            snprintf(auth, al, "Bearer %s", bearer);
            (void)esp_http_client_set_header(c, "Authorization", auth);   // copied by the client
            memset(auth, 0, al);
            free(auth);
        }
    }
    const uint32_t t0 = NowMs();
    const uint32_t big0 = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    const bool mon = heap_caps_monitor_local_minimum_free_size_start() == ESP_OK;
    _HttpCalls++;
    bool reused = _HttpOpen, reached = false;
    if (reused) _Reuses++;
    else _Conns++;
    int code = Exchange(get, path, body, out, cap, &reached);
    if (reused && !reached) {
        // the kept connection was gone (the server's idle close, a NAT timeout): nothing reached
        // the server, so the same call once more on a new connection (if the heap allows a handshake)
        esp_http_client_close(c);
        _HttpOpen = false;
        reused = false;
        if (esp_get_free_heap_size() >= (uint32_t)kTlsMinHeap) {
            _Retries++;
            _Conns++;
            code = Exchange(get, path, body, out, cap, &reached);
        }
    }
    // keep the connection only for a receive call whose whole answer was read and that the
    // server did not close
    _HttpOpen = keep && reached && !_ConnClose && esp_http_client_is_complete_data_received(c);
    if (!_HttpOpen) esp_http_client_close(c);
    _HttpUsedMs = NowMs();
    (void)esp_http_client_delete_header(c, "Authorization");
    _LastLow = mon ? (uint32_t)esp_get_minimum_free_heap_size() : 0;
    if (mon) (void)heap_caps_monitor_local_minimum_free_size_stop();
    _LastBefore = Free;
    _LastAfter = (uint32_t)esp_get_free_heap_size();
    _LastBig = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    _LastHttp = code;
    _LastHttpMs = NowMs() - t0;
    snprintf(_LastPath, sizeof(_LastPath), "%s", path);
    if (code < 200 || code >= 300) _HttpErrors++;
    printf("cwl relay: %s %s -> %d (%lu ms: open %lu, reply %lu) %s | heap before %lu (largest %lu), lowest %lu, after %lu (largest %lu), time-wait %d\n",
           get ? "GET" : "POST", path, code, (unsigned long)_LastHttpMs, (unsigned long)_LastOpenMs, (unsigned long)_LastHdrMs,
           reused ? "kept connection" : _HttpOpen ? "new connection, kept" : "new connection, closed", (unsigned long)Free,
           (unsigned long)big0, (unsigned long)_LastLow, (unsigned long)_LastAfter, (unsigned long)_LastBig, TimeWaitPcbs());
    return code;
}

static const char *JStr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static bool ResolveRelay(const char *hostport, uint32_t *ip, uint16_t *port)
{
    char host[64];
    snprintf(host, sizeof(host), "%s", hostport);
    char *colon = strrchr(host, ':');
    if (colon == NULL) return false;
    *colon = '\0';
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM};
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, colon + 1, &hints, &res) != 0 || res == NULL) return false;
    const struct sockaddr_in *a = (const struct sockaddr_in *)res->ai_addr;
    *ip = ntohl(a->sin_addr.s_addr);
    *port = ntohs(a->sin_port);
    freeaddrinfo(res);
    return true;
}

// ---------------------------------------------------------------- work items

static char _Resp[kRespMax];

static char _Tok[kTokenMax];   // the online task's copy of the device token during a call

static void DoFetchTicket(void)
{
    char *tok = _Tok;
    Lock();
    snprintf(_Tok, sizeof(_Tok), "%s", _Cfg.Token);
    Unlock();
    if (!tok[0]) { _TicketWanted = false; return; }
    const int code = Post("/link_device/ticket", "{}", tok, _Resp, sizeof(_Resp));
    memset(_Tok, 0, sizeof(_Tok));
    const uint32_t now = NowMs();
    if (code == 401) {
        // Revoked / unpaired from the account side (password reset, app unpair).
        printf("cwl relay: device token refused: unpaired\n");
        Lock();
        _Cfg.Token[0] = '\0';
        SaveSettings();
        _Gen++;
        Unlock();
        _TicketWanted = false;
        return;
    }
    cJSON *j = code == 200 ? cJSON_Parse(_Resp) : NULL;
    const char *t = j ? JStr(j, "ticket") : NULL;
    // 0.17.5: the relay of the API's own stack (a beta ticket named the prod relay)
    char relay_buf[sizeof(_Cfg.RelayHost)];
    if (CwlSec_RelayFor(_Cfg.Api, j ? JStr(j, "relay") : NULL, relay_buf, sizeof(relay_buf)))
        printf("cwl relay: ticket names %s, using %s (the API's stack)\n", j && JStr(j, "relay") ? JStr(j, "relay") : "no relay", relay_buf);
    const char *relay = relay_buf[0] ? relay_buf : NULL;
    const char *cid = j ? JStr(j, "client_id") : NULL;
    if (code == 200 && j) {
        const cJSON *pc = cJSON_GetObjectItemCaseSensitive(j, "party_consent");
        if (cJSON_IsBool(pc)) _PartyConsent = cJSON_IsTrue(pc) ? 1 : 0;
    }
    _ConsoleTicket = false;   // one try is what the console asked for
    if (t == NULL || strlen(t) > kSl_MaxTicket) {
        cJSON_Delete(j);
        _TicketRetryAt = now + _TicketBackoff;
        _TicketBackoff = _TicketBackoff * 2 > kTicketRetryMax_ms ? kTicketRetryMax_ms : _TicketBackoff * 2;
        return;
    }
    // The relay address: resolve when it changed (or first time), else the cache.
    uint32_t ip = 0;
    uint16_t port = 0;
    Lock();
    const bool same = _Cfg.RelayIp != 0 && (_Cfg.RelayOverride || (relay && strcmp(relay, _Cfg.RelayHost) == 0));
    if (same) { ip = _Cfg.RelayIp; port = _Cfg.RelayPort; }
    Unlock();
    if (!same && relay && ResolveRelay(relay, &ip, &port)) {
        Lock();
        snprintf(_Cfg.RelayHost, sizeof(_Cfg.RelayHost), "%s", relay);
        _Cfg.RelayIp = ip;
        _Cfg.RelayPort = port;
        SaveSettings();
        Unlock();
    }
    if (ip == 0) {
        cJSON_Delete(j);
        _TicketRetryAt = now + _TicketBackoff;
        return;
    }
    uint8_t mode = kSl_ModeMine;
    char rcode[kSl_CodeLen + 1] = "";
    const cJSON *room = cJSON_GetObjectItemCaseSensitive(j, "room");
    const char *m = room ? JStr(room, "mode") : NULL;
    // the app's instruction as it is, for a SHLINK game's room (lane chromshlink1):
    // "join" + code, or "room" + a game room id ("Invite my Chromatic")
    Lock();
    _InstrMode = (m && !strcmp(m, "join")) ? 'j' : (m && !strcmp(m, "room")) ? 'r' : (m && !strcmp(m, "host")) ? 'h' : 'm';
    snprintf(_InstrArg, sizeof(_InstrArg), "%s", _InstrMode == 'j' && JStr(room, "code") ? JStr(room, "code")
             : _InstrMode == 'r' && JStr(room, "room") ? JStr(room, "room") : "");
    _InstrMs = now;
    Unlock();
    if (m && !strcmp(m, "host")) mode = kSl_ModeHost;
    else if (m && !strcmp(m, "join") && JStr(room, "code") && Sl_ValidCode(JStr(room, "code"))) {
        mode = kSl_ModeJoin;
        snprintf(rcode, sizeof(rcode), "%s", JStr(room, "code"));
    }
    Lock();
    snprintf(_Ticket.ticket, sizeof(_Ticket.ticket), "%s", t);
    _Ticket.len = strlen(t);
    _Ticket.relay.ip = ip;
    _Ticket.relay.port = port;
    _Ticket.mode = mode;
    snprintf(_Ticket.code, sizeof(_Ticket.code), "%s", rcode);
    _Ticket.client_id = cid ? strtoull(cid, NULL, 16) : 0;
    _Ticket.fetched_ms = now;
    _TicketValid = true;
    _TicketWanted = false;
    _Tickets++;
    Unlock();
    _TicketBackoff = kTicketRetryMin_ms;
    memset(_Resp, 0, sizeof(_Resp));
    cJSON_Delete(j);
}

static void DoReport(void)
{
    char body[48];
    Lock();
    snprintf(body, sizeof(body), "{\"code\":\"%s\"}", _ReportCode);
    snprintf(_Tok, sizeof(_Tok), "%s", _Cfg.Token);
    Unlock();
    const int code = Post("/link_device/report", body, _Tok, _Resp, sizeof(_Resp));
    memset(_Tok, 0, sizeof(_Tok));
    if (code == 200 || code == 400 || code == 401) {
        Lock();
        snprintf(_ReportedCode, sizeof(_ReportedCode), "%s", _ReportCode);
        _ReportWanted = false;
        Unlock();
    }
}

// The player's answer to the public-match question (0.17.7), once: no retry here.
static void DoPartyConsent(void)
{
    char body[64];
    Lock();
    snprintf(body, sizeof(body), "{\"party_consent\":true,\"party_name\":\"%s\"}", _ConsentGeneric ? "generic" : "own");
    snprintf(_Tok, sizeof(_Tok), "%s", _Cfg.Token);
    Unlock();
    const int code = Post("/link_device/party_consent", body, _Tok, _Resp, sizeof(_Resp));
    memset(_Tok, 0, sizeof(_Tok));
    if (code == 200) _PartyConsent = 1;
    _ConsentResult = code == 0 ? -1 : code;
    _ConsentWanted = false;
    memset(_Resp, 0, sizeof(_Resp));
}

static void DoRevoke(void)
{
    const int code = Post("/link_device/revoke", "{}", _RevokeToken, _Resp, sizeof(_Resp));
    if (code == 200 || code == 401) {
        memset(_RevokeToken, 0, sizeof(_RevokeToken));
        _RevokeWanted = false;
    }
}

static void PairFail(const char *why)
{
    snprintf(_PairErr, sizeof(_PairErr), "%s", why);
    _DeviceCode[0] = '\0';
    _Pair = kCwlPair_Failed;
}

static void DoPairStart(void)
{
    _PairStartReq = false;
    const int code = Post("/device_link/start", "{\"label\":\"Chromatic\",\"scope\":\"link\"}", NULL, _Resp, sizeof(_Resp));
    cJSON *j = code == 200 ? cJSON_Parse(_Resp) : NULL;
    const char *dc = j ? JStr(j, "device_code") : NULL;
    const char *uc = j ? JStr(j, "user_code") : NULL;
    const char *uri = j ? JStr(j, "verification_uri_complete") : NULL;
    if (dc == NULL || uc == NULL) {
        cJSON_Delete(j);
        PairFail(code < 0 ? "NO  INTERNET" : "SERVER  ERROR");
        return;
    }
    const cJSON *iv = cJSON_GetObjectItemCaseSensitive(j, "interval");
    const cJSON *ex = cJSON_GetObjectItemCaseSensitive(j, "expires_in");
    const uint32_t now = NowMs();
    Lock();
    snprintf(_DeviceCode, sizeof(_DeviceCode), "%s", dc);
    snprintf(_UserCode, sizeof(_UserCode), "%s", uc);
    // The QR names the public page (shinygen.ai/link?code=...), whatever host
    // the backend said (a beta backend says beta.shinygen.ai).
    if (strcmp(_Cfg.Api, kDefaultApi) == 0) {
        // Production device API: the approval page is shinygen.ai/link (the
        // backend can still name beta.shinygen.ai after a color swap, which
        // approves too but reads oddly on a QR).
        char norm[16] = "";
        for (const char *q = uc; *q && strlen(norm) < sizeof(norm) - 1; q++) {
            if (*q != '-') { const size_t l = strlen(norm); norm[l] = *q; norm[l + 1] = '\0'; }
        }
        snprintf(_PairUrl, sizeof(_PairUrl), "https://shinygen.ai/link?code=%s", norm);
    } else {
        snprintf(_PairUrl, sizeof(_PairUrl), "%s", uri ? uri : "https://shinygen.ai/link");
    }
    _PairInterval_ms = (cJSON_IsNumber(iv) && iv->valueint > 0 ? (uint32_t)iv->valueint : 3) * 1000u;
    _PairExpires_ms = now + (cJSON_IsNumber(ex) && ex->valueint > 0 ? (uint32_t)ex->valueint : 600) * 1000u;
    _PairNextPoll = now + _PairInterval_ms;
    _Pair = kCwlPair_Waiting;
    Unlock();
    cJSON_Delete(j);
}

static void DoPairPoll(void)
{
    const uint32_t now = NowMs();
    if ((int32_t)(now - _PairExpires_ms) >= 0) { PairFail("CODE  EXPIRED"); return; }
    if ((int32_t)(now - _PairNextPoll) < 0) return;
    _PairNextPoll = now + _PairInterval_ms;
    char body[160];
    snprintf(body, sizeof(body), "{\"device_code\":\"%s\",\"label\":\"Chromatic\"}", _DeviceCode);
    const int code = Post("/device_link/poll", body, NULL, _Resp, sizeof(_Resp));
    if (code != 200) return;   // transient: poll again
    cJSON *j = cJSON_Parse(_Resp);
    const char *st = j ? JStr(j, "status") : NULL;
    if (st == NULL || !strcmp(st, "pending")) { cJSON_Delete(j); return; }
    if (!strcmp(st, "approved")) {
        const char *tok = JStr(j, "device_token");
        const char *did = JStr(j, "device_id");
        char relay_buf[sizeof(_Cfg.RelayHost)];
        (void)CwlSec_RelayFor(_Cfg.Api, JStr(j, "relay"), relay_buf, sizeof(relay_buf));
        const char *relay = relay_buf[0] ? relay_buf : NULL;
        if (tok == NULL || strlen(tok) >= kTokenMax || did == NULL) {
            cJSON_Delete(j);
            PairFail("SERVER  ERROR");
            return;
        }
        Lock();
        snprintf(_Cfg.Token, sizeof(_Cfg.Token), "%s", tok);
        snprintf(_Cfg.DeviceId, sizeof(_Cfg.DeviceId), "%s", did);
        if (relay) snprintf(_Cfg.RelayHost, sizeof(_Cfg.RelayHost), "%s", relay);
        _Cfg.RelayIp = 0;
        SaveSettings();
        _TicketValid = false;
        _Gen++;
        _Pair = kCwlPair_Done;
        _DeviceCode[0] = '\0';
        Unlock();
        memset(_Resp, 0, sizeof(_Resp));
        printf("cwl relay: paired (device %s)\n", did);
        _TicketWanted = true;   // first ticket right away
    } else if (!strcmp(st, "denied")) {
        PairFail("DENIED");
    } else {
        PairFail("CODE  EXPIRED");   // expired / consumed / unknown
    }
    cJSON_Delete(j);
}

// wifimem1: the receive calls that keep their connection open for the next one.
static bool RecvKeepPath(const char *p)
{
    return !strcmp(p, "/link_device/register") || !strcmp(p, "/link_device/recv/start") || !strcmp(p, "/link_device/recv/poll");
}
// The kept connection goes now (the online task, between calls).
static void RecvRelease(const char *why)
{
    if (_Http == NULL || !_HttpOpen || _InCall != 0) return;
    const uint32_t f0 = (uint32_t)esp_get_free_heap_size();
    HttpDrop();
    printf("cwl relay: kept connection closed (%s): free heap %lu -> %lu (largest %lu)\n", why, (unsigned long)f0,
           (unsigned long)esp_get_free_heap_size(), (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}

// Bench (`cwl relay rxprobe N [MS] [fresh]`): N receive-style calls, MS apart (4000), in whatever
// hold the caller arranged (cf_dev `cart rxprobe` puts receive mode's link hold around it). POST
// /link_device/recv/poll {"code":"BCDFGH"} with NO token (none is sent anywhere: a unit with no
// registration still makes the full call; production answers 404, a receive server "expired").
// "fresh" closes every connection as before 1.0.3-rc3 (one handshake per call). Every call prints
// its heap line; nothing secret is sent or printed.
static volatile int _ProbeLeft;
static volatile uint32_t _ProbeEvery = 4000;
static volatile bool _ProbeFresh;
static uint32_t _ProbeNext, _ProbeN;
static void DoProbe(void)
{
    const uint32_t now = NowMs();
    if ((int32_t)(now - _ProbeNext) < 0) return;
    _ProbeNext = now + _ProbeEvery;
    char out[256];
    const int code = CallKeep(false, "/link_device/recv/poll", "{\"code\":\"BCDFGH\"}", NULL, out, sizeof(out), !_ProbeFresh);
    memset(out, 0, sizeof(out));
    if (!_HttpOpen) HttpDrop();
    _ProbeN++;
    printf("cwl relay: rxprobe %lu -> %d | free %lu largest %lu | call lowest %lu | conns %lu reuses %lu retries %lu\n", (unsigned long)_ProbeN, code,
           (unsigned long)esp_get_free_heap_size(), (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT), (unsigned long)_LastLow,
           (unsigned long)_Conns, (unsigned long)_Reuses, (unsigned long)_Retries);
    if (--_ProbeLeft <= 0) {
        _ProbeLeft = 0;
        RecvRelease("probe done");
        HttpDrop();
        printf("cwl relay: rxprobe done (%lu calls)\n", (unsigned long)_ProbeN);
    }
}

// wifirecv1: one receive-mode call (the online task). /link_device/register goes with no token
// and its answer stays here: the token goes to NVS and the caller gets "{}".
static int RecvCall(void)
{
    if (!strcmp(_DevCallPath, "/link_device/register")) {
        const int code = CallKeep(false, _DevCallPath, "{}", NULL, _Resp, sizeof(_Resp), true);   // recv/start follows
        int r = code;
        if (code == 200) {
            cJSON *j = cJSON_Parse(_Resp);
            char *t = j ? (char *)JStr(j, "device_token") : NULL;
            const char *d = j ? JStr(j, "device_id") : NULL;
            bool ok = false;
            if (t && d && strlen(t) < kTokenMax && strlen(d) < sizeof(_Cfg.DeviceId)) {
                nvs_handle_t h;
                if (nvs_open(kNvs, NVS_READWRITE, &h) == ESP_OK) {
                    ok = nvs_set_str(h, "rtok", t) == ESP_OK && nvs_set_str(h, "rdid", d) == ESP_OK && nvs_commit(h) == ESP_OK;
                    nvs_close(h);
                }
                if (ok) printf("cwl relay: registered for receiving games (device %s)\n", d);
            }
            if (t) memset(t, 0, strlen(t));
            cJSON_Delete(j);
            _RegHave = ok;
            if (!ok) r = 502;
        }
        memset(_Resp, 0, sizeof(_Resp));
        snprintf(_DevCallOut, _DevCallCap, "{}");
        return r;
    }
    nvs_handle_t h;
    size_t n = sizeof(_Tok);
    bool have = false;
    if (nvs_open(kNvs, NVS_READONLY, &h) == ESP_OK) {
        have = nvs_get_str(h, "rtok", _Tok, &n) == ESP_OK && _Tok[0];
        nvs_close(h);
    }
    if (!have) { memset(_Tok, 0, sizeof(_Tok)); _RegHave = false; return 401; }
    // wifimem1: recv/start and recv/poll keep the connection for the next poll (one handshake per
    // receive session); everything else (resume, cancel, the acks) closes it after itself
    const bool keep = RecvKeepPath(_DevCallPath);
    const int code = CallKeep(_DevCallGet, _DevCallPath, _DevCallBody, _Tok, _DevCallOut, _DevCallCap, keep);
    memset(_Tok, 0, sizeof(_Tok));
    // an offer: the kept connection stays for the 'downloading' report (no second handshake, ~5 s), which
    // closes it after itself (not a receive path); the header read beside it needs 14 KB (unit 1: ~23 KB free)
    // without a kept connection the client and its buffers (~3 KB) are not kept between calls
    // either, so a handshake finds them free (unit 1, 2026-10-14: polls held at 20.6 KB)
    if (!_HttpOpen) HttpDrop();
    return code;
}

static bool Online(void) { return CWL_IsOnline(); }

static void OnlineTask(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
        if (_DropReq) {                  // CwlOnline_Quiet: no call is running on this task now
            if (_HttpOpen) RecvRelease("receive mode ended");
            HttpDrop();
            if (_Http == NULL || _InCall == 0) _DropReq = false;
        }
        // a kept connection (receive mode): closed when its polls stop or a cart job holds TLS
        if (_HttpOpen && !_DevCallWanted && _ProbeLeft == 0 && (int32_t)(NowMs() - _HttpUsedMs) >= kKeepIdle_ms) RecvRelease("idle");
        {   // a cart job (or the download) holds TLS: the kept connection goes once the hold has lasted 3 s
            // (the header read's short plain connection does not end it: the 'downloading' report reuses it)
            static uint32_t holdSince;
            if (!CwlOnline_TlsHold()) holdSince = 0;
            else if (!holdSince) holdSince = NowMs() | 1u;
            if (_HttpOpen && holdSince && (int32_t)(NowMs() - holdSince) >= 3000) RecvRelease("a cart job");
        }
        if (_PairCancelReq) {
            _PairCancelReq = false;
            _PairStartReq = false;
            _DeviceCode[0] = '\0';
            if (_Pair != kCwlPair_Done) _Pair = kCwlPair_Idle;
        }
        if (!Online()) continue;
        if (_RevokeWanted) DoRevoke();
        if (_PairStartReq) DoPairStart();
        if (_Pair == kCwlPair_Waiting) DoPairPoll();
        if (_ReportWanted && _Cfg.Token[0]) DoReport();
        if (_DevCallWanted) {
            int code = 401;
            if (_DevCallReg) code = RecvCall();
            else if (_Cfg.Token[0]) {
                /* lane mygames1: GET GAMES > MY GAMES's pages keep receive mode's connection (one handshake for a page turn) */
                const bool keep = !strncmp(_DevCallPath, "/link_device/my_games", 21);
                code = CallKeep(_DevCallGet, _DevCallPath, _DevCallBody, _Cfg.Token, _DevCallOut, _DevCallCap, keep);
                if (!_HttpOpen) HttpDrop();
            }
            _DevCallCode = code ? code : -1;
            _DevCallWanted = false;
        }
        if (_ConsentWanted) {
            if (_Cfg.Token[0]) DoPartyConsent();
            else { _ConsentResult = 401; _ConsentWanted = false; }
        }
        if (_TicketWanted && _Cfg.Token[0] && (int32_t)(NowMs() - _TicketRetryAt) >= 0) DoFetchTicket();
        if (_ProbeLeft > 0 && !_DevCallWanted) DoProbe();
    }
}

// ---------------------------------------------------------------- API

void CwlOnline_Init(void)
{
    _Mx = xSemaphoreCreateMutex();
    CwlWdt_Init();
    LoadSettings();
}

void CwlOnline_Start(void)
{
    xTaskCreatePinnedToCore(OnlineTask, "cwl_online", kTaskStack, NULL, kTaskPriority, &_Task, 1);
}

bool CwlOnline_Paired(void) { return _Cfg.Token[0] != '\0'; }

size_t CwlOnline_CopyToken(char *out, size_t cap)
{
    if (out == NULL || cap == 0) return 0;
    out[0] = '\0';
    if (_Mx == NULL) return 0;
    Lock();
    const size_t n = strlen(_Cfg.Token);
    if (n > 0 && n < cap) memcpy(out, _Cfg.Token, n + 1);
    Unlock();
    return (n > 0 && n < cap) ? n : 0;
}

const char *CwlOnline_Api(void) { return _Cfg.Api; }
const char *CwlOnline_DeviceId(void) { return _Cfg.DeviceId; }

char CwlOnline_Instruction(char *arg, size_t cap, uint32_t *age_ms)
{
    Lock();
    const char m = _InstrMs ? _InstrMode : 0;
    if (arg && cap) snprintf(arg, cap, "%s", _InstrArg);
    if (age_ms) *age_ms = NowMs() - _InstrMs;
    Unlock();
    return m;
}
bool CwlOnline_ForceRelay(void) { return _Cfg.Force != 0; }
CwlRoom_t CwlOnline_Room(void) { return (CwlRoom_t)_Cfg.Room; }
const char *CwlOnline_JoinCode(void) { return _Cfg.Code; }
uint32_t CwlOnline_Generation(void) { return _Gen; }

uint8_t CwlOnline_TakeLegacyVia(void)
{
    const uint8_t v = _LegacyVia;
    _LegacyVia = 0;
    if (v != 0xff) {
        nvs_handle_t h;
        if (nvs_open(kNvs, NVS_READWRITE, &h) == ESP_OK) {
            (void)nvs_erase_key(h, "xport");
            (void)nvs_commit(h);
            nvs_close(h);
        }
    }
    return v == 0xff ? 0 : v;
}

void CwlOnline_SetForceRelay(bool on)
{
    Lock(); _Cfg.Force = on ? 1 : 0; SaveSettings(); _Gen++; Unlock();
}

void CwlOnline_SetRoom(CwlRoom_t r, const char *code)
{
    if (r >= kCwlNumRoom) return;
    Lock();
    _Cfg.Room = (uint8_t)r;
    if (r == kCwlRoom_Join && code) {
        snprintf(_Cfg.Code, sizeof(_Cfg.Code), "%s", code);
        for (char *p = _Cfg.Code; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    }
    SaveSettings();
    _Gen++;
    Unlock();
}

void CwlOnline_WantTicket(void)
{
    if (!CwlOnline_Paired() || _TicketWanted) return;
    _TicketWanted = true;
    Kick();
}

bool CwlOnline_HaveTicket(uint32_t now_ms)
{
    return _TicketValid && (now_ms - _Ticket.fetched_ms) < kTicketFresh_ms;
}

bool CwlOnline_TakeTicket(CwlTicket_t *out, uint32_t now_ms)
{
    bool ok = false;
    Lock();
    if (_TicketValid && (now_ms - _Ticket.fetched_ms) < kRelay_TicketMaxAge_ms) {
        *out = _Ticket;
        ok = true;
    }
    _TicketValid = false;
    memset(_Ticket.ticket, 0, sizeof(_Ticket.ticket));
    Unlock();
    // Device-side room choice overrides the app's instruction.
    if (ok && _Cfg.Room != kCwlRoom_App) {
        out->mode = _Cfg.Room == kCwlRoom_Host ? kSl_ModeHost : _Cfg.Room == kCwlRoom_Join ? kSl_ModeJoin : kSl_ModeMine;
        snprintf(out->code, sizeof(out->code), "%s", _Cfg.Room == kCwlRoom_Join ? _Cfg.Code : "");
    }
    if (ok && out->client_id == 0) out->client_id = strtoull(_Cfg.DeviceId, NULL, 16);
    return ok;
}

void CwlOnline_ReportCode(const char *code)
{
    if (code == NULL || !Sl_ValidCode(code)) return;
    Lock();
    const bool fresh = strcmp(code, _ReportedCode) != 0;
    if (fresh) {
        snprintf(_ReportCode, sizeof(_ReportCode), "%s", code);
        _ReportWanted = true;
    }
    Unlock();
    if (fresh) Kick();
}

void CwlOnline_PairStart(void)
{
    _PairErr[0] = '\0';
    _UserCode[0] = '\0';
    _Pair = kCwlPair_Starting;
    _PairStartReq = true;
    Kick();
    CWL_Wake();
}

void CwlOnline_PairCancel(void)
{
    _PairCancelReq = true;
    Kick();
}

void CwlOnline_Unpair(void)
{
    Lock();
    if (_Cfg.Token[0]) {
        snprintf(_RevokeToken, sizeof(_RevokeToken), "%s", _Cfg.Token);
        _RevokeWanted = true;
    }
    memset(_Cfg.Token, 0, sizeof(_Cfg.Token));
    _Cfg.DeviceId[0] = '\0';
    SaveSettings();
    _TicketValid = false;
    memset(_Ticket.ticket, 0, sizeof(_Ticket.ticket));
    _Pair = kCwlPair_Idle;
    _Gen++;
    Unlock();
    Kick();
    CWL_Wake();
}

CwlPairState_t CwlOnline_PairState(void) { return _Pair; }
const char *CwlOnline_PairError(void) { return _PairErr; }

void CwlOnline_PairCode(char *code, size_t code_len, char *url, size_t url_len)
{
    Lock();
    if (code) snprintf(code, code_len, "%s", _UserCode);
    if (url) snprintf(url, url_len, "%s", _PairUrl);
    Unlock();
}

bool CwlOnline_NeedsRadio(void)
{
    return _Pair == kCwlPair_Starting || _Pair == kCwlPair_Waiting || _RevokeWanted || _ReportWanted ||
           _PairStartReq || _ConsentWanted || _DevCallWanted || (_ConsoleTicket && _TicketWanted) || _ProbeLeft > 0;
}

void CwlOnline_StatusJson(char *buf, size_t len)
{
    static const char *const kR[] = {"app", "mine", "host", "join"};
    static const char *const kP[] = {"idle", "starting", "waiting", "done", "failed"};
    const uint32_t now = NowMs();
    snprintf(buf, len,
             "\"paired\": %d, \"device_id\": \"%s\", \"api\": \"%s\", \"relay_host\": \"%s%s\", \"relay_ip\": \"%lu.%lu.%lu.%lu:%u\", "
             "\"room\": \"%s\", \"join_code\": \"%s\", \"force_relay\": %d, \"pair\": \"%s\", \"pair_err\": \"%s\", "
             "\"ticket_fresh\": %d, \"ticket_age_ms\": %ld, \"tickets\": %lu, \"http_calls\": %lu, \"http_errors\": %lu, "
             "\"last_http\": %d, \"last_path\": \"%s\", \"last_http_ms\": %lu, \"heap_holds\": %lu, \"reported\": \"%s\", "
             "\"https_conns\": %lu, \"https_reuses\": %lu, \"https_retries\": %lu, \"https_kept\": %d, \"last_heap\": [%lu, %lu, %lu, %lu], "
             "\"user_code\": \"%s\", \"pair_url\": \"%s\"",
             CwlOnline_Paired(), _Cfg.DeviceId, _Cfg.Api, _Cfg.RelayHost, _Cfg.RelayOverride ? " (override)" : "", (unsigned long)(_Cfg.RelayIp >> 24),
             (unsigned long)((_Cfg.RelayIp >> 16) & 255), (unsigned long)((_Cfg.RelayIp >> 8) & 255),
             (unsigned long)(_Cfg.RelayIp & 255), (unsigned)_Cfg.RelayPort, kR[_Cfg.Room % 4], _Cfg.Code,
             _Cfg.Force, kP[_Pair % 5], _PairErr, CwlOnline_HaveTicket(now), _TicketValid ? (long)(now - _Ticket.fetched_ms) : -1L,
             (unsigned long)_Tickets, (unsigned long)_HttpCalls, (unsigned long)_HttpErrors, _LastHttp, _LastPath,
             (unsigned long)_LastHttpMs, (unsigned long)_HeapHolds, _ReportedCode,
             (unsigned long)_Conns, (unsigned long)_Reuses, (unsigned long)_Retries, _HttpOpen ? 1 : 0, (unsigned long)_LastBefore,
             (unsigned long)_LastLow, (unsigned long)_LastAfter, (unsigned long)_LastBig,
             _Pair == kCwlPair_Waiting ? _UserCode : "", _Pair == kCwlPair_Waiting ? _PairUrl : "");
}

int CwlOnline_Console(int argc, char **argv)
{
    // argv[0] = "relay"
    const char *sub = argc > 1 ? argv[1] : "";
    if (!strcmp(sub, "pair")) {
        CwlOnline_PairStart();
        printf("cwl relay: pairing started (watch `cwl relay status` for the code)\n");
    } else if (!strcmp(sub, "cancel")) {
        CwlOnline_PairCancel();
    } else if (!strcmp(sub, "unpair")) {
        CwlOnline_Unpair();
        printf("cwl relay: unpaired (the token is revoked once online)\n");
    } else if (!strcmp(sub, "mode") && argc >= 3) {
        // Retired in 0.17.0: the link cable mode says it.
        printf("cwl relay mode: retired, use `cwl mode auto|nearby|online|off` (relay only, no P2P: `cwl relay force on`)\n");
        return 1;
    } else if (!strcmp(sub, "room") && argc >= 3) {
        const char *m = argv[2];
        if (!strcmp(m, "app")) CwlOnline_SetRoom(kCwlRoom_App, NULL);
        else if (!strcmp(m, "mine")) CwlOnline_SetRoom(kCwlRoom_Mine, NULL);
        else if (!strcmp(m, "host")) CwlOnline_SetRoom(kCwlRoom_Host, NULL);
        else if (!strcmp(m, "join") && argc >= 4) {
            char c[kSl_CodeLen + 2];
            snprintf(c, sizeof(c), "%s", argv[3]);
            for (char *p = c; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
            if (!Sl_ValidCode(c)) { printf("cwl relay: bad code (6 of %s)\n", kSl_CodeAlphabet); return 1; }
            CwlOnline_SetRoom(kCwlRoom_Join, c);
        } else return 1;
        printf("cwl relay: room %s\n", m);
    } else if (!strcmp(sub, "force") && argc >= 3) {
        CwlOnline_SetForceRelay(!strcmp(argv[2], "on") || !strcmp(argv[2], "1"));
        printf("cwl relay: force relay %s\n", CwlOnline_ForceRelay() ? "on" : "off");
    } else if (!strcmp(sub, "api") && argc >= 3) {
        const char *api = strcmp(argv[2], "default") ? argv[2] : kDefaultApi;
        if (!CwlSec_ApiUrlOk(api)) {
            printf("cwl relay: refused: the API carries the device token, so it must be https://host%s\n",
                   CwlSec_HttpAllowed() ? " or http://host" : " (http:// only in a debug build)");
            return 1;
        }
        Lock();
        snprintf(_Cfg.Api, sizeof(_Cfg.Api), "%s", api);
        // 0.17.5: the relay follows the API's stack at once (the next ticket
        // re-resolves it), unless `cwl relay relayhost` pinned one
        if (!_Cfg.RelayOverride) {
            const char *sr = CwlSec_StackRelay(api);
            if (sr) snprintf(_Cfg.RelayHost, sizeof(_Cfg.RelayHost), "%s", sr);
            _Cfg.RelayIp = 0;
            _Cfg.RelayPort = 0;
            _TicketValid = false;   // a ticket from the old API is for the old relay
        }
        SaveSettings();
        Unlock();
        printf("cwl relay: api %s, relay %s%s\n", _Cfg.Api, _Cfg.RelayHost, _Cfg.RelayOverride ? " (override)" : "");
    } else if (!strcmp(sub, "relayhost") && argc >= 3) {
        // Debug override of the relay the tickets name ("default" clears it).
        uint32_t ip = 0; uint16_t port = 0;
        const bool clear = !strcmp(argv[2], "default");
        if (!clear && !ResolveRelay(argv[2], &ip, &port)) { printf("cannot resolve %s\n", argv[2]); return 1; }
        Lock();
        snprintf(_Cfg.RelayHost, sizeof(_Cfg.RelayHost), "%s", clear ? "" : argv[2]);
        _Cfg.RelayIp = ip; _Cfg.RelayPort = port;
        _Cfg.RelayOverride = clear ? 0 : 1;
        SaveSettings();
        _Gen++;
        Unlock();
    } else if (!strcmp(sub, "rxprobe") && argc >= 3) {
        // bench: receive-mode call series (see DoProbe); `cwl relay rxprobe 0` stops it
        const int n = atoi(argv[2]);
        _ProbeEvery = (argc >= 4 && atoi(argv[3]) >= 500) ? (uint32_t)atoi(argv[3]) : 4000u;
        _ProbeFresh = argc >= 5 && !strcmp(argv[4], "fresh");
        _ProbeN = 0;
        _ProbeNext = NowMs();
        _ProbeLeft = n > 0 ? n : 0;
        printf("cwl relay: rxprobe %d calls every %lu ms, %s\n", n, (unsigned long)_ProbeEvery, _ProbeFresh ? "a new connection each" : "connection kept");
        Kick();
    } else if (!strcmp(sub, "ticket")) {
        // 0.17.7: like a game's network use, a console ticket brings a dormant
        // radio up (CwlOnline_NeedsRadio) for its one try.
        _ConsoleTicket = true;
        CwlOnline_WantTicket();
    } else {
        return -1;   // not ours (status etc. live in cwl_esp.c)
    }
    return 0;
}

bool CwlOnline_DeviceCallStart(bool Get, const char *Path, const char *Body, char *Out, int Cap)
{
    if (!CwlOnline_Paired() || _DevCallWanted || !Path || !Out || Cap < 2) return false;
    snprintf(_DevCallPath, sizeof(_DevCallPath), "%s", Path);
    snprintf(_DevCallBody, sizeof(_DevCallBody), "%s", Body ? Body : "{}");
    _DevCallGet = Get;
    _DevCallReg = false;
    _DevCallOut = Out;
    _DevCallCap = Cap;
    _DevCallCode = 0;
    _DevCallWanted = true;
    Kick();
    return true;
}

bool CwlOnline_RecvCallStart(const char *Path, const char *Body, char *Out, int Cap)
{
    if (_DevCallWanted || !Path || !Out || Cap < 2) return false;
    snprintf(_DevCallPath, sizeof(_DevCallPath), "%s", Path);
    snprintf(_DevCallBody, sizeof(_DevCallBody), "%s", Body ? Body : "{}");
    _DevCallGet = false;
    _DevCallReg = true;
    _DevCallOut = Out;
    _DevCallCap = Cap;
    _DevCallCode = 0;
    _DevCallWanted = true;
    Kick();
    return true;
}

bool CwlOnline_Registered(void) { return _RegHave; }

void CwlOnline_RecvForget(void)
{
    nvs_handle_t h;
    if (nvs_open(kNvs, NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_erase_key(h, "rtok");
        (void)nvs_erase_key(h, "rdid");
        (void)nvs_commit(h);
        nvs_close(h);
    }
    _RegHave = false;
}

bool CwlOnline_DeviceCallBusy(void) { return _DevCallWanted; }

int CwlOnline_DeviceCallResult(void)
{
    if (_DevCallWanted) return 0;
    const int c = _DevCallCode;
    _DevCallCode = 0;
    return c;
}

int CwlOnline_PartyConsent(void) { return _PartyConsent; }

bool CwlOnline_PostPartyConsent(bool Generic)
{
    if (!CwlOnline_Paired() || _ConsentWanted) return false;
    _ConsentGeneric = Generic;
    _ConsentResult = 0;
    _ConsentWanted = true;
    Kick();
    return true;
}

bool CwlOnline_PartyConsentResult(int *pHttp)
{
    if (_ConsentWanted || _ConsentResult == 0) return false;
    *pHttp = _ConsentResult;
    _ConsentResult = 0;
    return true;
}

void CwlOnline_DropTicket(void)
{
    Lock();
    _TicketValid = false;
    memset(_Ticket.ticket, 0, sizeof(_Ticket.ticket));
    Unlock();
}
