// nw_dev.c: network window service on the device. See nw_dev.h.
#ifdef NW_CARTFLASH
#include "cf_esp.h"
cf_esp_t *CfDev_Engine(void);
int CfDev_HttpWork(void);
void CfDev_Init(void);
void CfDev_Tick(void);
void CfDev_XsStatus(const uint8_t *p, uint8_t n);
#endif
#include "nw_dev.h"
#include "nw_esp.h"
#include "nw_link.h"
#include "shl_client.h"
#include "party_ask.h"
#include "esp_system.h"
#include "lwip/sockets.h"
#include <fcntl.h>

#include "cwl.h"
#include "cwl_time.h"
#include "cwl_online.h"
#include "cwl_net.h"
#include "cwl_sec.h"
#include "cwl_wdt.h"
#include "crc8_sae_j1850.h"
#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_crt_bundle.h"
#include "cwl_tls.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "NW";
static const char *kNvs = "nw";

enum {
#ifdef NW_CARTFLASH
    kTaskStack = 6144,               // cf_esp plans with 512-byte buffers + partition reads
#else
    kTaskStack = 4096,
#endif
    kTaskPriority = 5,           // below the CWL link task (6)
    kHttpStack = 6144,
    kHttpPriority = 4,
    kQueueLen = 24,
    kTick_ms = 5,
    kHttpTimeout_ms = 8000,      // under the service's 10 s call timeout
    kBodyMax = NW_RX_SIZE - NW_RESP_HDR,   // 4086
    kHandles = 4,
    kUartWait_ms = 60,           // let earlier packets drain (one 255-byte packet: 22 ms)
    kLinkChunk = 32,             // RXDATA chunk while the link cable is busy (UART shared)
};

typedef enum { kMsg_Packet, kMsg_Complete, kMsg_CartXs } MsgKind_t;

typedef struct {
    uint8_t Kind;
    uint8_t Len;
    uint8_t Data[255];
} Msg_t;

// One call in flight at a time (the mailbox enforces it).
typedef struct {
    uint8_t Id;
    uint8_t Op;
    uint8_t GameId[16];
    uint16_t GameVer;
    char Path[204];
    uint16_t BodyLen;
    uint8_t Body[NW_TX_SIZE];
} Job_t;

typedef struct {
    uint8_t Id, Status, Handle;
    uint32_t Total;
    uint16_t Len;
    int8_t Notify;   // X-Gbnet-Notify of the answer, -1 none
} Result_t;

typedef struct {
    bool Used;
    uint8_t GameId[16];
    char Path[220];              // relative to the base URL
    uint32_t Total;
} Handle_t;

// settings
static char _Url[128];
static uint8_t _Enabled = 1;

static nw_esp_t _E;
static QueueHandle_t _Q, _JobQ;
static TaskHandle_t _Task, _HttpTask;
static Job_t _Job;                // handed to the HTTP task by value through _JobQ (id only)
static volatile uint8_t _CancelId = 0xFF;
static uint8_t _Out[kBodyMax];    // HTTP result body
static Result_t _Res;
static Handle_t _Handles[kHandles];
static uint8_t _NextHandle = 0;
static bool _Started = false;
static volatile bool _HaveWindow = false;
static volatile bool _SawWindow = false;   // a 0x0B packet arrived: this FPGA has the window

// stats / debug
static uint32_t _PktIn, _PktOut, _PktDrop, _HttpCalls, _HttpErr;
static uint8_t _LastReq[48], _LastReqLen;
static uint16_t _LastReqFull;
static uint8_t _LastResp[48], _LastRespLen;
static int _LastHttpStatus;
static uint32_t _LastHttpMs;
static char _LastTitle[17];

static uint32_t NowMs(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---------------------------------------------------------------- settings

static void Load(void)
{
    nvs_handle_t h;
    if (nvs_open(kNvs, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof(_Url);
    (void)nvs_get_str(h, "url", _Url, &n);
    // An http:// gateway (a LAN mock, stored by a debug build) would get the
    // device token in clear text: a release build falls back to the paired
    // API host (0.17.1).
    if (_Url[0] && !CwlSec_ApiUrlOk(_Url)) _Url[0] = '\0';   // (CwlSec_GatewayBase then uses the paired API host)
    (void)nvs_get_u8(h, "enabled", &_Enabled);
    nvs_close(h);
}

static void Save(void)
{
    nvs_handle_t h;
    if (nvs_open(kNvs, NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_str(h, "url", _Url);
    (void)nvs_erase_key(h, "token");   // the old second token: the pairing's is the one now
    (void)nvs_set_u8(h, "enabled", _Enabled);
    (void)nvs_commit(h);
    nvs_close(h);
}

// ---------------------------------------------------------------- SHLINK
// A Shiny Gen GBC game's multiplayer room (lane chromshlink1): the window's
// LINK_* ops are answered here every frame (nw_link.c, never HTTPS) and the
// room client (shl_client.c) talks UDP to Shiny Relay on its own socket.
// Everything runs on the nw task; the OSD only posts requests.
//
// Heap only while needed (0.17.0, ~3.8 KB): the room client and its buffer are
// allocated at a game's LINK_JOIN and freed, with the UDP socket, once no game
// is attached and no room is live (Idle / Ended). Everything else (the link
// cable's relay, TLS ticket fetches) keeps that RAM.
typedef struct {
    ShlClient_t c;
    // one buffer for the two things the tick does in turn: datagrams in, a ticket
    union {
        uint8_t rx[1100];
        CwlTicket_t t;
    } buf;
} ShlMem_t;
static ShlMem_t *_Sm;                     // NULL: no SHLINK game, no room
static NwLink_t _Link;
static NwRoomInfo_t _RoomInfo;            // snapshot for the OSD (written on the nw task)
static uint64_t _ShlClientId;             // kept across allocations (rejoin by the same id)
static uint32_t _ShlAllocs, _ShlAllocFails;
static ShlStats_t _ShlStats;              // snapshot for `nw status`
static int _ShlSock = -1;
static volatile int _RoomReq = -1;       // a kShl_Mode*, or 100 = leave
static char _RoomReqArg[kShl_RoomIdMax + 1];
static volatile bool _MenuWanted;         // the game asked (lk_ask): show the room menu
static volatile bool _DlWanted;           // the install machine wants SYSTEM > DOWNLOAD on the screen (wifirecv1)
static volatile bool _GgWanted;           // lane ui110dev: SYSTEM > GET GAMES wanted on the screen (its install's result)
static volatile bool _PlayWanted;         // SYSTEM > PLAY wanted on the screen (1.1.0: a play send started, `rc page show`)
static volatile bool _FwWanted;           // the bench wants SYSTEM > UPDATE on the screen (1.1.0, `cart fw show`)
static volatile bool _RecvHeld;           // receive mode: no game traffic, no room (NW_RecvHold)
// 0.17.7: the one-time question before a public party match
static PartyAsk_t _Pa;
static volatile int _PaAnswerReq = -1;    // from the OSD: a PaAnswer_t
static bool _RoomReqRejoin;               // the request is the one rejoin after the answer
static uint32_t _PaSince;
enum { kPaPostWait_ms = 30000 };           // no answer from the API by then: the error line
static void (*_OpenRoomMenu)(void);
static void (*_OpenPage)(const char *Item);   // 1.0.2: a SYSTEM item on the screen (LINK_UI)
static bool _LastOpLink;                  // the request in the window was a LINK op
static void ShlSend(void *ctx, ShlEp_t to, const uint8_t *b, size_t n);
static uint32_t ShlRand(void *ctx);
static void ShlNeedTicket(void *ctx);
static void OnAsk(void *ctx);
static void OnGameRoom(void *ctx, uint8_t mode, const char *arg);
static void OnGameUi(void *ctx, uint8_t what);
static void OnGameStatus(void *ctx, NwLinkDev_t *dev);

static bool ShlAlloc(void)
{
    if (_Sm != NULL) return true;
    ShlMem_t *const m = (ShlMem_t *)calloc(1, sizeof(ShlMem_t));
    if (m == NULL) { _ShlAllocFails++; return false; }
    const ShlHost_t sh = {NULL, ShlSend, ShlRand, ShlNeedTicket};
    Shl_Init(&m->c, &sh);
    m->c.client_id = _ShlClientId;
    _Sm = m;
    _Link.c = &m->c;
    _ShlAllocs++;
    return true;
}

static void ShlFree(void)
{
    if (_Sm == NULL) return;
    if (_Sm->c.client_id) _ShlClientId = _Sm->c.client_id;
    _Link.c = NULL;
    memset(&_Sm->buf, 0, sizeof(_Sm->buf));   // a ticket may have passed through it
    free(_Sm);
    _Sm = NULL;
    if (_ShlSock >= 0) { close(_ShlSock); _ShlSock = -1; }
}

static uint8_t LocalOp(void *u, uint8_t op, const uint8_t *body, uint16_t blen, uint8_t *out, uint16_t cap,
                       uint16_t *out_len)
{
    (void)u;
    _LastOpLink = true;
    *out_len = 0;
    if (_Sm == NULL) {
        // 1.0.3: receive mode's hold freed the room client: XCHG and JOIN answered as an idle client (OK, no
        // room), never NOT_IDENTIFIED / OFFLINE: PARTY BATTLE v3-v11 latch a failed JOIN as "not a Chromatic"
        if (_RecvHeld && (op == kNwLink_OpXchg || op == kNwLink_OpJoin))
            return NwLink_OpNoClient(&_Link, op, body, blen, out, cap, out_len, (uint32_t)(esp_timer_get_time() / 1000));
        // XCHG before a JOIN: NOT_IDENTIFIED (the helper joins again); LEAVE: nothing to leave
        if (op == kNwLink_OpXchg) return NW_S_NOT_IDENTIFIED;
        if (op == kNwLink_OpLeave) return NW_S_OK;
        // 1.0.2: the device's state and its system screens need no room client
        if (op == kNwLink_OpStatus || op == kNwLink_OpUi)
            return NwLink_Op(&_Link, op, body, blen, out, cap, out_len, (uint32_t)(esp_timer_get_time() / 1000));
        if (op == kNwLink_OpRoom) return NW_S_NOT_IDENTIFIED;
        if (op != kNwLink_OpJoin) return NW_S_UNSUPPORTED;
        // (a JOIN while held is answered above; one whose room client does not fit the heap: the same idle answer)
        if (!ShlAlloc()) return NwLink_OpNoClient(&_Link, op, body, blen, out, cap, out_len, (uint32_t)(esp_timer_get_time() / 1000));   // tried again at its next exchange
    }
    return NwLink_Op(&_Link, op, body, blen, out, cap, out_len, (uint32_t)(esp_timer_get_time() / 1000));
}

static void ShlSend(void *ctx, ShlEp_t to, const uint8_t *b, size_t n)
{
    (void)ctx;
    if (_ShlSock < 0) return;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(to.port);
    a.sin_addr.s_addr = htonl(to.ip);
    (void)sendto(_ShlSock, b, n, 0, (struct sockaddr *)&a, sizeof a);
}

static uint32_t ShlRand(void *ctx) { (void)ctx; return esp_random(); }
static void ShlNeedTicket(void *ctx) { (void)ctx; CwlOnline_WantTicket(); }
// The game asked for its room (lk_ask). The app may have said which one ("join
// CODE" from the app's Chromatic settings, or "Invite my Chromatic"): take it at
// once. The menu opens either way (it shows JOINING, or the choices).
// 1.0.1: a party game (family "SHPTY1:") with no app instruction takes the
// quick match (MY DEVICES) at once and the menu does NOT open: PARTY on the game
// means the public match (owner 2026-10-12). The public-match question still
// opens it first for an account that never answered (ShlTick: PartyAsk_OnPick).
static void OnAsk(void *ctx)
{
    (void)ctx;
    // 1.0.2: a game that chooses its room on its own screens (LINK_ROOM) never gets the
    // room menu from an ask
    if (_Link.game_room_mode != 0xFF) return;
    char arg[40];
    uint32_t age = 0;
    const char m = CwlOnline_Instruction(arg, sizeof(arg), &age);
    if (_Sm == NULL || _Sm->c.state == kShl_Idle || _Sm->c.state == kShl_Ended) {
        if (m == 'j' && Sl_ValidCode(arg)) NW_RoomRequest(kShl_ModeJoin, arg);
        else if (m == 'r' && arg[0]) NW_RoomRequest(kShl_ModeRoom, arg);
        else {
            const int quick = PartyAsk_AskMode(_Link.fam);
            if (quick >= 0) {
                NW_RoomRequest(quick, "");
                return;
            }
        }
    }
    _MenuWanted = true;
}

static void ShlEnsureSocket(void)
{
    if (_ShlSock >= 0 || _Sm == NULL || !CWL_IsOnline()) return;
    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) { close(s); return; }
    const int fl = fcntl(s, F_GETFL, 0);
    (void)fcntl(s, F_SETFL, fl | O_NONBLOCK);
    _ShlSock = s;
}

// Once per nw tick: the socket, tickets, the OSD's requests, the stream's timers.
static void ShlTick(uint32_t now)
{
    // receive mode (wifirecv1): the room is left, and no new one starts until it is over
    if (_RecvHeld && _RoomReq >= 0 && _RoomReq != 100) _RoomReq = -1;
    const int req = _RoomReq;
    // A room asked for before any SHLINK game is attached (console `nw room`)
    // waits for the game's LINK_JOIN.
    if (req >= 0 && (req == 100 || _Link.attached)) {
        _RoomReq = -1;
        const bool rejoin = _RoomReqRejoin;
        _RoomReqRejoin = false;
        if (req == 100) {
            if (_Sm != NULL) Shl_Leave(&_Sm->c);
        } else if (!rejoin && PartyAsk_OnPick(&_Pa, _Link.fam, (uint8_t)req, CwlOnline_PartyConsent())) {
            // a public match and the account has not said OK: ask first (no SJOIN)
            _MenuWanted = true;
        } else if (ShlAlloc()) {
            if (rejoin) PartyAsk_OnRejoinStarted(&_Pa);
            const ShlEp_t none = {0, 0};
            Shl_Start(&_Sm->c, none, (uint8_t)req, req == kShl_ModeJoin ? _RoomReqArg : "",
                      req == kShl_ModeRoom ? _RoomReqArg : "", _Link.ident, _Link.fam, _Link.maxp,
                      strtoull(CwlOnline_DeviceId(), NULL, 16), now);
        }
    }
    // a SHLINK game arrived: a ticket in hand before the player picks a room
    // (the HTTPS takes seconds), and the app's instruction with it
    static bool WasAttached;
    if (_Link.attached && !WasAttached) CwlOnline_WantTicket();
    WasAttached = _Link.attached;
    if (_Sm != NULL) {
        ShlClient_t *const c = &_Sm->c;
        ShlEnsureSocket();
        if (_ShlSock >= 0) {
            uint8_t *const b = _Sm->buf.rx;
            for (;;) {
                struct sockaddr_in from;
                socklen_t fl = sizeof from;
                const int n = recvfrom(_ShlSock, b, sizeof _Sm->buf.rx, 0, (struct sockaddr *)&from, &fl);
                if (n <= 0) break;
                const ShlEp_t ep = {ntohl(from.sin_addr.s_addr), ntohs(from.sin_port)};
                (void)Shl_OnDatagram(c, ep, b, (size_t)n, now);
            }
        }
        if (c->state == kShl_NeedTicket) {
            CwlTicket_t *const tp = &_Sm->buf.t;
            CwlOnline_WantTicket();
            if (CwlOnline_TakeTicket(tp, now)) {
                c->relay.ip = tp->relay.ip;
                c->relay.port = tp->relay.port;
                if (c->client_id == 0) c->client_id = tp->client_id;
                Shl_SetTicket(c, tp->ticket, tp->len, now);
                memset(tp, 0, sizeof(*tp));
            }
        }
        Shl_Poll(c, now);
        // the relay refused a public party seat for the account's answer (ENDED 14)
        static ShlState_t WasState;
        if (c->state == kShl_Ended && WasState != kShl_Ended &&
            PartyAsk_OnEnded(&_Pa, c->fam, c->asked_mode, c->ended_reason))
            _MenuWanted = true;
        WasState = c->state;
        // a live room keeps the radio at full power (no modem sleep: its listen
        // interval would add up to 300 ms to every message)
        // 0.17.5: from the SWELCOME on (Seated), not only once the first STATE
        // item says "in room": the seat's first packets (STATE + the join
        // backlog) must be acked at full speed or the relay repeats its window.
        if (c->state == kShl_Seated || c->state == kShl_Joining) CWL_NetworkUse(_E.fpga_unlocked != 0, true);
        // the snapshot the OSD reads
        NwRoomInfo_t ri = {0};
        ri.Attached = _Link.attached;
        ri.State = (int)c->state;
        ri.InRoom = Shl_InRoom(c);
        ri.Host = c->is_host;
        ri.Peers = c->peers;
        ri.Players = (uint16_t)PartyAsk_ShownPlayers(c->fam, c->peers);   // 1.0.1
        ri.MaxPeers = c->max_peers;
        snprintf(ri.Code, sizeof(ri.Code), "%s", c->room_code);
        snprintf(ri.Ended, sizeof(ri.Ended), "%s", c->state == kShl_Ended ? Shl_ReasonName(c->ended_reason) : "");
        _RoomInfo = ri;
        _ShlStats = c->stats;
        // nothing left to serve: give the RAM back
        // (receive mode, wifirecv1: also with a game attached, once the room is left; unit 1 with
        // PARTY BATTLE v8 kept ~4 KB of it and its socket, and its polls were held under the TLS floor)
        if ((!_Link.attached || _RecvHeld) && (c->state == kShl_Idle || c->state == kShl_Ended)) ShlFree();
    }
    if (_Sm == NULL) {
        NwRoomInfo_t ri = {0};
        ri.Attached = _Link.attached;
        _RoomInfo = ri;
    }
    // the player's answer, the POST, and the one rejoin
    const int ans = _PaAnswerReq;
    if (ans >= 0) {
        _PaAnswerReq = -1;
        const PaAnswer_t a = PartyAsk_OnAnswer(&_Pa, (PaAnswer_t)ans);
        if (a != kPa_AnswerNone) {
            _PaSince = now;
            if (!CwlOnline_PostPartyConsent(a == kPa_AnswerGeneric)) (void)PartyAsk_OnPosted(&_Pa, -1);
        }
    }
    if (_Pa.State == kPa_Posting) {
        int http = 0;
        const bool done = CwlOnline_PartyConsentResult(&http);
        if (done || now - _PaSince > kPaPostWait_ms) {
            if (PartyAsk_OnPosted(&_Pa, done ? http : -1)) {
                CwlOnline_DropTicket();     // the spare one carries no answer
                _RoomReqRejoin = true;
                _RoomReq = _Pa.Mode;        // _RoomReqArg still holds the room id (mode 3)
            }
        }
    }
    _RoomInfo.AskConsent = _Pa.State == kPa_Asking;
    _RoomInfo.AskBusy = _Pa.State == kPa_Posting || _Pa.State == kPa_Rejoin;
    _RoomInfo.AskFailed = _Pa.State == kPa_Failed;
    if (_MenuWanted) {
        _MenuWanted = false;
        if (_OpenRoomMenu) _OpenRoomMenu();
    }
    if (_DlWanted) {
        _DlWanted = false;
        if (_OpenPage) _OpenPage("DOWNLOAD");
    }
    if (_FwWanted) {
        _FwWanted = false;
        if (_OpenPage) _OpenPage("UPDATE");
    }
    if (_GgWanted) {
        _GgWanted = false;
        if (_OpenPage) _OpenPage("GET  GAMES");
    }
    if (_PlayWanted) {
        _PlayWanted = false;
        if (_OpenPage) _OpenPage("GAMES");   // lane ui110dev: PLAY is part of GAMES
    }
}

// ---------------------------------------------------------------- FPGA

// FPGA 18.21+ (CWL debug build) has the window.
static bool FpgaHasWindow(void)
{
    const uint16_t v = CWL_FpgaVersionRaw();
    const unsigned major = v & 0x3F, minor = (v >> 6) & 0x3F, debug = (v >> 12) & 1;
    return debug && major == 18 && minor >= 21;
}

// A packet to address 0x11. Waits for the UART to drain first so link-cable
// commands queued meanwhile go out between window packets, not behind 4 KiB.
static void Send(void *user, const uint8_t *p, int n)
{
    (void)user;
    if (n <= 0 || n > 255) return;
    // While the link cable is busy, cut big RXDATA packets into small ones.
    if (p[0] == NW_R_RXDATA && n > 4 + kLinkChunk && CWL_LinkBusy()) {
        const uint16_t off = (uint16_t)(p[2] | (p[3] << 8));
        uint8_t q[4 + kLinkChunk];
        for (int k = 0; k < n - 4; k += kLinkChunk) {
            const int m = (n - 4 - k) > kLinkChunk ? kLinkChunk : (n - 4 - k);
            const uint16_t o = (uint16_t)(off + k);
            q[0] = p[0]; q[1] = p[1]; q[2] = (uint8_t)o; q[3] = (uint8_t)(o >> 8);
            memcpy(q + 4, p + 4 + k, (size_t)m);
            Send(NULL, q, 4 + m);
        }
        return;
    }
    static uint8_t Buf[3 + 255 + 1];
    Buf[0] = 0x8F;
    Buf[1] = NW_UART_ADDR_TO_FPGA;
    Buf[2] = (uint8_t)n;
    memcpy(Buf + 3, p, (size_t)n);
    const size_t Size = crc8_sae_j1850_encode(Buf, 3 + (size_t)n, Buf);
    (void)uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(kUartWait_ms));
    (void)uart_write_bytes(UART_NUM_1, Buf, Size);
    _PktOut++;
}

void NW_OnFpgaPacket(const uint8_t *Payload, uint8_t Len)
{
    _PktIn++;
    _SawWindow = true;   // (the version word only arrives with the OSD or on request)
    if (_Q == NULL) return;
    Msg_t m;
    m.Kind = kMsg_Packet;
    m.Len = Len;
    memcpy(m.Data, Payload, Len);
    if (xQueueSend(_Q, &m, 0) != pdTRUE) _PktDrop++;
    // A game opened the window while the MCU may sleep: keep it awake now.
    if (Len >= 3 && Payload[0] == NW_T_STATE && (Payload[2] & NW_SF_UNLOCKED)) CWL_NetworkUse(true, true);
}

// cartspeed1: the express cart stream's status (address 0x0C) goes to the cart job on THIS task
// (the one that runs cf_esp_op / cf_esp_tick), so the job is never touched from two tasks.
void NW_OnCartXsPacket(const uint8_t *Payload, uint8_t Len)
{
    if (_Q == NULL || Len > sizeof(((Msg_t *)0)->Data)) return;
    Msg_t m;
    m.Kind = kMsg_CartXs;
    m.Len = Len;
    memcpy(m.Data, Payload, Len);
    if (xQueueSend(_Q, &m, 0) != pdTRUE) _PktDrop++;
}

// ---------------------------------------------------------------- backend

static void HexId(const uint8_t *id, char *out)
{
    static const char *hx = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { out[2 * i] = hx[id[i] >> 4]; out[2 * i + 1] = hx[id[i] & 15]; }
    out[32] = '\0';
}

static uint8_t MapHttp(int code)
{
    if (code == 200 || code == 206) return NW_S_OK;
    if (code == 400) return NW_S_BAD_REQUEST;
    if (code == 401 || code == 403) return NW_S_DENIED;
    if (code == 404 || code == 416) return NW_S_NOT_FOUND;
    if (code == 413) return NW_S_TOO_LARGE;
    if (code == 429) return NW_S_RATE_LIMITED;
    if (code == 501) return NW_S_UNSUPPORTED;
    return NW_S_SERVER_ERROR;
}

// ---- the gateway client: ONE persistent esp_http_client with keep-alive (a
// full TLS handshake to api.shinyrelay.com is ~4-5 s on this ESP32, so a
// request per handshake made every window op take seconds). The connection
// is closed after kIdleClose_ms without a call, or at once when the heap is
// low, so an idle console does not hold TLS buffers in the ~20 KB heap.
// Response headers only arrive through the event handler (in ESP-IDF
// esp_http_client_get_header reads REQUEST headers), so the three we need are
// captured there: X-Total-Length (dev_proxy re-frames bodies as chunked, so
// Content-Length is gone), Content-Range, and X-Gbnet-Notify (NOTIFY).
// The gateway: NVS "nw/url" when set (a LAN mock), else the paired API host's
// /gbnet/v1 (https://api.shinyrelay.com/gbnet/v1 by default).
static bool _GatewayOk;   // the last GatewayBase() may carry the token (https://, or a debug build's http://)
static const char *GatewayBase(void)
{
    static char b[160];
    _GatewayOk = CwlSec_GatewayBase(_Url, CwlOnline_Api(), b, sizeof(b), CwlSec_HttpAllowed());
    return b;
}

// Why the last Http() gave what it gave, for `nw http` / `nw status` (0.17.2:
// a status 5 OFFLINE said nothing about which of four things it was).
static char _LastWhy[72] = "";

enum { kIdleClose_ms = 20000, kLowHeap = 14000, kAuthMax = 800, kTlsMinHeapNw = 21000 };
static esp_http_client_handle_t _Cli;
static char _CliBase[128];
static uint32_t _CliLast;
static uint32_t _HdrTotal;
static bool _HdrHaveTotal;
static int8_t _HdrNotify = -1;   // -1: the answer said nothing


static esp_err_t OnHttpEvent(esp_http_client_event_t *ev)
{
    if (ev->event_id != HTTP_EVENT_ON_HEADER || ev->header_key == NULL || ev->header_value == NULL) return ESP_OK;
    if (strcasecmp(ev->header_key, "Date") == 0) {
        (void)CwlTime_FromHttpDate(ev->header_value);   // 1.1.7: the real time from our server when nothing set the clock yet
    } else if (strcasecmp(ev->header_key, "X-Total-Length") == 0) {
        _HdrTotal = (uint32_t)strtoul(ev->header_value, NULL, 10);
        _HdrHaveTotal = true;
    } else if (strcasecmp(ev->header_key, "Content-Range") == 0 && !_HdrHaveTotal) {
        const char *slash = strchr(ev->header_value, '/');
        if (slash && slash[1] != '*') _HdrTotal = (uint32_t)strtoul(slash + 1, NULL, 10);
    } else if (strcasecmp(ev->header_key, "X-Gbnet-Notify") == 0) {
        _HdrNotify = ev->header_value[0] == '1' ? 1 : 0;
    }
    return ESP_OK;
}

static void CloseClient(void)
{
    if (_Cli) {
        esp_http_client_cleanup(_Cli);
        _Cli = NULL;
    }
}

// One HTTP exchange. rel: path below the base URL. Returns the NW status; the
// body (at most kBodyMax bytes) lands in _Out, *len/*total set.
static uint8_t HttpUnlocked(const char *method, const char *rel, uint16_t gamever, const uint8_t *body, uint16_t blen,
                            uint32_t range_off, uint16_t range_max, uint16_t *len, uint32_t *total);
// One gateway client (_Cli, _Out): the game's window jobs and the cart-install download
// (lane cartflash1, another task) take turns.
static SemaphoreHandle_t _HttpMx;
static uint8_t Http(const char *method, const char *rel, uint16_t gamever, const uint8_t *body, uint16_t blen,
                    uint32_t range_off, uint16_t range_max, uint16_t *len, uint32_t *total)
{
    if (_HttpMx == NULL) _HttpMx = xSemaphoreCreateMutex();
    if (_HttpMx == NULL || xSemaphoreTake(_HttpMx, pdMS_TO_TICKS(30000)) != pdTRUE) { *len = 0; *total = 0; return NW_S_OFFLINE; }
    const uint8_t st = HttpUnlocked(method, rel, gamever, body, blen, range_off, range_max, len, total);
    xSemaphoreGive(_HttpMx);
    return st;
}

// A cart job needs the heap: wait for a request in flight (up to wait_ms), then free the
// client (its TLS buffers). The next request makes a new one (TlsHold refuses it meanwhile).
bool NW_DropClient(uint32_t wait_ms)
{
    if (_HttpMx == NULL) return true;
    if (xSemaphoreTake(_HttpMx, pdMS_TO_TICKS(wait_ms)) != pdTRUE) return false;
    CloseClient();
    xSemaphoreGive(_HttpMx);
    return true;
}

static uint8_t HttpUnlocked(const char *method, const char *rel, uint16_t gamever, const uint8_t *body, uint16_t blen,
                            uint32_t range_off, uint16_t range_max, uint16_t *len, uint32_t *total)
{
    *len = 0;
    *total = 0;
    const char *base = GatewayBase();
    _LastWhy[0] = '\0';
    if (base[0] == '\0') { snprintf(_LastWhy, sizeof(_LastWhy), "no gateway URL"); return NW_S_OFFLINE; }
    if (!_GatewayOk) { snprintf(_LastWhy, sizeof(_LastWhy), "gateway URL not https"); return NW_S_OFFLINE; }   // the Authorization header is the device token
    if (CwlOnline_TlsHold()) { snprintf(_LastWhy, sizeof(_LastWhy), "held: cart-flash job"); return NW_S_OFFLINE; }
    if (_RecvHeld) { snprintf(_LastWhy, sizeof(_LastWhy), "held: receiving a game"); return NW_S_OFFLINE; }
    static char url[400];
    snprintf(url, sizeof(url), "%s/%s", base, rel);
    if (_Cli && strcmp(_CliBase, base) != 0) CloseClient();
    // A new connection is a new TLS handshake: the same floor as cwl_online.c's Post()
    // (0.17.6, ~21 KB; below it mbedtls dereferenced a failed allocation: unit 1, 2026-10-11).
    if (_Cli == NULL && strncmp(base, "https:", 6) == 0 && esp_get_free_heap_size() < kTlsMinHeapNw) {
        snprintf(_LastWhy, sizeof(_LastWhy), "low heap for TLS: %lu", (unsigned long)esp_get_free_heap_size());
        return NW_S_OFFLINE;
    }
    if (_Cli == NULL) {
        esp_http_client_config_t cfg = {
            .url = url,
            .timeout_ms = kHttpTimeout_ms,
            .buffer_size = 1024,
            .buffer_size_tx = 1536,   // the Authorization header carries a ~400-byte JWT
            .keep_alive_enable = true,
            .event_handler = OnHttpEvent,
        };
        if (strncmp(base, "https:", 6) == 0) cfg.crt_bundle_attach = CwlTls_Attach;
        _Cli = esp_http_client_init(&cfg);
        if (_Cli == NULL) return NW_S_SERVER_ERROR;
        snprintf(_CliBase, sizeof(_CliBase), "%s", base);
    } else {
        (void)esp_http_client_set_url(_Cli, url);
    }
    esp_http_client_handle_t c = _Cli;
    (void)esp_http_client_set_method(c, strcmp(method, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    // the ONE device token: the one Shiny Link pairing stored (cwl_online.c)
    (void)esp_http_client_delete_header(c, "Authorization");
    // (on the heap for the moment it takes: the static RAM is the heap's)
    char *auth = malloc(kAuthMax + 8);
    if (auth != NULL) {
        memcpy(auth, "Device ", 7);
        if (CwlOnline_CopyToken(auth + 7, kAuthMax) > 0) {
            (void)esp_http_client_set_header(c, "Authorization", auth);   // copied by the client
        }
        memset(auth, 0, kAuthMax + 8);
        free(auth);
    }
    char hv[64];
    snprintf(hv, sizeof(hv), "%u", (unsigned)gamever);
    (void)esp_http_client_set_header(c, "X-Game-Version", hv);
    (void)esp_http_client_set_header(c, "User-Agent", "ShinyChromatic-nw/1");
    (void)esp_http_client_delete_header(c, "Range");
    if (range_max) {
        snprintf(hv, sizeof(hv), "bytes=%lu-%lu", (unsigned long)range_off, (unsigned long)(range_off + range_max - 1));
        (void)esp_http_client_set_header(c, "Range", hv);
    }
    (void)esp_http_client_delete_header(c, "Content-Type");
    if (blen) (void)esp_http_client_set_header(c, "Content-Type", "application/octet-stream");

    const uint32_t t0 = NowMs();
    _HttpCalls++;
    _HdrTotal = 0;
    _HdrHaveTotal = false;
    _HdrNotify = -1;
    const int wdt = CwlWdt_LongBegin();   // a new connection's TLS handshake (~6 s of ECC math, no yield): cwl_wdt.h
    esp_err_t err = esp_http_client_open(c, blen);
    CwlWdt_LongEnd(wdt);   // the core Begin took (the console task is not pinned)
    if (err != ESP_OK) {
        _HttpErr++;
        _LastHttpStatus = -1;
        snprintf(_LastWhy, sizeof(_LastWhy), "open: %s, wifi %s, heap %lu", esp_err_to_name(err),
                 CWL_IsOnline() ? "up" : "DOWN", (unsigned long)esp_get_free_heap_size());
        CloseClient();
        return NW_S_OFFLINE;
    }
    if (blen && esp_http_client_write(c, (const char *)body, blen) != blen) {
        _HttpErr++;
        _LastHttpStatus = -2;
        CloseClient();
        return NW_S_SERVER_ERROR;
    }
    const int64_t clen = esp_http_client_fetch_headers(c);
    if (clen < 0) {
        _HttpErr++;
        _LastHttpStatus = -3;
        snprintf(_LastWhy, sizeof(_LastWhy), "no response headers");
        CloseClient();
        return NW_S_OFFLINE;
    }
    const int code = esp_http_client_get_status_code(c);
    _LastHttpStatus = code;
    const uint8_t st = MapHttp(code);
    uint16_t n = 0;
    for (;;) {
        if (n >= kBodyMax) break;
        const int r = esp_http_client_read(c, (char *)_Out + n, kBodyMax - n);
        if (r <= 0) break;
        n = (uint16_t)(n + r);
    }
    // the rest of a body bigger than one answer: drained, so the connection can be reused
    int drained = 0;
    (void)esp_http_client_flush_response(c, &drained);
    uint32_t tot = _HdrTotal ? _HdrTotal : (clen > 0 ? (uint32_t)clen : 0);
    if (tot < n) tot = n;
    *len = n;
    *total = tot;
    if (!esp_http_client_is_complete_data_received(c) || esp_get_free_heap_size() < kLowHeap) CloseClient();
    _CliLast = NowMs();
    _LastHttpMs = NowMs() - t0;
    return st;
}

static uint8_t NewHandle(const uint8_t *gid, const char *rel, uint32_t total)
{
    const uint8_t i = _NextHandle;
    _NextHandle = (uint8_t)((_NextHandle + 1) % kHandles);
    _Handles[i].Used = true;
    memcpy(_Handles[i].GameId, gid, 16);
    snprintf(_Handles[i].Path, sizeof(_Handles[i].Path), "%s", rel);
    _Handles[i].Total = total;
    return (uint8_t)(i + 1);
}

static void RunJob(const Job_t *j, Result_t *r)
{
    char gid[33], rel[300];
    HexId(j->GameId, gid);
    uint16_t len = 0;
    uint32_t total = 0;
    r->Handle = 0;
    switch (j->Op) {
    case NW_OP_GET:
    case NW_OP_POST:
        snprintf(rel, sizeof(rel), "games/%s/content/%s", gid, j->Path);
        r->Status = Http(j->Op == NW_OP_GET ? "GET" : "POST", rel, j->GameVer, j->Body, j->BodyLen, 0, 0, &len, &total);
        if (r->Status == NW_S_OK && total > len && j->Op == NW_OP_GET) r->Handle = NewHandle(j->GameId, rel, total);
        break;
    case NW_OP_SERVICE:
        snprintf(rel, sizeof(rel), "games/%s/service/%u", gid, j->Body[0]);
        r->Status = Http("POST", rel, j->GameVer, j->Body + 1, (uint16_t)(j->BodyLen - 1), 0, 0, &len, &total);
        break;
    case NW_OP_UPDATE_CHECK:
        snprintf(rel, sizeof(rel), "games/%s/update?version=%u", gid, (unsigned)j->GameVer);
        r->Status = Http("GET", rel, j->GameVer, NULL, 0, 0, 0, &len, &total);
        // [available u8, new_version u16, size u32] + handle for BLOB_READ of the new image
        if (r->Status == NW_S_OK && len >= 7) {
            if (_Out[0]) {
                char img[300];
                snprintf(img, sizeof(img), "games/%s/update/rom", gid);
                const uint32_t sz = (uint32_t)_Out[3] | (uint32_t)_Out[4] << 8 | (uint32_t)_Out[5] << 16 | (uint32_t)_Out[6] << 24;
                _Out[7] = NewHandle(j->GameId, img, sz);
            } else {
                _Out[7] = 0;
            }
            len = 8;
            total = 8;
        }
        break;
    case NW_OP_POLL:
        snprintf(rel, sizeof(rel), "games/%s/poll", gid);
        r->Status = Http("GET", rel, j->GameVer, NULL, 0, 0, 0, &len, &total);
        break;
    case NW_OP_LB_SUBMIT:
        snprintf(rel, sizeof(rel), "games/%s/lb/%u", gid, j->Body[0]);
        r->Status = Http("POST", rel, j->GameVer, j->Body + 1, 12, 0, 0, &len, &total);
        break;
    case NW_OP_LB_TOP:
        snprintf(rel, sizeof(rel), "games/%s/lb/%u?start=%u&count=%u", gid, j->Body[0],
                 (unsigned)(j->Body[1] | (j->Body[2] << 8)), j->Body[3]);
        r->Status = Http("GET", rel, j->GameVer, NULL, 0, 0, 0, &len, &total);
        break;
    case NW_OP_BLOB_READ: {
        const uint8_t h = j->Body[0];
        const uint32_t off = (uint32_t)j->Body[1] | (uint32_t)j->Body[2] << 8 | (uint32_t)j->Body[3] << 16 | (uint32_t)j->Body[4] << 24;
        uint16_t max = (uint16_t)(j->Body[5] | (j->Body[6] << 8));
        if (max == 0 || max > kBodyMax) max = kBodyMax;
        if (h == 0 || h > kHandles || !_Handles[h - 1].Used || memcmp(_Handles[h - 1].GameId, j->GameId, 16) != 0) {
            r->Status = NW_S_NOT_FOUND;
            break;
        }
        if (off >= _Handles[h - 1].Total) { r->Status = NW_S_OK; r->Handle = h; total = _Handles[h - 1].Total; break; }
        r->Status = Http("GET", _Handles[h - 1].Path, j->GameVer, NULL, 0, off, max, &len, &total);
        if (len > max) len = max;
        r->Handle = h;
        break;
    }
    default:
        r->Status = NW_S_UNSUPPORTED;
        break;
    }
    r->Len = len;
    r->Total = total;
}

static void HttpTask(void *arg)
{
    (void)arg;
    uint8_t id;
    for (;;) {
        // the cart install's own connection is worked here too (cf_dev.c): no second task, no second stack
#ifdef NW_CARTFLASH
        const int cf = CfDev_HttpWork();
#else
        const int cf = 0;
#endif
        if (xQueueReceive(_JobQ, &id, pdMS_TO_TICKS(cf ? 10 : 1000)) != pdTRUE) {
            // idle: free the TLS buffers. Under the mutex: another task (the console's `nw http`, the
            // cart fetch) may be INSIDE a request on this client, and _CliLast is only set when a
            // request ends, so a connect that began after a long wait looked idle and was freed
            // under it (unit 1, 2026-10-11: InstrFetchProhibited in esp_http_client_connect).
            if (_Cli && NowMs() - _CliLast > kIdleClose_ms && _HttpMx != NULL && xSemaphoreTake(_HttpMx, 0) == pdTRUE) {
                if (_Cli && NowMs() - _CliLast > kIdleClose_ms) CloseClient();
                xSemaphoreGive(_HttpMx);
            }
            continue;
        }
        if (id != _Job.Id || _CancelId == id) continue;   // superseded / cancelled before it started
        Result_t r = {.Id = id};
        RunJob(&_Job, &r);
        r.Notify = _HdrNotify;
        _Res = r;
        Msg_t m = {.Kind = kMsg_Complete, .Len = 1};
        m.Data[0] = id;
        (void)xQueueSend(_Q, &m, portMAX_DELAY);
    }
}

static void BeCall(void *user, nw_esp_t *e, uint8_t id, const nw_call_t *c)
{
    (void)user;
    (void)e;
    _CancelId = 0xFF;
    _Job.Id = id;
    _Job.Op = c->op;
    memcpy(_Job.GameId, c->game_id, 16);
    _Job.GameVer = c->game_ver;
    snprintf(_Job.Path, sizeof(_Job.Path), "%s", c->path ? c->path : "");
    _Job.BodyLen = c->body_len > sizeof(_Job.Body) ? sizeof(_Job.Body) : c->body_len;
    if (_Job.BodyLen) memcpy(_Job.Body, c->body, _Job.BodyLen);
    if (_E.title[0]) snprintf(_LastTitle, sizeof(_LastTitle), "%s", _E.title);
    (void)xQueueSend(_JobQ, &id, 0);
}

static void BeCancel(void *user, uint8_t id)
{
    (void)user;
    _CancelId = id;
}

// ---------------------------------------------------------------- task

static void Note(const uint8_t *src, uint16_t n, uint8_t *dst, uint8_t *dlen)
{
    *dlen = (uint8_t)(n > 48 ? 48 : n);
    memcpy(dst, src, *dlen);
}

static void NwTask(void *arg)
{
    (void)arg;
    const esp_app_desc_t *app = esp_app_get_description();
    unsigned maj = 0, min = 0, pat = 0;
    (void)sscanf(app->version, "%u.%u.%u", &maj, &min, &pat);
    const nw_esp_config_t cfg = {
        .fw_major = (uint8_t)maj, .fw_minor = (uint8_t)(min * 16 + (pat & 15)),
        .rate_per_s = 4, .rate_burst = 8, .call_timeout_ms = 10000,
    };
    const nw_backend_t be = {NULL, BeCall, BeCancel};
    nw_esp_init(&_E, &cfg, &be, Send, NULL);
    _E.local_op = LocalOp;
#ifdef NW_CARTFLASH
    _E.cf_op = cf_esp_op;            // lane cartflash1 (DRAFT): CF_LOAD / CF_NEXT
    _E.cf_user = CfDev_Engine();
    _E.cf_probe_owns = cf_probe_owns;  // `cart probe` over USB: no Wi-Fi, no http
    _E.cf_probe_call = cf_probe_call;
#endif
    NwLink_Init(&_Link, NULL);   // the room client comes with the first LINK_JOIN (ShlAlloc)
    _Link.on_ask = OnAsk;
    _Link.on_room = OnGameRoom;
    _Link.on_ui = OnGameUi;
    _Link.on_status = OnGameStatus;
    _E.enabled = _Enabled;
    _E.awake = 0;
    bool WasWindow = false, WasUnlocked = false;
    int WasOnline = -1;
    uint8_t LastId = 0;
    for (;;) {
        Msg_t m;
        const bool Got = xQueueReceive(_Q, &m, pdMS_TO_TICKS(kTick_ms)) == pdTRUE;
        const uint32_t now = NowMs();
        _E.now = now;
        _HaveWindow = FpgaHasWindow() || _SawWindow;
        if (Got && m.Kind == kMsg_Packet) {
            // A SHLINK game exchanges every frame: that is not "use" by itself
            // (only a live room keeps full power, ShlTick), or a game sitting on
            // its title would hold the radio at full power for ever.
            const bool Activity = (m.Data[0] != NW_T_STATE || (m.Len >= 3 && (m.Data[2] & NW_SF_ACTIVE))) && !_LastOpLink;
            const uint32_t local0 = _E.n_local, req0 = _E.n_requests;
            nw_esp_on_packet(&_E, m.Data, m.Len);
            if (_E.n_requests != req0) _LastOpLink = false;
            else if (_E.n_local != local0) _LastOpLink = true;
            if (Activity) CWL_NetworkUse(_E.fpga_unlocked != 0, true);
            if (_E.state == NW_ESP_CALLING && _E.id != LastId) {
                LastId = _E.id;
                _LastReqFull = _E.req_len;
                Note(_E.req, _E.req_len, _LastReq, &_LastReqLen);
            }
            if (_E.state == NW_ESP_RESPONDING || (_E.state == NW_ESP_IDLE && _E.resp_len && _E.id != LastId)) {
                // answered by the service itself (INFO, IDENTIFY, errors)
                if (_E.id != LastId) {
                    LastId = _E.id;
                    _LastReqFull = _E.req_len;
                    Note(_E.req, _E.req_len, _LastReq, &_LastReqLen);
                    Note(_E.resp, _E.resp_len, _LastResp, &_LastRespLen);
                }
            }
        } else if (Got && m.Kind == kMsg_CartXs) {
#ifdef NW_CARTFLASH
            _E.now = now;
            CfDev_XsStatus(m.Data, m.Len);
#endif
        } else if (Got && m.Kind == kMsg_Complete) {
            if (m.Data[0] == _Res.Id) {
                // NOTIFY follows the gateway's X-Gbnet-Notify on every answer (before the
                // answer itself, so a POLL that emptied the queue clears it in the same CONFIG)
                if (_Res.Notify >= 0) nw_esp_set_notify(&_E, _Res.Notify);
                nw_esp_complete(&_E, _Res.Id, _Res.Status, _Res.Handle, _Res.Total, _Out, _Res.Len);
                Note(_E.resp, _E.resp_len, _LastResp, &_LastRespLen);
                CWL_NetworkUse(_E.fpga_unlocked != 0, true);
            }
        }
        if (!_HaveWindow) {
            WasWindow = false;
            continue;   // older FPGA: nothing listens on address 0x11
        }
        if (!WasWindow) {
            WasWindow = true;
            nw_esp_set_enabled(&_E, _Enabled);   // first CONFIG
            const uint8_t st = NW_R_STATUS;
            Send(NULL, &st, 1);                  // learn the window state (game may be running)
        }
        if (_E.enabled != _Enabled) {
            nw_esp_set_enabled(&_E, _Enabled);
            if (!_Enabled && _E.state != NW_ESP_IDLE) { BeCancel(NULL, _E.id); }
        }
        const int Online = (_Enabled && CWL_IsOnline()) ? 1 : 0;
        if (Online != WasOnline) {
            WasOnline = Online;
            nw_esp_set_online(&_E, Online);
        }
        const bool Unlocked = _E.fpga_unlocked != 0;
        // AWAKE only while a game has the window open: the MCU then stays
        // awake (CWL_KeepAwake), so the FPGA may skip the wake preamble.
        // Cleared before the power manager may sleep again.
        nw_esp_set_awake(&_E, (Unlocked && _Enabled) ? 1 : 0);
        if (Unlocked != WasUnlocked) {
            WasUnlocked = Unlocked;
            if (!Unlocked) NwLink_Detach(&_Link);   // the game is gone (its room stays for a reboot)
            CWL_NetworkUse(Unlocked, Unlocked);
            ESP_LOGI(TAG, "window %s", Unlocked ? "unlocked" : "locked");
        }
        ShlTick(now);
        nw_esp_tick(&_E, now);
#ifdef NW_CARTFLASH
        cf_esp_tick(CfDev_Engine(), now);
        CfDev_Tick();
#endif
    }
}

// ---------------------------------------------------------------- API

bool NW_LinkAttached(void) { return _Link.attached; }

// ---------------------------------------------------------------- 1.0.2: the game's own screens

// The game chose its room (LINK_ROOM): as the room menu's choice, with no menu (the
// public-match question can still come first, as for MY DEVICES).
static void OnGameRoom(void *ctx, uint8_t mode, const char *arg)
{
    (void)ctx;
    NW_RoomRequest(mode == kNwLink_RoomLeave ? 100 : mode, arg);
}

static void OnGameUi(void *ctx, uint8_t what)
{
    (void)ctx;
    switch (what) {
    case kNwLink_UiWifi:
        if (_OpenPage) _OpenPage("WI-FI");
        break;
    case kNwLink_UiPairStart:
        if (!CwlOnline_Paired()) CwlOnline_PairStart();
        break;
    case kNwLink_UiPairCancel:
        CwlOnline_PairCancel();
        break;
    case kNwLink_UiRoomMenu:
        _MenuWanted = true;
        break;
    default:
        break;
    }
}

static void OnGameStatus(void *ctx, NwLinkDev_t *dev)
{
    (void)ctx;
    dev->flags = (uint8_t)((CWL_IsOnline() ? kNwLink_SOnline : 0) | (CwlNet_SavedCount() > 0 ? kNwLink_SWifiSaved : 0) |
                           (CwlOnline_Paired() ? kNwLink_SPaired : 0));
    const CwlPairState_t p = CwlOnline_PairState();
    dev->pair_state = p == kCwlPair_Starting ? 1 : p == kCwlPair_Waiting ? 2 : p == kCwlPair_Done ? 3 : p == kCwlPair_Failed ? 4 : 0;
    if (p == kCwlPair_Waiting) CwlOnline_PairCode(dev->pair_code, sizeof(dev->pair_code), NULL, 0);
}

void NW_SetPageOpener(void (*Fn)(const char *Item)) { _OpenPage = Fn; }
void NW_OpenPage(const char *Item) { if (_OpenPage) _OpenPage(Item); }   // lane sdcard1 (the console's `gpage show`)

void NW_RoomRequest(int Mode, const char *Arg)
{
    snprintf(_RoomReqArg, sizeof(_RoomReqArg), "%s", Arg ? Arg : "");
    _RoomReq = Mode;
}

void NW_SetRoomMenuOpener(void (*Fn)(void)) { _OpenRoomMenu = Fn; }

void NW_PartyAnswer(int Answer) { _PaAnswerReq = Answer; }

void NW_GetRoomInfo(NwRoomInfo_t *Out)
{
    *Out = _RoomInfo;   // the nw task's snapshot (the client itself may be freed any tick)
}

bool NW_IsEnabled(void) { return _Enabled != 0; }

void NW_SetEnabled(bool Enabled)
{
    if ((_Enabled != 0) == Enabled) return;
    _Enabled = Enabled ? 1 : 0;
    Save();
    CWL_SetNetworkWanted(Enabled);
}

const char *NW_StatusText(void)
{
    if (!_Enabled) return "";
    if (!_HaveWindow) return "NEEDS  FPGA  18.21";
    if (_E.fpga_unlocked) return _E.identified ? "GAME  ONLINE" : "GAME  CONNECTING";
    return CWL_IsOnline() ? "READY" : "STANDBY";
}

void NW_GetDetailText(char *Buf, unsigned Len)
{
    if (Buf == NULL || Len == 0) return;
    Buf[0] = '\0';
    if (!_Enabled || !_HaveWindow) return;
    if (_E.fpga_unlocked && _E.title[0]) snprintf(Buf, Len, "%s", _E.title);
    else snprintf(Buf, Len, "%lu  REQUESTS", (unsigned long)_E.n_requests);
}

static void PrintHex(const char *label, const uint8_t *p, unsigned n, unsigned full)
{
    printf("  \"%s\": \"", label);
    for (unsigned i = 0; i < n; i++) printf("%02x", p[i]);
    printf("\", \"%s_len\": %u", label, full);
}

static void PrintStatus(void)
{
    printf("{ \"enabled\": %d, \"window_fpga\": %d, \"fwver\": \"0x%04x\", \"url\": \"%s\", \"token_set\": %d, "
           "\"online\": %d, \"unlocked\": %d, \"identified\": %d, \"title\": \"%s\", \"state\": %d, "
           "\"requests\": %lu, \"txread\": %lu, \"rxnak\": %lu, \"resync\": %lu, \"denied\": %lu, \"rate_limited\": %lu, "
           "\"pkt_in\": %lu, \"pkt_out\": %lu, \"pkt_drop\": %lu, \"http_calls\": %lu, \"http_err\": %lu, "
           "\"last_http\": %d, \"last_http_ms\": %lu, \"last_why\": \"%s\", \"last_title\": \"%s\",\n",
           _Enabled, _HaveWindow, CWL_FpgaVersionRaw(), GatewayBase(), CwlOnline_Paired(), _E.online, _E.fpga_unlocked,
           _E.identified, _E.title, _E.state, (unsigned long)_E.n_requests, (unsigned long)_E.n_txread,
           (unsigned long)_E.n_rxnak, (unsigned long)_E.n_resync, (unsigned long)_E.n_denied,
           (unsigned long)_E.n_rate_limited, (unsigned long)_PktIn, (unsigned long)_PktOut, (unsigned long)_PktDrop,
           (unsigned long)_HttpCalls, (unsigned long)_HttpErr, _LastHttpStatus, (unsigned long)_LastHttpMs, _LastWhy, _LastTitle);
    PrintHex("last_req", _LastReq, _LastReqLen, _LastReqFull);
    printf(",\n");
    PrintHex("last_resp", _LastResp, _LastRespLen, _E.resp_len);
    const NwRoomInfo_t ri = _RoomInfo;
    const ShlStats_t st = _ShlStats;
    printf(",\n\"shl\": { \"allocated\": %d, \"allocs\": %lu, \"alloc_fails\": %lu, \"attached\": %d, \"ident\": \"%s\", "
           "\"fam\": \"%s\", \"state\": %d, \"in_room\": %d, \"host\": %d, \"peers\": %u, \"code\": \"%s\", "
           "\"ended\": \"%s\", \"joins\": %lu, \"xchgs\": %lu, \"up\": %lu, \"down\": %lu, \"dropped_offline\": %lu, "
           "\"stale\": %lu, \"lost\": %u, \"pkts_out\": %lu, \"pkts_in\": %lu, \"resends\": %lu, \"dups_in\": %lu, \"held_back\": %lu, \"items_in\": %lu, "
           "\"refusals\": %lu, \"rejoins\": %lu, \"last_refuse\": %u, \"free_heap\": %lu } }\n",
           _Sm != NULL, (unsigned long)_ShlAllocs, (unsigned long)_ShlAllocFails, _Link.attached, _Link.ident, _Link.fam,
           ri.State, ri.InRoom, ri.Host, ri.Peers, ri.Code, ri.Ended, (unsigned long)_Link.joins,
           (unsigned long)_Link.xchgs, (unsigned long)_Link.msgs_up, (unsigned long)_Link.msgs_down,
           (unsigned long)_Link.dropped_offline, (unsigned long)_Link.stale, _Link.lost, (unsigned long)st.pkts_out,
           (unsigned long)st.pkts_in, (unsigned long)st.resends, (unsigned long)st.dups_in, (unsigned long)st.held_back, (unsigned long)st.items_in, (unsigned long)st.refusals,
           (unsigned long)st.rejoins, st.last_refuse, (unsigned long)esp_get_free_heap_size());
}

// Status for the "CWS?" UDP reply (no USB needed).
static int StatusExtra(char *Buf, int Len)
{
    return snprintf(Buf, (size_t)Len, " nw=%d/%d/%d/%d nw_req=%lu nw_http=%lu/%lu nw_last=%d nw_pkts=%lu/%lu/%lu",
                    _Enabled, _HaveWindow, _E.fpga_unlocked, _E.identified, (unsigned long)_E.n_requests,
                    (unsigned long)_HttpCalls, (unsigned long)_HttpErr, _LastHttpStatus,
                    (unsigned long)_PktIn, (unsigned long)_PktOut, (unsigned long)_PktDrop);
}

static int CmdNw(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "status";
    if (strcmp(sub, "status") == 0) {
        PrintStatus();
    } else if (strcmp(sub, "on") == 0 || strcmp(sub, "off") == 0) {
        NW_SetEnabled(strcmp(sub, "on") == 0);
        printf("nw: %s\n", _Enabled ? "on" : "off");
    } else if (strcmp(sub, "room") == 0 && argc >= 3) {
        // The running SHLINK game's room, as the OSD room menu does it.
        const char *m = argv[2];
        if (strcmp(m, "host") == 0) NW_RoomRequest(kShl_ModeHost, "");
        else if (strcmp(m, "mine") == 0) NW_RoomRequest(kShl_ModeMine, "");
        else if (strcmp(m, "leave") == 0) NW_RoomRequest(100, "");
        else if (strcmp(m, "join") == 0 && argc >= 4) NW_RoomRequest(kShl_ModeJoin, argv[3]);
        else { printf("usage: nw room host|mine|join <CODE>|leave\n"); return 1; }
        printf("nw room: %s%s\n", m, _Link.attached ? "" : " (no SHLINK game attached: taken when one attaches)");
    } else if (strcmp(sub, "url") == 0 && argc >= 3) {
        if (strcmp(argv[2], "none") != 0 && !CwlSec_ApiUrlOk(argv[2])) {
            printf("nw: refused: the gateway gets the device token, so it must be https://host%s\n",
                   CwlSec_HttpAllowed() ? " or http://host" : " (http:// only in a debug build)");
            return 1;
        }
        snprintf(_Url, sizeof(_Url), "%s", strcmp(argv[2], "none") == 0 ? "" : argv[2]);
        size_t n = strlen(_Url);
        while (n > 0 && _Url[n - 1] == '/') _Url[--n] = '\0';
        Save();
        printf("nw: gateway %s%s\n", GatewayBase(), _Url[0] ? "" : " (the paired API host)");
    } else if (strcmp(sub, "token") == 0) {
        printf("nw: the gateway uses the Shiny Link pairing's device token (%s); pair with `cwl relay pair`\n",
               CwlOnline_Paired() ? "paired" : "not paired");
    } else if (strcmp(sub, "http") == 0 && argc >= 3) {
        // Bench: one gateway call from the console, bypassing the FPGA
        // (`nw http <rel path below the base URL> [post-body]`).
        uint16_t len = 0;
        uint32_t total = 0;
        const char *b = argc >= 4 ? argv[3] : NULL;
        // A game's window use wakes the radio (CWL_NetworkUse); this bench
        // call did not, so with the link dormant it failed at once (status 5,
        // nothing said). Now it asks for the network and waits for it.
        const bool was_wanted = _Enabled != 0;
        if (!CWL_IsOnline()) {
            CWL_SetNetworkWanted(true);
            CWL_NetworkUse(false, true);
            printf("nw http: waiting for Wi-Fi...\n");
            for (int i = 0; i < 150 && !CWL_IsOnline(); i++) vTaskDelay(pdMS_TO_TICKS(100));
        }
        const uint8_t st = Http(b ? "POST" : "GET", argv[2], 1, (const uint8_t *)b, b ? (uint16_t)strlen(b) : 0, 0, 0, &len, &total);
        printf("nw http: status %u http %d len %u total %lu ms %lu body: ", st, _LastHttpStatus, len,
               (unsigned long)total, (unsigned long)_LastHttpMs);
        for (unsigned i = 0; i < len && i < 64; i++) putchar(_Out[i] >= 0x20 && _Out[i] < 0x7F ? _Out[i] : '.');
        printf("\n");
        if (_LastWhy[0]) printf("nw http: %s (gateway %s)\n", _LastWhy, GatewayBase());
        if (!was_wanted) CWL_SetNetworkWanted(false);
    } else {
        printf("usage: nw [status | on | off | url <http(s)://host[:port]/gbnet/v1|none> | token <t|none> | http <path> [body]]\n");
        return 1;
    }
    return 0;
}

#ifndef CONFIG_NW_CARTFLASH
void NW_CartView(NwCartView_t *Out) { memset(Out, 0, sizeof *Out); Out->Kind = -1; }
void NW_CartButton(int A) { (void)A; }
void NW_FwView(NwFwView_t *Out) { memset(Out, 0, sizeof *Out); Out->Kind = -1; }
void NW_FwButton(int A) { (void)A; }
void NW_FwCustom(void) { }
void NW_PlayView(NwPlayView_t *Out) { memset(Out, 0, sizeof *Out); Out->Kind = -1; }
void NW_PlayButton(int A) { (void)A; }
void NW_PlaySd(const char *Path) { (void)Path; }
int  NW_LibCount(uint32_t *FreeB, uint32_t *TotalB, uint32_t *Gen) { (void)FreeB; (void)TotalB; if (Gen) *Gen = 0; return -1; }
int  NW_LibGet(int I, NwLibGame_t *Out) { (void)I; (void)Out; return -1; }
void NW_LibHold(bool On) { (void)On; }
int  NW_LibBusy(void) { return 0; }
int  NW_LibResult(void) { return 0; }
int  NW_LibDelete(uint32_t Uid) { (void)Uid; return -1; }
void NW_LibPlay(uint32_t Uid) { (void)Uid; }
int  NW_LibPlayingUid(void) { return 0; }
void NW_PlayAfterStop(int Card, uint32_t Uid, const char *Path) { (void)Card; (void)Uid; (void)Path; }
void NW_CartTitle(char *Out, unsigned Len) { if (Len) Out[0] = 0; }
bool NW_SdPlayingPath(const char *Path) { (void)Path; return false; }
void NW_GgStart(void) { }
int  NW_GgSide(const char *Path, const char *Body, char *Out, int Cap) { (void)Path; (void)Body; (void)Out; (void)Cap; return -1; }
int  NW_GgSideDone(int *Code) { *Code = -1; return -1; }
int  NW_GgReady(void) { return 0; }
int  NW_GgActive(void) { return 0; }
int  NW_Paired(void) { return 0; }
int  NW_SaveCloudReady(void) { return 0; }
int  NW_SaveCloudHas(uint32_t Uid) { (void)Uid; return 0; }
void NW_SaveCloudForget(void) { }
#endif
static volatile int _GgOwnsJob;
void NW_GgOwnJob(int On) { _GgOwnsJob = On ? 1 : 0; }
int  NW_GgOwnsJob(void) { return _GgOwnsJob; }
#ifdef CONFIG_NW_CARTFLASH
// cartflash (cf_dev.c, `cart fetch`): one ranged GET below the gateway base, from the console
// task, as `nw http` does it. 0 = *data/*len valid, 1 = try again, 2 = the server says no.
const char *NW_CfGatewayBase(void) { return GatewayBase(); }
void CfDev_View(NwCartView_t *Out);
void CfDev_Button(int A);
void NW_CartView(NwCartView_t *Out) { CfDev_View(Out); }
void NW_CartButton(int A) { CfDev_Button(A); }
void CfDev_FwView(NwFwView_t *Out);
void CfDev_FwButton(int A);
void NW_FwView(NwFwView_t *Out) { CfDev_FwView(Out); }
void NW_FwButton(int A) { CfDev_FwButton(A); }
void CfDev_FwCustom(void);
void NW_FwCustom(void) { CfDev_FwCustom(); }
void CfRc_View(NwPlayView_t *Out);
void CfRc_Button(int A);
void NW_PlayView(NwPlayView_t *Out) { CfRc_View(Out); }
void NW_PlayButton(int A) { CfRc_Button(A); }
void CfRc_PlaySd(const char *path);
void NW_PlaySd(const char *Path) { CfRc_PlaySd(Path); }
#include "cf_rc_dev.h"
int  NW_LibCount(uint32_t *FreeB, uint32_t *TotalB, uint32_t *Gen) { return CfRc_LibCount(FreeB, TotalB, Gen); }
int  NW_LibGet(int I, NwLibGame_t *Out)
{
    cf_lib_view_ent_t v;
    if (CfRc_LibGet(I, &v)) return -1;
    memset(Out, 0, sizeof *Out);
    Out->Uid = v.uid; Out->RomSize = v.rom_size; Out->StoredSize = v.stored_size; Out->SaveSize = v.save_size; Out->Sgz = (v.flags & 1u) != 0;
    snprintf(Out->Title, sizeof Out->Title, "%s", v.title);
    return 0;
}
void NW_LibHold(bool On) { CfRc_LibHold(On ? 1 : 0); }
int  NW_LibBusy(void) { return CfRc_LibBusy(); }
int  NW_LibResult(void) { return CfRc_LibResult(); }
int  NW_LibDelete(uint32_t Uid) { return CfRc_LibDelete(Uid); }
void NW_LibPlay(uint32_t Uid) { CfRc_LibPlay(Uid); }
int  NW_LibPlayingUid(void) { return CfRc_LibPlayingUid(); }
void NW_PlayAfterStop(int Card, uint32_t Uid, const char *Path) { CfRc_PlayAfterStop(Card, Uid, Path); }
__attribute__((weak)) void CfDev_CartTitle(char *out, unsigned n) { if (n) out[0] = 0; }   // (a build with no Wi-Fi install: not known)
void NW_CartTitle(char *Out, unsigned Len) { CfDev_CartTitle(Out, Len); }
bool NW_SdPlayingPath(const char *Path) { return CfRc_SdPlaying() && Path && !strcmp(CfRc_SdPath(), Path); }
void CfDev_GgStart(void);
int  CfDev_GgSide(const char *path, const char *body, char *out, int cap);
int  CfDev_GgSideDone(int *code);
int  CfDev_GgReady(void);
int  CfDev_GgActive(void);
void NW_GgStart(void) { CfDev_GgStart(); }
int  NW_GgSide(const char *Path, const char *Body, char *Out, int Cap) { return CfDev_GgSide(Path, Body, Out, Cap); }
int  NW_GgSideDone(int *Code) { return CfDev_GgSideDone(Code); }
int  NW_GgReady(void) { return CfDev_GgReady(); }
int  NW_GgActive(void) { return CfDev_GgActive(); }
int  NW_Paired(void) { return CwlOnline_Paired() ? 1 : 0; }
// lane savecloud1: SAVE TO MY GAMES is offered when this unit is paired and has a Wi-Fi network saved
int  NW_SaveCloudReady(void)
{
    char ssid[36];
    CWL_GetSsidText(ssid, sizeof ssid);
    return CwlOnline_Paired() && ssid[0] ? 1 : 0;
}
int  NW_SaveCloudHas(uint32_t Uid) { return CfRc_LibSaveKnown(Uid); }
void NW_SaveCloudForget(void) { CfRc_LibSaveForget(); }
// The streamed cart install takes the memory of the game traffic's idle connection (cf_dev.c net_start).
void NW_CfCloseGateway(void)
{
    if (_HttpMx == NULL) _HttpMx = xSemaphoreCreateMutex();
    if (_HttpMx == NULL || xSemaphoreTake(_HttpMx, pdMS_TO_TICKS(3000)) != pdTRUE) return;   // in a request: it is not idle
    CloseClient();
    xSemaphoreGive(_HttpMx);
}
void NW_CfOpenMenu(void) { _DlWanted = true; }
void NW_FwOpenMenu(void) { _FwWanted = true; }
void NW_PlayOpenMenu(void) { _PlayWanted = true; }
void NW_GgOpenMenu(void) { _GgWanted = true; }
bool NW_RecvHeld(void) { return _RecvHeld; }   // lane gamecomp1: a library unpack holds the link only when nobody else does
void NW_RecvHold(bool On)
{
    _RecvHeld = On;
    if (!On) return;
    _RoomReq = 100;                 // leave the room (the relay seat goes with the link session)
    NW_CfCloseGateway();            // the game traffic's idle connection gives its memory back
}   // SYSTEM > DOWNLOAD (wifirecv1), from the nw task

// The cart install's copy of one ranged answer (the shared _Out is the next job's).
enum { kCfOut = 2048 };
static uint8_t *_CfOut;   // bench only (`cart fetch`): on the heap at its first use, not in every unit's static RAM
int NW_CfRange(const char *rel, uint32_t off, uint16_t max, const uint8_t **data, uint16_t *len, uint32_t *total)
{
    if (!CWL_IsOnline()) {
        CWL_SetNetworkWanted(true);
        CWL_NetworkUse(false, true);
        for (int i = 0; i < 150 && !CWL_IsOnline(); i++) vTaskDelay(pdMS_TO_TICKS(100));
        if (!CWL_IsOnline()) return 1;
    }
    if (max == 0 || max > kBodyMax) max = kBodyMax;
    if (_CfOut == NULL) _CfOut = malloc(kCfOut);
    if (_CfOut == NULL) return 1;
    if (max > kCfOut) max = kCfOut;
    if (_HttpMx == NULL) _HttpMx = xSemaphoreCreateMutex();
    if (_HttpMx == NULL || xSemaphoreTake(_HttpMx, pdMS_TO_TICKS(30000)) != pdTRUE) return 1;
    const uint8_t st = HttpUnlocked("GET", rel, 1, NULL, 0, off, max, len, total);
    if (*len > kCfOut) *len = kCfOut;
    memcpy(_CfOut, _Out, *len);
    xSemaphoreGive(_HttpMx);
    *data = _CfOut;
    if (st == NW_S_OK) return (*len > 0 && *len <= max) ? 0 : 1;   // a server that ignores Range sends more: not usable
    if (st == NW_S_NOT_FOUND || st == NW_S_DENIED || st == NW_S_BAD_REQUEST) return 2;
    return 1;
}
const char *NW_CfLastWhy(void) { return _LastWhy; }
#endif

void NW_Initialize(void)
{
    Load();
    _Q = xQueueCreate(kQueueLen, sizeof(Msg_t));
    _JobQ = xQueueCreate(4, sizeof(uint8_t));
    const esp_console_cmd_t cmd = {
        .command = "nw",
        .help = "Network window service: nw [status|on|off|url <base>|token <t>|http <path> [body]|room host|mine|join <c>|leave]",
        .hint = NULL,
        .func = &CmdNw,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
    CWL_SetStatusExtra(StatusExtra);
#ifdef NW_CARTFLASH
    CfDev_Init();
#endif
}

void NW_Start(void)
{
    if (_Started) return;
    _Started = true;
    CWL_SetNetworkWanted(_Enabled != 0);
    xTaskCreatePinnedToCore(NwTask, "nw", kTaskStack, NULL, kTaskPriority, &_Task, 0);
    // cartspeed2: core 1 (with the OSD timer and the online task): on core 0 the cart write's nw task (priority 5)
    // and this one shared a core, and a Wi-Fi install that writes the cart behind its download starved both
    xTaskCreatePinnedToCore(HttpTask, "nw_http", kHttpStack, NULL, kHttpPriority, &_HttpTask, 1);
}
