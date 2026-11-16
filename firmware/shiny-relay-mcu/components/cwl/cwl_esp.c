// cwl_esp.c: CWL2 Chromatic Wi-Fi Link, ESP32 side. See cwl.h.
#include "cwl.h"
#include "cwl_proto.h"
#include "cwl_online.h"
#include "cwl_relay.h"
#include "cwl_lx.h"
#include "cwl_radio.h"
#include "cwl_net.h"
#include "cwl_priv.h"
#include "cwl_logic.h"
#include "cwl_time.h"
#include "sdkconfig.h"

#include "button.h"
#include "osd.h"
#include "crc8_sae_j1850.h"
#include "driver/uart.h"
#include "esp_console.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    kFpgaCmd_MasterReply = 1,
    kFpgaCmd_SlaveXfer = 2,
    kFpgaCmd_Config = 3,
    kFpgaCmd_MasterDrop = 4,   // release an abandoned master without IRQ (FPGA write barrier)
    kFpgaCmd_CartPowerCycle = 5,   // debug (FPGA 18.18+): cartridge off ~1 s, as the power switch would
    kFpgaCmd_SlaveWithdraw = 6,    // FPGA 18.20+: take back a standing offer (SLAVE_NAK if it still waited)
    kFpgaAddr_LinkCmd = 0x10,
    kFpgaConfig_NetEn = 1 << 0,
    kFpgaConfig_EspMaySleep = 1 << 1,   // FPGA >= 18.15 (CWL debug build): wake preamble
    kFpgaConfig_Barrier = 1 << 2,       // FPGA >= 18.16: we handle ABANDON / MASTER_DROP / event 7,
                                        // so the FPGA may park writes and stall the CPU (write barrier)
    kFpgaConfig_Standing = 1 << 3,      // FPGA >= 18.20: standing offers (exchanged when the game arms)

    kTaskStack = 4096,
    kTaskPriority = 6,
    kEventQueueLen = 32,
    kConfigResend_ms = 1000,

    // Power policy defaults (console: cwl power <active> <standby> <armed>)
    kDefaultActive_s = 20,      // full power this long after the last link use
    kDefaultStandby_s = 60,     // then modem sleep this long, then Wi-Fi off
    kDefaultArmed_s = 300,      // modem sleep while a game waits armed as slave
    kDefaultTimeout_ms = 1500,  // master timeout (a modem-sleeping peer needs > 250 ms)
    kFpgaSafety_ms = 3600,      // stay under the FPGA's ~4 s held-master safety timeout
    kMaxHold_ms = 2000,         // hold a master this long for Wi-Fi to come back
    kListenInterval = 3,        // beacons per wake in modem sleep (~300 ms)
    kJoinFull_ms = 10000,       // full power while joining, at most this long
    kReconnectMin_ms = 250,
    kReconnectMax_ms = 5000,
    // A wake reuses the address of the last DHCP lease for this long after
    // it was granted (DHCP took ~3 s after association; home leases last a
    // day or more), then does a real DHCP again.
    kLeaseReuse_ms = 60 * 60 * 1000,
};

typedef enum {
    kPower_Off,       // link disabled
    kPower_Dormant,   // Wi-Fi off, MCU may light-sleep
    kPower_Low,       // Wi-Fi on, modem sleep
    kPower_Full,      // Wi-Fi on, no power save
    kNumPower,
} Power_t;
static const char *const kPowerName[kNumPower] = {"off", "dormant", "low", "full"};

typedef enum {
    kCtl_FpgaEvent,      // a: event, b: byte, u: ms when received
    kCtl_SetTimeout,     // u: ms
    kCtl_SetPartner,     // a: 1 = Mac, 0 = forget
} CtlKind_t;

typedef struct {
    CtlKind_t Kind;
    uint8_t A, B;
    uint32_t U;
    uint8_t Mac[6];
} CtlMsg_t;

static const char *TAG = "CWL";

// FPGA RX addresses shown in the status reply (main/fpga_rx.h kRxCmd_*).
enum { kStatusAddr_Buttons = 0x2, kStatusAddr_FWVer = 0x6 };
static const char *kNvsNamespace = "cwl";

static volatile bool _Enabled = false;    // the OSD switch / `cwl on|off`
// wifirecv1: SYSTEM > DOWNLOAD > RECEIVE GAME holds the link (CWL_RecvHold): no link session
// (game link, relay seat, ESP-NOW, LAN), the radio up for the HTTPS calls. RAM only.
static volatile bool _RecvHoldF = false;
static volatile uint32_t _SessModeGen = 0;   // the _ModeGen the running session started under
static inline bool LinkEn(void) { return _Enabled && !_RecvHoldF; }
// Network window service (components/nw): Wi-Fi session wanted even with the
// link off; a game holds the window open; last window activity.
static volatile bool _NwWanted = false;
static volatile bool _NwUnlocked = false;
static volatile uint32_t _NwUseMs = 0;
static volatile bool _NwUsed = false;     // _NwUseMs valid
static int (*_StatusExtra)(char *Buf, int Len) = NULL;
static volatile Power_t _Power = kPower_Off;
static volatile bool _WakeReq = false;    // a link event arrived while dormant
static volatile bool _SleepOk = true;     // Wi-Fi fully off: the MCU may light-sleep
static volatile bool _WifiConnected = false;
static bool _WifiInited = false;
static volatile bool _RadioWanted = false;
static bool _RadioStarted = false;
static uint32_t _RadioRetryMs = 0;   // a failed radio / ESP-NOW start is tried again from the session loop
static uint32_t _RadioStartMs = 0;
static volatile uint32_t _ReconnectAt = 0;   // 0: none scheduled
static uint32_t _ReconnectBackoff = kReconnectMin_ms;
static uint8_t _ChannelHint = 0;          // AP channel from the last association
static esp_netif_t *_Netif = NULL;
static esp_netif_ip_info_t _Lease;        // last DHCP result
static esp_netif_dns_info_t _LeaseDns[2]; // its DNS servers (main, backup): a static rejoin
                                          // sets no DNS, so names (api.shinyrelay.com) failed
static bool _LeaseValid = false;
static uint32_t _LeaseMs = 0;             // when it was granted
static volatile bool _StaticRejoin = false;   // this radio start reuses _Lease
static QueueHandle_t _Queue = NULL;
static TaskHandle_t _Task = NULL;
static CwlState_t _Cwl;
static int _Sock = -1;
static struct sockaddr_in _Peer;
static bool _PeerKnown = false;           // learned or configured
static bool _PeerFixed = false;           // configured in NVS
static volatile uint32_t _OwnIp = 0;

// Shiny Link relay (cwl_relay.c): the internet path. Shares _Sock (47820).
static RelayClient_t _Relay;
static bool _RelayStarted = false;        // Relay_Start done for this generation
static uint32_t _RelayGen = 0;            // CwlOnline_Generation() it started with
static bool _RelayUp = false;             // socket + Wi-Fi were up on the last pass
static uint32_t _LanPeerMs = 0;           // last bare CWL packet from a LAN peer (auto mode)
static bool _LanPeerSeen = false;
static uint32_t _RelayDelivered = 0, _RelayDeliveredDirect = 0;
enum {
    kLanPeerFresh_ms = 3000,
    // Seat keeper: while dormant (Wi-Fi off), wake the radio briefly to ping
    // the relay before it forgets our seat (10 min of silence), so a wake
    // never needs a new ticket (an HTTPS handshake takes ~4 s here, longer
    // than the 2 s master hold). Only for an hour after the link was last used.
    kSeatKeepEvery_ms = 8 * 60 * 1000,
    kSeatKeepFor_ms = 60 * 60 * 1000,
    kSeatKeepMax_ms = 8000,      // radio on at most this long per keep
};
static volatile bool _KeepArmed = false;   // a timer wake is wanted while dormant
static volatile uint32_t _KeepDueMs = 0;
static volatile bool _KeepActive = false;  // radio up for a keep ping
static uint32_t _KeepStartMs = 0;
static uint32_t _Keeps = 0, _KeepsAnswered = 0;

// Link use, for the power policy.
static volatile uint32_t _LastUseMs = 0;  // FPGA link event or peer exchange
static bool _PortArmed = false;           // the game waits as slave
static uint32_t _ArmedSinceMs = 0;

// Power statistics (status / CWS?).
typedef struct {
    uint32_t StateMs[kNumPower];          // time spent per power level
    uint32_t StateSinceMs;
    uint32_t Wakes;                       // dormant -> on by a link event
    uint32_t WakeStartMs;                 // radio start of the current wake
    int32_t LastAssocMs, LastIpMs, LastPeerMs;   // -1: not reached
    bool WakeTiming;                      // a wake is being timed
    uint32_t Sleeps, SleptMs, SleepsFpga, SleepsConsole;
    uint32_t RadioStarts;
} PowerStats_t;
static PowerStats_t _Pwr = {.LastAssocMs = -1, .LastIpMs = -1, .LastPeerMs = -1};

// FPGA traffic seen by the RX task, for the "CWS?" status reply.
static volatile uint32_t _FpgaPackets = 0;
static volatile uint32_t _FpgaLinkEvents = 0;
static volatile uint16_t _FpgaLast[16];   // last data per RX address (0..15)
static volatile uint32_t _FpgaRxBytes = 0;
static volatile uint16_t _FpgaAddrCount[17];  // packets per RX address (16 = any other)
static volatile uint8_t _FpgaLastAddr = 0;
static volatile uint32_t _FpgaRxCrcErrors = 0;
static volatile uint32_t _FpgaRxLenErrors = 0;

static uint32_t NowMs(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
// 1.1.7: the radio for the real time (SNTP) at power-on, when a network is saved; short, and gone as soon as the time is set
static bool TimeWantsRadio(void);

// ---- link modes and paths (PROTOCOL.md "Link modes", "Nearby (ESP-NOW)")
static volatile CwlMode_t _Mode = kCwlMode_Off;
static volatile uint32_t _ModeGen = 0;        // bumped on a mode change: the session restarts
static volatile bool _Associate = true;       // this radio start joins the AP
static volatile bool _NetHoldF = false;       // cwl_net has the radio (scan, join, phone setup)
static volatile bool _CartHoldF = false;      // a cart job has the heap (CWL_CartHold; RAM only)
static volatile bool _InSession = false;

// Where link packets go. A partner is heard on a path; the first one heard
// wins and stays while it is fresh; when several are fresh at once the
// nearby ones win (ESP-NOW, then LAN, then the relay), and a better path to
// the SAME partner (same HELLO session) is taken over at once.
typedef enum { kPath_None, kPath_EspNow, kPath_Lan, kPath_Relay, kNumPath } Path_t;
static const char *const kPathName[kNumPath] = {"none", "espnow", "lan", "relay"};
static Path_t _Path = kPath_None;
static uint32_t _PathHeardMs[kNumPath];
static bool _PathHeard[kNumPath];
static uint16_t _PathSession[kNumPath];
static uint32_t _PathSwitches = 0, _PathFiltered = 0;

// ESP-NOW (cwl_espnow.c) through the cwl_lx frame layer.
static QueueHandle_t _LxQueue = NULL;
static CwlLxBatch_t _LxBatch;
static CwlLxPeer_t _EnPeer;
static CwlLxCands_t _Cands;
static uint8_t _OwnMac[6];
static char _OwnName[16];
static uint32_t _NameSentMs = 0;
static uint32_t _LxRxFrames = 0, _LxBad = 0, _LxForeign = 0, _Pairings = 0;
static bool _WasConfirmed = false;
// Channel without an AP: the home channel (the last AP's, else EnChan),
// with a sweep of the others while nobody answers (PROTOCOL.md).
static uint8_t _EnChan = 1;
static uint32_t _SweepAtMs = 0;
static uint8_t _SweepIdx = 0;
static uint32_t _Sweeps = 0;
static uint8_t _ConnFails = 0;                // consecutive failed AP joins
enum {
    kLxQueueLen = 8,
    kPathFresh_ms = 3000,
    kNameEvery_ms = 1000,           // name beacon while looking for a partner
    kNameEveryPaired_ms = 5000,
    kHomeDwell_ms = 2000,           // channel sweep without an AP: home this long ...
    kSweepDwell_ms = 120,           // ... then each other channel this long (a HELLO_PROBE on arrival)
    kApAbsentFails = 3,             // AP joins failing in a row: treat as "no AP here"
    kApAbsentRetry_ms = 30000,      // ... then retry it this rarely (each try scans away from ESP-NOW)
    kPingEvery_ms = 50,
    kToast_ms = 3000,
};
// Latency (any path): a new REQ's first send to its ACK / CANCELLED.
static CwlLxRtt_t _XRtt;
// xstat (linkperf1): power while linked, since `cwl xstat reset`
static uint32_t _XPwrMs0[kNumPower];      // _Pwr.StateMs at the reset
static uint32_t _XPwrSince0 = 0;
static uint32_t _XNotFull = 0;            // REQ / ACK sent while not at full power
static uint32_t _XPsFixes = 0;            // full power found with Wi-Fi power save on (re-set to none)
static bool _ReqOpen = false;
static uint8_t _ReqSeq = 0;
static uint32_t _ReqMs = 0;
// Transport ping (cwl espnow ping): air round trip without the game.
static CwlLxRtt_t _PingRtt;
static volatile int _PingLeft = 0;
static volatile uint32_t _PingSent = 0, _PingGot = 0;
static uint8_t _PingId = 0;
static uint32_t _PingLastMs = 0;
static char _Toast[24];
static volatile uint32_t _ToastUntil = 0;

static void MacText(const uint8_t m[6], char *buf, size_t len)
{
    snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static bool ParseMac(const char *t, uint8_t out[6])
{
    unsigned v[6];
    if (sscanf(t, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) {
        if (v[i] > 255) return false;
        out[i] = (uint8_t)v[i];
    }
    return !CwlLx_MacIsBroadcast(out);
}

// ---------------------------------------------------------------- settings

typedef struct {
    char Ssid[33];
    char Pass[65];
    char Peer[16];       // dotted IPv4, empty = learn from the first HELLO
    uint8_t Enabled;
    uint16_t Timeout_ms;
    uint16_t Active_s, Standby_s, Armed_s;
    uint8_t Mode;        // CwlMode_t (the OSD LINK CABLE item)
    uint8_t LastMode;    // the last mode that was not OFF (cwl on)
    uint8_t EnChan;      // ESP-NOW home channel without an AP (1..13)
    char EnPeer[18];     // remembered ESP-NOW partner "AA:BB:CC:DD:EE:FF", "" = pair with the next
    char Name[16];       // display name sent to nearby sides ("" = CHROMATIC + MAC)
} CwlSettings_t;

static CwlSettings_t _Settings;

static void LoadSettings(void)
{
    memset(&_Settings, 0, sizeof(_Settings));
    _Settings.Timeout_ms = kDefaultTimeout_ms;
    _Settings.Active_s = kDefaultActive_s;
    _Settings.Standby_s = kDefaultStandby_s;
    _Settings.Armed_s = kDefaultArmed_s;
    _Settings.Mode = _Settings.LastMode = kCwlMode_Auto;   // a device that never set anything
    _Settings.EnChan = 1;
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof(_Settings.Ssid);
    (void)nvs_get_str(h, "ssid", _Settings.Ssid, &n);
    n = sizeof(_Settings.Pass);
    (void)nvs_get_str(h, "pass", _Settings.Pass, &n);
    n = sizeof(_Settings.Peer);
    (void)nvs_get_str(h, "peer", _Settings.Peer, &n);
    (void)nvs_get_u8(h, "enabled", &_Settings.Enabled);
    (void)nvs_get_u16(h, "timeout", &_Settings.Timeout_ms);
    (void)nvs_get_u16(h, "active", &_Settings.Active_s);
    (void)nvs_get_u16(h, "standby", &_Settings.Standby_s);
    (void)nvs_get_u16(h, "armed", &_Settings.Armed_s);
    uint8_t Mode = 0xff;
    if (nvs_get_u8(h, "mode", &Mode) == ESP_OK && Mode < kCwlNumMode) {
        _Settings.Mode = Mode;
    } else {
        uint8_t En = 1;   // before 0.15.0: the on/off switch only
        if (nvs_get_u8(h, "enabled", &En) == ESP_OK) _Settings.Mode = En ? kCwlMode_Auto : kCwlMode_Off;
    }
    (void)nvs_get_u8(h, "lastmode", &_Settings.LastMode);
    if (_Settings.LastMode >= kCwlMode_Off) _Settings.LastMode = kCwlMode_Auto;
    (void)nvs_get_u8(h, "enchan", &_Settings.EnChan);
    if (_Settings.EnChan < 1 || _Settings.EnChan > 13) _Settings.EnChan = 1;
    n = sizeof(_Settings.EnPeer);
    (void)nvs_get_str(h, "enpeer", _Settings.EnPeer, &n);
    n = sizeof(_Settings.Name);
    (void)nvs_get_str(h, "name", _Settings.Name, &n);
    nvs_close(h);
    _Settings.Enabled = _Settings.Mode != kCwlMode_Off;
}

static void SaveSettings(void)
{
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed");
        return;
    }
    (void)nvs_set_str(h, "ssid", _Settings.Ssid);
    (void)nvs_set_str(h, "pass", _Settings.Pass);
    (void)nvs_set_str(h, "peer", _Settings.Peer);
    (void)nvs_set_u8(h, "enabled", _Settings.Enabled);
    (void)nvs_set_u16(h, "timeout", _Settings.Timeout_ms);
    (void)nvs_set_u16(h, "active", _Settings.Active_s);
    (void)nvs_set_u16(h, "standby", _Settings.Standby_s);
    (void)nvs_set_u16(h, "armed", _Settings.Armed_s);
    (void)nvs_set_u8(h, "mode", _Settings.Mode);
    (void)nvs_set_u8(h, "lastmode", _Settings.LastMode);
    (void)nvs_set_u8(h, "enchan", _Settings.EnChan);
    (void)nvs_set_str(h, "enpeer", _Settings.EnPeer);
    (void)nvs_set_str(h, "name", _Settings.Name);
    (void)nvs_commit(h);
    nvs_close(h);
}

// ---------------------------------------------------------------- trace

// Ring of timestamped link events, dumped by `cwl trace` (debugging one
// exchange end to end against a peer's own trace).
//  E ev byte qwait | R type seq byte | T type seq byte | M byte | O byte
//  P new_power old_power | W what(1 radio on, 2 assoc, 3 ip, 4 peer, 5 radio off)
typedef struct { uint32_t Ms; char Kind; uint8_t A, B, C; } TraceEnt_t;
enum { kTraceLen = 256 };   // 2 KB of RAM (TLS for the relay API needs the heap; 0.16: 384 -> 256 for the network setup)
static TraceEnt_t _Trace[kTraceLen];
static volatile uint32_t _TraceN = 0;
static bool _TraceOn = true;

static void Trace(char Kind, uint8_t A, uint8_t B, uint8_t C)
{
    if (!_TraceOn) return;
    TraceEnt_t *t = &_Trace[_TraceN % kTraceLen];
    t->Ms = NowMs(); t->Kind = Kind; t->A = A; t->B = B; t->C = C;
    _TraceN++;
}

// ---------------------------------------------------------------- FPGA

static void FpgaSend(uint8_t Cmd, uint8_t Byte)
{
    uint8_t Buf[6] = {0x8F, kFpgaAddr_LinkCmd, 2, Cmd, Byte, 0};
    const size_t Size = crc8_sae_j1850_encode(Buf, 5, Buf);
    (void)uart_write_bytes(UART_NUM_1, Buf, Size);
}

// The FPGA build sends the stock wake preamble on CONFIG bit 1 (cwl FPGA
// 18.15+, debug bit set). Older builds would drop the first link event after
// a light sleep, so with them the MCU stays awake (Wi-Fi still goes off).
static bool FpgaHasWakePreamble(void)
{
    const uint16_t v = _FpgaLast[kStatusAddr_FWVer];
    const unsigned major = v & 0x3F, minor = (v >> 6) & 0x3F, debug = (v >> 12) & 1;
    return debug && major == 18 && minor >= 15;
}

// FPGA 18.20+ keeps an offer until the game arms (CONFIG bit 3, cmd 6).
static bool FpgaHasStanding(void)
{
    const uint16_t v = _FpgaLast[kStatusAddr_FWVer];
    const unsigned major = v & 0x3F, minor = (v >> 6) & 0x3F, debug = (v >> 12) & 1;
    return debug && major == 18 && minor >= 20;
}

static uint8_t ConfigByte(void)
{
    if (!LinkEn()) return 0;
    uint8_t c = kFpgaConfig_NetEn | kFpgaConfig_Barrier;
    // The protocol core and the FPGA must agree: decided here, where every
    // CONFIG goes out (the FPGA version arrives in its status stream).
    const bool standing = FpgaHasStanding();
    Cwl_SetStandingOffers(&_Cwl, standing);
    if (standing) c |= kFpgaConfig_Standing;
    if (_Power == kPower_Dormant && FpgaHasWakePreamble()) c |= kFpgaConfig_EspMaySleep;
    return c;
}

static void HostMasterReply(void *ctx, uint8_t byte) { (void)ctx; Trace('M', byte, 0, 0); FpgaSend(kFpgaCmd_MasterReply, byte); }
// The FPGA answers with SLAVE_DONE / SLAVE_NAK (-> Cwl_OnLocalSlaveResult).
static void HostMasterDropped(void *ctx) { (void)ctx; Trace('D', 0, 0, 0); FpgaSend(kFpgaCmd_MasterDrop, 0); }
static void HostSlaveOffer(void *ctx, uint8_t byte) { (void)ctx; Trace('O', byte, 0, 0); FpgaSend(kFpgaCmd_SlaveXfer, byte); }
static uint32_t HostNowUs(void *ctx) { (void)ctx; return (uint32_t)esp_timer_get_time(); }
static void HostSlaveWithdraw(void *ctx) { (void)ctx; Trace('X', 0, 0, 0); FpgaSend(kFpgaCmd_SlaveWithdraw, 0); }

// ---------------------------------------------------------------- network

static bool RelayWanted(void);

// What the mode uses: AUTO all paths, NEARBY ESP-NOW + LAN, ONLINE the relay
// (0.17.0 retired the ONLINE menu's "via" setting; `cwl relay force on` stays
// as a hidden relay-only, no-P2P switch).
static bool EspNowOn(void) { return _Mode == kCwlMode_Auto || _Mode == kCwlMode_Nearby; }
static bool LanOn(void) { return EspNowOn(); }
static bool RelayOn(void) { return _Mode == kCwlMode_Auto || _Mode == kCwlMode_Online; }
// Join the AP: always when one is set (LAN, relay, network window; ESP-NOW
// then rides the AP's channel).
static volatile bool _NoApTest = false;   // console `cwl espnow apjoin off`: test without an AP (not saved)
static bool WantAssoc(void) { return _Settings.Ssid[0] != '\0' && !_NoApTest; }
static bool TimeWantsRadio(void) { CwlTime_Poll(); return WantAssoc() && CwlTime_Wants(); }

static bool PathFresh(Path_t p, uint32_t now)
{
    return p != kPath_None && _PathHeard[p] && (now - _PathHeardMs[p]) < kPathFresh_ms;
}

// A CWL2 packet from the partner on path p: feed it to the core?
static bool OnPathRx(Path_t p, const uint8_t *pkt, uint32_t now)
{
    _PathHeard[p] = true;
    _PathHeardMs[p] = now;
    if (pkt[3] == kCwlType_Hello || pkt[3] == kCwlType_HelloProbe) _PathSession[p] = (uint16_t)(pkt[4] << 8 | pkt[5]);
    const Path_t old = _Path;
    if (!PathFresh(_Path, now)) {
        _Path = kPath_None;
        for (int q = kPath_EspNow; q < kNumPath; q++)
            if (PathFresh((Path_t)q, now)) { _Path = (Path_t)q; break; }   // nearby first
    } else if (p < _Path && _PathSession[p] != 0 && _PathSession[p] == _PathSession[_Path]) {
        _Path = p;   // the same partner on a better path (two Chromatics on one AP: ESP-NOW over LAN)
    }
    if (_Path != old) {
        _PathSwitches++;
        Trace('L', 150, (uint8_t)_Path, (uint8_t)old);
    }
    if (p != _Path) _PathFiltered++;
    return p == _Path;
}

static uint8_t EnChannelNow(void)
{
    CwlEnStatus_t st;
    CwlEn_GetStatus(&st);
    return CwlEn_On() ? st.channel : _EnChan;
}

static void LxFlush(void)
{
    if (_LxBatch.len == 0) return;
    uint8_t d[6];
    CwlLx_PeerDest(&_EnPeer, d);
    (void)CwlEn_Send(d, _LxBatch.buf, _LxBatch.len);
    CwlLx_BatchClear(&_LxBatch);
}

static void LxAddCwl(const uint8_t pkt[kCwlPacketLen])
{
    if (!CwlLx_BatchAddCwl(&_LxBatch, pkt)) { LxFlush(); (void)CwlLx_BatchAddCwl(&_LxBatch, pkt); }
}
static void LxAddPing(bool pong, uint8_t id, uint32_t t)
{
    if (!CwlLx_BatchAddPing(&_LxBatch, pong, id, t)) { LxFlush(); (void)CwlLx_BatchAddPing(&_LxBatch, pong, id, t); }
}
// The name beacon is always broadcast (even when paired): every side nearby
// can list us.
static void LxBeaconName(void)
{
    CwlLxBatch_t b;
    CwlLx_BatchInit(&b, kCwlLx_MaxFrame);
    if (!CwlLx_BatchAddName(&b, _OwnName)) return;
    static const uint8_t bc[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    (void)CwlEn_Send(bc, b.buf, b.len);
}

static void NoteReqSent(const uint8_t *pkt, uint32_t now)
{
    if (pkt[3] == kCwlType_Req && (!_ReqOpen || pkt[4] != _ReqSeq)) {
        _ReqOpen = true;
        _ReqSeq = pkt[4];
        _ReqMs = now;
    }
}
static void NoteReply(const uint8_t *pkt, uint32_t now)
{
    if (_ReqOpen && (pkt[3] == kCwlType_Ack || pkt[3] == kCwlType_Cancelled) && pkt[4] == _ReqSeq) {
        _ReqOpen = false;
        CwlLx_RttAdd(&_XRtt, now - _ReqMs);
    }
}

static void LanSend(const uint8_t pkt[kCwlPacketLen]);

static void HostSend(void *ctx, const uint8_t pkt[kCwlPacketLen])
{
    (void)ctx;
    if ((pkt[3] == kCwlType_Req || pkt[3] == kCwlType_Ack) && _Power != kPower_Full) _XNotFull++;
    if (pkt[3] != kCwlType_Hello) Trace('T', pkt[3], pkt[4], pkt[5]);
    const uint32_t now = NowMs();
    NoteReqSent(pkt, now);
    const Path_t p = PathFresh(_Path, now) ? _Path : kPath_None;
    if (p == kPath_None && pkt[3] != kCwlType_Hello && pkt[3] != kCwlType_HelloProbe) {
        // No partner path: only discovery goes out. (The core sends exchanges
        // only to a live peer, which is heard on a fresh path; this guards
        // against a request reaching two different partners at once.)
        return;
    }
    if ((p == kPath_None || p == kPath_EspNow) && EspNowOn() && CwlEn_On()) LxAddCwl(pkt);   // flushed at the end of the pass
    if ((p == kPath_None || p == kPath_Lan) && LanOn()) LanSend(pkt);
    if ((p == kPath_None || p == kPath_Relay) && RelayOn() && RelayWanted() && _Sock >= 0 && _WifiConnected &&
        Relay_Seated(&_Relay)) {
        (void)Relay_Send(&_Relay, pkt, kCwlPacketLen, now);
    }
}

static void LanSend(const uint8_t pkt[kCwlPacketLen])
{
    if (_Sock < 0 || !_WifiConnected) return;
    struct sockaddr_in To;
    if (_PeerKnown) {
        To = _Peer;
    } else {
        memset(&To, 0, sizeof(To));
        To.sin_family = AF_INET;
        To.sin_port = htons(kCwlPort);
        To.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    }
    (void)sendto(_Sock, pkt, kCwlPacketLen, 0, (struct sockaddr *)&To, sizeof(To));
}

static void WifiEvent(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (_Associate) (void)esp_wifi_connect();   // no AP set: ESP-NOW only
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *ev = (const wifi_event_sta_connected_t *)data;
        _ChannelHint = ev->channel;
        if (_Pwr.WakeTiming && _Pwr.LastAssocMs < 0) _Pwr.LastAssocMs = (int32_t)(NowMs() - _Pwr.WakeStartMs);
        Trace('W', 2, ev->channel, 0);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = (const wifi_event_sta_disconnected_t *)data;
        _WifiConnected = false;
        if (ev->reason == WIFI_REASON_NO_AP_FOUND) _ChannelHint = 0;   // the AP moved: full scan next
        // Reconnect from the link task, with a backoff (no AP in range must
        // not keep the radio scanning flat out).
        if (_ConnFails < 255) _ConnFails++;
        if (_RadioWanted && _Associate && _ReconnectAt == 0) {
            // With ESP-NOW on, an AP that keeps failing is tried rarely: each
            // try scans the channels and ESP-NOW goes deaf meanwhile.
            const uint32_t wait = (_ConnFails >= kApAbsentFails && CwlEn_On()) ? kApAbsentRetry_ms : _ReconnectBackoff;
            const uint32_t at = NowMs() + wait;
            _ReconnectAt = at ? at : 1;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        _OwnIp = ev->ip_info.ip.addr;
        if (!_StaticRejoin) {
            _Lease = ev->ip_info;
            _LeaseValid = true;
            _LeaseMs = NowMs();
            memset(_LeaseDns, 0, sizeof(_LeaseDns));
            if (_Netif != NULL) {
                (void)esp_netif_get_dns_info(_Netif, ESP_NETIF_DNS_MAIN, &_LeaseDns[0]);
                (void)esp_netif_get_dns_info(_Netif, ESP_NETIF_DNS_BACKUP, &_LeaseDns[1]);
            }
        }
        _WifiConnected = true;
        CwlTime_OnGotIp();
        _ConnFails = 0;
        _ReconnectBackoff = kReconnectMin_ms;
        if (_Pwr.WakeTiming && _Pwr.LastIpMs < 0) _Pwr.LastIpMs = (int32_t)(NowMs() - _Pwr.WakeStartMs);
        Trace('W', 3, 0, 0);
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&ev->ip_info.ip));
    }
}

static esp_err_t WifiInit(void)
{
    if (_WifiInited) return ESP_OK;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    _Netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;
    (void)esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, WifiEvent, NULL);
    (void)esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, WifiEvent, NULL);
    (void)esp_wifi_set_storage(WIFI_STORAGE_RAM);
    (void)esp_wifi_set_mode(WIFI_MODE_STA);
    _WifiInited = true;
    return ESP_OK;
}

static void ApplyStaConfig(void)
{
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, _Settings.Ssid, sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, _Settings.Pass, sizeof(wc.sta.password));
    wc.sta.channel = _ChannelHint;              // known channel: no full scan on a wake
    wc.sta.listen_interval = kListenInterval;   // modem sleep (kPower_Low)
    (void)esp_wifi_set_config(WIFI_IF_STA, &wc);
}

static void CloseSocket(void)
{
    if (_Sock >= 0) {
        close(_Sock);
        _Sock = -1;
    }
}

static void EnStart(void);

static void RadioOn(uint32_t now)
{
    _RadioWanted = true;
    if (_RadioStarted) {
        EnStart();
        return;
    }
    if (WifiInit() != ESP_OK) {
        printf("cwl: wifi init failed\n");
        return;
    }
    _Associate = WantAssoc();
    _ConnFails = 0;
    if (!_Associate) {
        // No AP set: the radio for ESP-NOW only, on the home channel.
        if (esp_wifi_start() == ESP_OK) {
            _RadioStarted = true;
            _RadioStartMs = now;
            _Pwr.RadioStarts++;
            Trace('W', 1, _EnChan, 2);
            EnStart();
        }
        return;
    }
    ApplyStaConfig();
    _ReconnectAt = 0;
    _ReconnectBackoff = kReconnectMin_ms;
    // Fast rejoin: the last lease's address right at association (GOT_IP
    // follows CONNECTED), no DHCP round trips. Otherwise a normal DHCP.
    _StaticRejoin = _Netif != NULL && _LeaseValid && (now - _LeaseMs) < kLeaseReuse_ms;
    if (_Netif != NULL) {
        if (_StaticRejoin) {
            (void)esp_netif_dhcpc_stop(_Netif);
            (void)esp_netif_set_ip_info(_Netif, &_Lease);
            // The lease's DNS servers (or the gateway, which is a resolver on
            // most home routers, when the lease named none).
            esp_netif_dns_info_t d = _LeaseDns[0];
            if (d.ip.u_addr.ip4.addr == 0) {
                d.ip.type = ESP_IPADDR_TYPE_V4;
                d.ip.u_addr.ip4.addr = _Lease.gw.addr;
            }
            (void)esp_netif_set_dns_info(_Netif, ESP_NETIF_DNS_MAIN, &d);
            if (_LeaseDns[1].ip.u_addr.ip4.addr != 0) (void)esp_netif_set_dns_info(_Netif, ESP_NETIF_DNS_BACKUP, &_LeaseDns[1]);
        } else {
            (void)esp_netif_dhcpc_start(_Netif);   // netif down: back to DHCP on connect
        }
    }
    if (esp_wifi_start() == ESP_OK) {
        _RadioStarted = true;
        _RadioStartMs = now;
        _Pwr.RadioStarts++;
        Trace('W', 1, _ChannelHint, _StaticRejoin);
        EnStart();
    }
}

// ESP-NOW on the started radio (the AP's channel once associated).
static void EnStart(void)
{
    if (!LinkEn() || !EspNowOn() || !_RadioStarted || CwlEn_On()) return;
    _EnChan = _ChannelHint ? _ChannelHint : _Settings.EnChan;
    const esp_err_t err = CwlEn_Start(_LxQueue, _EnChan);
    if (err != ESP_OK) printf("cwl: esp-now start failed (%d)\n", err);
    _SweepAtMs = NowMs() + kHomeDwell_ms;
    _SweepIdx = 0;
}

static void RadioOff(void)
{
    _RadioWanted = false;
    _ReconnectAt = 0;
    CloseSocket();
    CwlEn_Stop();
    if (!_RadioStarted) return;
    _WifiConnected = false;
    (void)esp_wifi_disconnect();
    (void)esp_wifi_stop();
    _RadioStarted = false;
    Trace('W', 5, 0, 0);
}

static int OpenSocket(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return -1;
    int yes = 1;
    (void)setsockopt(s, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    struct timeval tv = {.tv_sec = 0, .tv_usec = 1000};
    (void)setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(kCwlPort);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(s);
        return -1;
    }
    return s;
}

// ---------------------------------------------------------------- power

static void SetPower(Power_t p, uint32_t now)
{
    const Power_t old = _Power;
    if (p == old) return;
    _Pwr.StateMs[old] += now - _Pwr.StateSinceMs;
    _Pwr.StateSinceMs = now;
    Trace('P', (uint8_t)p, (uint8_t)old, 0);

    _SleepOk = false;   // no light sleep while Wi-Fi is being switched
    if (p == kPower_Off || p == kPower_Dormant) {
        _Power = p;   // first, so CONFIG carries the may-sleep bit
        FpgaSend(kFpgaCmd_Config, ConfigByte());
        (void)uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(20));
        RadioOff();
        _WakeReq = false;
        _SleepOk = true;    // Wi-Fi is off: from here on the MCU may light-sleep
        return;
    }
    // Low / Full: the MCU stays awake (CWL_KeepAwake) from here on.
    _Power = p;
    if (old == kPower_Off || old == kPower_Dormant) {
        FpgaSend(kFpgaCmd_Config, ConfigByte());   // no wake preamble any more
        RadioOn(now);
    }
    if (_RadioStarted && _Associate) (void)esp_wifi_set_ps(p == kPower_Full ? WIFI_PS_NONE : WIFI_PS_MAX_MODEM);
    else CwlEn_SetLowPower(p != kPower_Full);   // no AP: connectionless modem sleep
}

// A link-port event while Wi-Fi is off: bring it back, and hold a master for
// the peer meanwhile (instead of the "unplugged" 0xFF).
static void Wake(uint32_t now)
{
    _Pwr.Wakes++;
    _Pwr.WakeStartMs = now;
    _Pwr.LastAssocMs = _Pwr.LastIpMs = _Pwr.LastPeerMs = -1;
    _Pwr.WakeTiming = true;
    uint32_t hold = kFpgaSafety_ms > _Settings.Timeout_ms ? kFpgaSafety_ms - _Settings.Timeout_ms : 0;
    if (hold > kMaxHold_ms) hold = kMaxHold_ms;
    Cwl_HoldForPeer(&_Cwl, now, hold);
    SetPower(kPower_Full, now);
}

static Power_t LinkWantedPower(uint32_t now);

static Power_t WantedPower(uint32_t now)
{
    Power_t p = LinkWantedPower(now);
    // Pairing / an HTTPS call waiting: radio up (they are short). Receive mode polls every few
    // seconds for minutes: the radio stays up (associated, no power save) the whole time.
    if (CwlOnline_NeedsRadio() || _RecvHoldF || TimeWantsRadio()) return kPower_Full;
    if (_KeepActive && p < kPower_Low) p = kPower_Low;   // seat keeper ping
    if (_NwWanted) {
        // A game using the window: flat out for a while after each request,
        // and never below modem sleep (radio associated, MCU awake) while it
        // holds the window open.
        if (_NwUsed && (now - _NwUseMs) < (uint32_t)_Settings.Active_s * 1000u) p = kPower_Full;
        else if (_NwUnlocked && p < kPower_Low) p = kPower_Low;
    }
    return p;
}

static Power_t LinkWantedPower(uint32_t now)
{
    const uint32_t idle = now - _LastUseMs;
    const bool busy = LinkEn() && (Cwl_Busy(&_Cwl) || Cwl_Holding(&_Cwl, now));
    if (busy || idle < (uint32_t)_Settings.Active_s * 1000u) {
        // In use: flat out with a partner (or while waiting for one after a
        // wake), modem sleep while nobody answers (a game polling the port).
        // (and flat out for the first seconds of joining Wi-Fi: faster DHCP;
        // not for long, a missing AP must not keep the radio flat out)
        const bool Joining = _Associate && !_WifiConnected && (now - _RadioStartMs) < kJoinFull_ms;
        if (!LinkEn()) return Joining ? kPower_Full : kPower_Low;   // network window use only
        return (Cwl_PeerAlive(&_Cwl, now) || Cwl_Holding(&_Cwl, now) || Joining) ? kPower_Full : kPower_Low;
    }
    if (idle < (uint32_t)_Settings.Standby_s * 1000u) return kPower_Low;
    if (_PortArmed && (now - _ArmedSinceMs) < (uint32_t)_Settings.Armed_s * 1000u) return kPower_Low;
    return kPower_Dormant;
}

static void NoteLinkUse(uint32_t now) { _LastUseMs = now; }

// Announce our address right after (re)joining: a peer whose ARP entry went
// stale while we were off (macOS marks the host "down" and fails every send
// for a while) learns us again at once, instead of after its own ARP retry.
static void GratuitousArp(void *arg)
{
    (void)arg;
    for (struct netif *n = netif_list; n != NULL; n = n->next) {
        if (netif_is_up(n) && netif_is_link_up(n) && !ip4_addr_isany_val(*netif_ip4_addr(n)) &&
            (n->flags & NETIF_FLAG_ETHARP)) {
            (void)etharp_gratuitous(n);
        }
    }
}

// ---------------------------------------------------------------- task

#if defined(CONFIG_SHINY_RELAY_DEBUG_STATUS_PROBE)
// Reply to a "CWS?" UDP probe with one line of counters: proves the FPGA is
// alive and talking to the ESP32 without the USB console. Debug builds only
// (0.17.1): it answers anyone, names the Wi-Fi network and the room code and
// is ~250x the request (see components/cwl/Kconfig).
static void SendStatus(const struct sockaddr_in *To)
{
    const uint32_t now = NowMs();
    char Line[900];
    int Len = snprintf(Line, sizeof(Line),
        "CWS fpga_pkts=%lu link_events=%lu fwver=0x%04x buttons=0x%04x "
        "master_done=%lu master_timeouts=%lu slave_done=%lu refused=%lu rx=%lu peer=%d "
        "restarts=%lu probes=%lu peer_resets=%lu cancels=%lu max_wait=%lu "
        "abandons=%lu unknown_abandons=%lu collisions=%lu "
        "uart_bytes=%lu crc_err=%lu len_err=%lu "
        "power=%s wakes=%lu held=%lu wake_ms=%ld/%ld/%ld sleeps=%lu slept_ms=%lu "
        "ms_dormant=%lu ms_low=%lu ms_full=%lu\n",
        (unsigned long)_FpgaPackets, (unsigned long)_FpgaLinkEvents,
        _FpgaLast[kStatusAddr_FWVer], _FpgaLast[kStatusAddr_Buttons],
        (unsigned long)_Cwl.stats.master_done, (unsigned long)_Cwl.stats.master_timeouts,
        (unsigned long)_Cwl.stats.slave_done, (unsigned long)_Cwl.stats.slave_offers_refused,
        (unsigned long)_Cwl.stats.rx_packets, Cwl_PeerAlive(&_Cwl, now) ? 1 : 0,
        (unsigned long)_Cwl.stats.master_restarts, (unsigned long)_Cwl.stats.offer_probes,
        (unsigned long)_Cwl.stats.peer_resets, (unsigned long)_Cwl.stats.cancels,
        (unsigned long)_Cwl.stats.max_master_wait_ms,
        (unsigned long)_Cwl.stats.master_abandons, (unsigned long)_Cwl.stats.unknown_abandons,
        (unsigned long)_Cwl.stats.master_collisions,
        (unsigned long)_FpgaRxBytes, (unsigned long)_FpgaRxCrcErrors, (unsigned long)_FpgaRxLenErrors,
        kPowerName[_Power], (unsigned long)_Pwr.Wakes, (unsigned long)_Cwl.stats.held_masters,
        (long)_Pwr.LastAssocMs, (long)_Pwr.LastIpMs, (long)_Pwr.LastPeerMs,
        (unsigned long)_Pwr.Sleeps, (unsigned long)_Pwr.SleptMs,
        (unsigned long)(_Pwr.StateMs[kPower_Dormant] + (_Power == kPower_Dormant ? now - _Pwr.StateSinceMs : 0)),
        (unsigned long)(_Pwr.StateMs[kPower_Low] + (_Power == kPower_Low ? now - _Pwr.StateSinceMs : 0)),
        (unsigned long)(_Pwr.StateMs[kPower_Full] + (_Power == kPower_Full ? now - _Pwr.StateSinceMs : 0)));
    if (Len > 0 && Len < (int)sizeof(Line)) {
        Len--;   // drop the newline, append per-address counts
        Len += snprintf(Line + Len, sizeof(Line) - (size_t)Len, " last_addr=0x%02x addrs=", _FpgaLastAddr);
        for (int a = 0; a < 17 && Len < (int)sizeof(Line) - 24; a++) {
            if (_FpgaAddrCount[a] == 0) continue;
            Len += snprintf(Line + Len, sizeof(Line) - (size_t)Len, "%x:%u/%04x,", a, (unsigned)_FpgaAddrCount[a],
                            a < 16 ? _FpgaLast[a] : 0);
        }
        if (Len < (int)sizeof(Line) - 120) {
            Len += snprintf(Line + Len, sizeof(Line) - (size_t)Len, " relay=%d/%s/%d/%s rtt=%u/%u",
                            (int)_Relay.state, _Relay.code, _Relay.peer_present ? 1 : 0,
                            Relay_DirectAlive(&_Relay, now) ? "direct" : "relay", _Relay.relay_rtt_ms, _Relay.direct_rtt_ms);
        }
        if (Len < (int)sizeof(Line) - 200) {
            char pm[18] = "-";
            if (_EnPeer.locked) MacText(_EnPeer.mac, pm, sizeof(pm));
            Len += snprintf(Line + Len, sizeof(Line) - (size_t)Len,
                            " mode=%s path=%s en=%d/%u/%s/%d%d xrtt=%u/%u/%u/%lu ping=%u/%u/%lu/%lu heap=%u/%u",
                            CWL_ModeText(_Mode), kPathName[_Path], CwlEn_On() ? 1 : 0, (unsigned)EnChannelNow(), pm,
                            _EnPeer.confirmed ? 1 : 0, _EnPeer.fixed ? 1 : 0,
                            (unsigned)CwlLx_RttPct(&_XRtt, 50), (unsigned)CwlLx_RttPct(&_XRtt, 90),
                            (unsigned)_XRtt.max_ms, (unsigned long)_XRtt.n,
                            (unsigned)CwlLx_RttPct(&_PingRtt, 50), (unsigned)CwlLx_RttPct(&_PingRtt, 90),
                            (unsigned long)_PingSent, (unsigned long)_PingGot,
                            (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
        }
        if (Len < (int)sizeof(Line) - 60) Len += CwlNet_StatusExtra(Line + Len, (int)sizeof(Line) - Len);
        if (_StatusExtra != NULL && Len < (int)sizeof(Line) - 2) {
            const int x = _StatusExtra(Line + Len, (int)sizeof(Line) - Len - 2);
            if (x > 0 && x < (int)sizeof(Line) - Len - 2) Len += x;
        }
        Len += snprintf(Line + Len, sizeof(Line) - (size_t)Len, "\n");
    }
    if (Len > 0 && Len < (int)sizeof(Line)) {
        (void)sendto(_Sock, Line, (size_t)Len, 0, (const struct sockaddr *)To, sizeof(*To));
    }
}
#endif   // CONFIG_SHINY_RELAY_DEBUG_STATUS_PROBE

static void HandleFpgaEvent(const CtlMsg_t *m, uint32_t now)
{
    Trace('E', m->A, m->B, (uint8_t)(now - m->U));   // C = queue wait ms
    // A NAK only answers our own offer probe: not the game using the port.
    if (m->A != kCwlFpgaEvent_SlaveNak && m->A != kCwlFpgaEvent_SlaveNakStalled) NoteLinkUse(now);
    switch (m->A) {
    case kCwlFpgaEvent_Armed:
        if (!_PortArmed) _ArmedSinceMs = now;
        _PortArmed = true;
        break;
    case kCwlFpgaEvent_SlaveNak:
    case kCwlFpgaEvent_SlaveNakStalled:
        break;                        // an offer found the port unarmed: no news
    default:
        _PortArmed = false;           // disarmed, became master, or exchanged
        break;
    }
    if (_Power == kPower_Dormant) Wake(now);   // before the core sees a MASTER
    switch (m->A) {
    case kCwlFpgaEvent_Armed:     Cwl_OnLocalArmed(&_Cwl, now); break;
    case kCwlFpgaEvent_Disarm:    Cwl_OnLocalDisarm(&_Cwl, now); break;
    case kCwlFpgaEvent_Master:    Cwl_OnLocalMaster(&_Cwl, m->B, now); break;
    case kCwlFpgaEvent_SlaveDone: Cwl_OnLocalSlaveResult(&_Cwl, true, m->B, now); break;
    case kCwlFpgaEvent_SlaveNak:  Cwl_OnLocalSlaveResult(&_Cwl, false, 0, now); break;
    case kCwlFpgaEvent_Abandon:   Cwl_OnLocalMasterAbandonB(&_Cwl, m->B, now); break;
    case kCwlFpgaEvent_SlaveNakStalled:
        // With standing offers event 7 only reports the collision: the offer
        // keeps standing until we withdraw it.
        if (!_Cwl.standing) Cwl_OnLocalSlaveResult(&_Cwl, false, 0, now);
        Cwl_OnLocalMasterCollision(&_Cwl, now);
        break;
    default: break;
    }
}

// ---------------------------------------------------------------- relay

static void RelayHostSend(void *ctx, SlEp_t to, const uint8_t *buf, size_t n)
{
    (void)ctx;
    if (_Sock < 0 || !_WifiConnected) return;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(to.port);
    a.sin_addr.s_addr = htonl(to.ip);
    (void)sendto(_Sock, buf, n, 0, (struct sockaddr *)&a, sizeof(a));
}

static void RelayDeliver(void *ctx, const uint8_t *p, size_t n, bool direct)
{
    (void)ctx;
    if (!LinkEn()) return;
    const uint32_t now = NowMs();
    _RelayDelivered++;
    if (direct) _RelayDeliveredDirect++;
    if (n != kCwlPacketLen || p[0] != 'C' || p[1] != 'W' || p[2] != kCwlVersion) return;
    if (!OnPathRx(kPath_Relay, p, now)) return;   // a nearby partner has the link
    const bool Exchange = p[3] != kCwlType_Hello && p[3] != kCwlType_HelloProbe && p[3] != kCwlType_Caps;
    if (Exchange) Trace('R', p[3], p[4], p[5]);
    NoteReply(p, now);
    if (Cwl_OnPacket(&_Cwl, p, n, now) && Exchange) NoteLinkUse(now);
}

static void RelayEvent(void *ctx, RelayEvent_t ev, int arg)
{
    (void)ctx;
    Trace('L', (uint8_t)ev, (uint8_t)arg, 0);
    switch (ev) {
    case kRelayEv_Seated:
        printf("cwl relay: seated in room %s (slot %d)\n", _Relay.code, arg);
        if (_Relay.orig_mode == kSl_ModeHost) CwlOnline_ReportCode(_Relay.code);
        break;
    case kRelayEv_Refused: printf("cwl relay: JOIN refused (%s)\n", Relay_RefuseName((uint8_t)arg)); break;
    case kRelayEv_NeedTicket: CwlOnline_WantTicket(); break;
    case kRelayEv_PeerPresent: printf("cwl relay: partner present%s\n", arg ? " (relay only)" : ""); break;
    case kRelayEv_PeerAbsent: printf("cwl relay: partner left\n"); break;
    case kRelayEv_DirectUp: printf("cwl relay: direct path up\n"); break;
    case kRelayEv_DirectDown: printf("cwl relay: direct path lost, via relay\n"); break;
    case kRelayEv_Rejoin: printf("cwl relay: seat forgotten, rejoining\n"); break;
    }
}

static uint32_t RelayRand(void *ctx) { (void)ctx; return esp_random(); }

static void RelayInit(uint32_t now)
{
    const RelayHost_t h = {NULL, RelayHostSend, RelayDeliver, RelayEvent, RelayRand};
    Relay_Init(&_Relay, &h, now);
    _RelayStarted = false;
    _RelayUp = false;
}

static bool RelayWanted(void)
{
    return LinkEn() && RelayOn() && CwlOnline_Paired();
}

static SlEp_t OwnEp(void)
{
    const SlEp_t e = {ntohl(_OwnIp), kCwlPort};
    return e;
}

static void RelayStop(void)
{
    if (_Relay.state != kRelay_Idle) Relay_Stop(&_Relay);   // BYE when seated
    _RelayStarted = false;
}

// Once per link-task pass: seat management, tickets, keepalives.
static void RelayService(uint32_t now)
{
    if (!RelayWanted()) {
        if (_RelayStarted || _Relay.state != kRelay_Idle) RelayStop();
        return;
    }
    const uint32_t gen = CwlOnline_Generation();
    if (gen != _RelayGen) {
        // Room / transport / force / pairing changed: leave and start over.
        _RelayGen = gen;
        RelayStop();
    }
    Relay_SetP2p(&_Relay, !CwlOnline_ForceRelay());
    // Seat keeper (see kSeatKeepEvery_ms).
    if (_KeepActive) {
        if ((int32_t)(_Relay.last_relay_heard_ms - _KeepStartMs) > 0) {
            _KeepActive = false;   // the relay answered: seat (and NAT mapping) refreshed
            _KeepsAnswered++;
        } else if (now - _KeepStartMs > kSeatKeepMax_ms) {
            _KeepActive = false;
        }
    }
    _KeepArmed = Relay_Seated(&_Relay) && (now - _LastUseMs) < kSeatKeepFor_ms;
    if (_KeepArmed) _KeepDueMs = _Relay.last_relay_ping_ms + kSeatKeepEvery_ms;
    if (_KeepArmed && _Power == kPower_Dormant && !_KeepActive && (int32_t)(now - _KeepDueMs) >= 0) {
        _KeepActive = true;
        _KeepStartMs = now;
        _Keeps++;
        Trace('L', 100, 0, 0);
    }
    const bool up = _Sock >= 0 && _WifiConnected && (_Power == kPower_Low || _Power == kPower_Full);
    if (!up) {
        _RelayUp = false;   // dormant: send nothing (the seat survives 10 min on the relay)
        return;
    }
    if (!_RelayUp) {
        // Back from a radio-off spell: old token first (the relay rebinds our
        // new address); a forgotten seat answers unknown_session -> rejoin.
        _RelayUp = true;
        Relay_SetLocal(&_Relay, OwnEp());
        if (_RelayStarted) Relay_Resume(&_Relay, now);
    }
    Relay_SetLowPower(&_Relay, _Power == kPower_Low);
    // Tickets: keep one ready while the radio is up (fetched by the online
    // task), so a JOIN never waits for HTTPS when a ticket can be had early.
    if (!CwlOnline_HaveTicket(now)) CwlOnline_WantTicket();
    const bool needs = !_RelayStarted || (_Relay.state == kRelay_NeedTicket && !_Relay.retry_pending);
    if (needs && CwlOnline_HaveTicket(now)) {
        CwlTicket_t *t = malloc(sizeof(*t));
        if (t != NULL && CwlOnline_TakeTicket(t, now)) {
            Relay_SetTicket(&_Relay, t->ticket, t->len, now);
            if (!_RelayStarted) {
                Relay_Start(&_Relay, t->relay, t->mode, t->code, t->client_id, OwnEp(), now);
                _RelayStarted = true;
            } else {
                Relay_Resume(&_Relay, now);
            }
            memset(t->ticket, 0, sizeof(t->ticket));
        }
        free(t);
    }
    Relay_Poll(&_Relay, now);
}

// ---------------------------------------------------------------- nearby (ESP-NOW)

typedef struct { const uint8_t *src; uint32_t now; bool deliver; } LxCtx_t;

static void LxOnCwl(void *ctx, const uint8_t p[kCwlLx_CwlLen])
{
    LxCtx_t *c = ctx;
    if (!c->deliver) return;
    if (!OnPathRx(kPath_EspNow, p, c->now)) return;
    const bool Exchange = p[3] != kCwlType_Hello && p[3] != kCwlType_HelloProbe && p[3] != kCwlType_Caps;
    if (Exchange) Trace('R', p[3], p[4], p[5]);
    NoteReply(p, c->now);
    if (Cwl_OnPacket(&_Cwl, p, kCwlPacketLen, c->now) && Exchange) NoteLinkUse(c->now);
}

static void LxOnPing(void *ctx, bool pong, uint8_t id, uint32_t t)
{
    LxCtx_t *c = ctx;
    if (!c->deliver) return;
    if (!pong) {
        LxAddPing(true, id, t);   // echo the sender's clock
    } else {
        CwlLx_RttAdd(&_PingRtt, c->now - t);
        _PingGot++;
    }
}

static void LxOnName(void *ctx, const char *name, size_t n)
{
    LxCtx_t *c = ctx;
    CwlLx_CandName(&_Cands, c->src, name, n);
}

static void ShowToast(const char *text)
{
    snprintf(_Toast, sizeof(_Toast), "%s", text);
    _ToastUntil = NowMs() + kToast_ms;
}

static const char *CandName(const uint8_t mac[6])
{
    for (int i = 0; i < kCwlLx_MaxCand; i++)
        if (_Cands.c[i].used && memcmp(_Cands.c[i].mac, mac, 6) == 0) return _Cands.c[i].name;
    return NULL;
}

// The lock just became mutual with a side we had not paired with: remember it.
static void Pair(const uint8_t mac[6])
{
    char t[18];
    MacText(mac, t, sizeof(t));
    snprintf(_Settings.EnPeer, sizeof(_Settings.EnPeer), "%s", t);
    SaveSettings();
    CwlLx_PeerFix(&_EnPeer, mac);
    _Pairings++;
    const char *n = CandName(mac);
    char def[16];
    if (n == NULL) { CwlLx_DefaultName(mac, def, sizeof(def)); n = def; }
    char msg[24];
    snprintf(msg, sizeof(msg), "LINKED  %s", n);
    ShowToast(msg);
    printf("cwl: paired with %s (%s)\n", n, t);
    Trace('L', 160, mac[4], mac[5]);
}

static void LxReceive(const CwlRadioRx_t *r, uint32_t now)
{
    if (!LinkEn() || !EspNowOn() || r->medium != kCwlMed_EspNow) return;
    _LxRxFrames++;
    LxCtx_t c = {r->src, now, false};
    const CwlLxSink_t count = {&c, NULL, NULL, LxOnName};   // names first: the pick list
    const int n = CwlLx_Split(r->f, r->len, &count);
    if (n < 0) { _LxBad++; return; }
    CwlLx_CandSeen(&_Cands, r->src, r->rssi, now);
    (void)CwlLx_Split(r->f, r->len, &count);                 // a new candidate gets its name now
    if (n > 0) {
        if (!CwlLx_PeerAccept(&_EnPeer, r->src, r->unicast != 0, now)) { _LxForeign++; return; }
        if (_EnPeer.confirmed && !_WasConfirmed && !_EnPeer.fixed) Pair(r->src);
        _WasConfirmed = _EnPeer.confirmed;
    } else if (!(_EnPeer.locked && memcmp(_EnPeer.mac, r->src, 6) == 0)) {
        return;   // a name / ping from someone else: the pick list only
    }
    c.deliver = true;
    const CwlLxSink_t sink = {&c, LxOnCwl, LxOnPing, NULL};
    (void)CwlLx_Split(r->f, r->len, &sink);
}

// Once per pass while ESP-NOW is on: lost / unconfirmed partners, the name
// beacon, and the channel sweep without an AP.
static void NearbyService(uint32_t now)
{
    if (CwlLx_PeerPoll(&_EnPeer, now)) {
        _WasConfirmed = false;
        Trace('L', 161, 0, 0);
    }
    const uint32_t every = _EnPeer.confirmed ? kNameEveryPaired_ms : kNameEvery_ms;
    if (now - _NameSentMs >= every) {
        _NameSentMs = now;
        LxBeaconName();
    }
    // Channel. With an AP the radio stays on its channel: a partner on the same
    // AP is there too, one elsewhere finds us with its own sweep. Without one
    // (none set, or it kept failing) stay on the home channel, and while nobody
    // is heard visit every other channel for 120 ms every 2 s, sending a
    // HELLO_PROBE on arrival (answered at once). A partner heard pins it.
    if (_WifiConnected || PathFresh(kPath_EspNow, now)) return;
    if (_Associate && _ConnFails < kApAbsentFails) return;   // a join scan is moving the channel anyway
    if ((int32_t)(now - _SweepAtMs) < 0) return;
    static const uint8_t kOrder[] = {1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13};
    uint8_t ch;
    if (_SweepIdx >= sizeof(kOrder)) {
        _SweepIdx = 0;
        ch = _EnChan;              // home again
        _SweepAtMs = now + kHomeDwell_ms;
        _Sweeps++;
    } else {
        ch = kOrder[_SweepIdx++];
        if (ch == _EnChan) {       // home is not a sweep stop
            if (_SweepIdx >= sizeof(kOrder)) { _SweepAtMs = now; return; }
            ch = kOrder[_SweepIdx++];
        }
        _SweepAtMs = now + kSweepDwell_ms;
    }
    (void)esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    const uint8_t probe[kCwlPacketLen] = {'C', 'W', kCwlVersion, kCwlType_HelloProbe, (uint8_t)(_Cwl.session >> 8), (uint8_t)_Cwl.session};
    LxAddCwl(probe);
    LxBeaconName();
}

static void LinkCoreInit(uint32_t now)
{
    const CwlHost_t Host = {NULL, HostSend, HostMasterReply, HostSlaveOffer, HostMasterDropped, HostSlaveWithdraw,
                            NULL, NULL, HostNowUs};   // send-ahead (lock) needs FPGA 18.22: not wired in this build
    Cwl_Init(&_Cwl, &Host, now);
    Cwl_SetSession(&_Cwl, (uint16_t)esp_random());
    _Cwl.master_timeout_ms = _Settings.Timeout_ms;

    CwlLx_BatchInit(&_LxBatch, kCwlLx_MaxFrame);
    uint8_t m[6];
    if (_Settings.EnPeer[0] != '\0' && ParseMac(_Settings.EnPeer, m)) CwlLx_PeerInit(&_EnPeer, m);
    else CwlLx_PeerInit(&_EnPeer, NULL);
    _WasConfirmed = _EnPeer.confirmed;
    memset(&_Cands, 0, sizeof(_Cands));
    _Path = kPath_None;
    memset(_PathHeard, 0, sizeof(_PathHeard));
    memset(_PathSession, 0, sizeof(_PathSession));
    _ReqOpen = false;

    _PeerFixed = _PeerKnown = false;
    if (_Settings.Peer[0] != '\0') {
        memset(&_Peer, 0, sizeof(_Peer));
        _Peer.sin_family = AF_INET;
        _Peer.sin_port = htons(kCwlPort);
        if (inet_aton(_Settings.Peer, &_Peer.sin_addr)) _PeerFixed = _PeerKnown = true;
    }
}

// Runs while the link cable or the network window service is on.
static void LinkSession(void)
{
    uint32_t now = NowMs();
    LinkCoreInit(now);
    RelayInit(now);
    _RelayGen = CwlOnline_Generation();
    bool LinkWas = LinkEn();

    // Start in standby: on Wi-Fi in modem sleep until a game uses the port,
    // Wi-Fi off after the standby time. With only the network service on,
    // Wi-Fi stays off until a game opens the window.
    _LastUseMs = now;
    _PortArmed = false;
    SetPower(LinkEn() ? kPower_Low : kPower_Dormant, now);
    uint32_t LastConfig = now;
    bool WasAlive = false;
    bool WasConnected = false;
    int GarpLeft = 0;
    uint32_t GarpAt = 0;

    const uint32_t Gen = _ModeGen;
    _SessModeGen = Gen;
    _InSession = true;
    while ((LinkEn() || _RecvHoldF || _NwWanted || CwlOnline_NeedsRadio() || TimeWantsRadio()) && Gen == _ModeGen && !_NetHoldF && !_CartHoldF) {
        now = NowMs();
        if (LinkEn() != LinkWas) {
            LinkWas = LinkEn();
            if (LinkWas) {
                LinkCoreInit(now);   // the link switched on inside a running session
                _LastUseMs = now;
            } else {
                _PortArmed = false;
                RelayStop();         // BYE: the partner sees us leave at once
            }
            FpgaSend(kFpgaCmd_Config, ConfigByte());
        }
        // A radio / ESP-NOW start that failed was never tried again (the power level did
        // not change, so SetPower() had nothing to do). From linkperf4 e81fbcd.
        if ((_Power == kPower_Low || _Power == kPower_Full) && (now - _RadioRetryMs) >= 300 &&
            (!_RadioStarted || (LinkEn() && EspNowOn() && !CwlEn_On()))) {
            _RadioRetryMs = now;
            if (!_RadioStarted) RadioOn(now);
            else EnStart();
            if (_RadioStarted && _Associate) (void)esp_wifi_set_ps(_Power == kPower_Full ? WIFI_PS_NONE : WIFI_PS_MAX_MODEM);
        }
        if (_Sock < 0 && _WifiConnected) {
            _Sock = OpenSocket();
            if (_Sock < 0) ESP_LOGE(TAG, "socket failed");
        }
        if (_ReconnectAt != 0 && (int32_t)(now - _ReconnectAt) >= 0 && PathFresh(kPath_EspNow, now)) {
            _ReconnectAt = now + kApAbsentRetry_ms;   // a join scan would cut the nearby partner off
        }
        if (_ReconnectAt != 0 && (int32_t)(now - _ReconnectAt) >= 0) {
            _ReconnectAt = 0;
            if (_RadioWanted && _RadioStarted && !_WifiConnected) {
                ApplyStaConfig();   // the channel hint may have been cleared
                (void)esp_wifi_connect();
                _ReconnectBackoff = _ReconnectBackoff * 2 > kReconnectMax_ms ? kReconnectMax_ms : _ReconnectBackoff * 2;
            }
        }

        // linkperf1 (A): wait here for the next thing to do instead of in
        // recvfrom(): an FPGA event or an ESP-NOW frame wakes the task at once
        // (CWL_OnFpgaEvent / the ESP-NOW receive callback notify it), and
        // what it makes goes out in this same pass. 0.17.2 waited in
        // recvfrom(1 ms) between taking the FPGA events and LxFlush(), so a
        // packet made from a port event waited up to a tick, and a frame or
        // event that came during the wait waited for it to end. UDP (LAN,
        // relay) is still read every tick (up to 1 ms late, was at once).
        if (uxQueueMessagesWaiting(_Queue) == 0 && uxQueueMessagesWaiting(_LxQueue) == 0) {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(_Power == kPower_Dormant ? 10 : 1));
            now = NowMs();
        }

        // Local link-port events and control messages.
        CtlMsg_t m;
        while (xQueueReceive(_Queue, &m, 0) == pdTRUE) {
            now = NowMs();
            if (m.Kind == kCtl_FpgaEvent) HandleFpgaEvent(&m, now);
            else if (m.Kind == kCtl_SetTimeout) _Cwl.master_timeout_ms = m.U;
            else if (m.Kind == kCtl_SetPartner) {
                if (m.A) CwlLx_PeerFix(&_EnPeer, m.Mac);
                else CwlLx_PeerInit(&_EnPeer, NULL);
                _WasConfirmed = _EnPeer.confirmed;
            }
        }

        // Network.
        for (int Rx = 0; _Sock >= 0 && Rx < 8; Rx++) {
            uint8_t Buf[160];
            struct sockaddr_in From;
            socklen_t FromLen = sizeof(From);
            const int n = recvfrom(_Sock, Buf, sizeof(Buf), MSG_DONTWAIT, (struct sockaddr *)&From, &FromLen);
            if (n < 0) break;
            now = NowMs();
            if (false) {
#if defined(CONFIG_SHINY_RELAY_DEBUG_STATUS_PROBE)
            } else if (n == 4 && memcmp(Buf, "CWS?", 4) == 0) {
                SendStatus(&From);
#endif
            } else if (n >= kSl_Hdr && Buf[0] == 'S' && Buf[1] == 'L') {
                // Shiny Link envelope: the relay, or a punched partner.
                const SlEp_t f = {ntohl(From.sin_addr.s_addr), ntohs(From.sin_port)};
                if (RelayWanted()) (void)Relay_OnDatagram(&_Relay, f, Buf, (size_t)n, now);
            } else if (n == kCwlPacketLen && From.sin_addr.s_addr != _OwnIp && LinkEn() && LanOn() &&
                       Buf[0] == 'C' && Buf[1] == 'W' && Buf[2] == kCwlVersion) {
                _LanPeerMs = now;
                _LanPeerSeen = true;
                if (!_PeerFixed && (!_PeerKnown || _Peer.sin_addr.s_addr != From.sin_addr.s_addr ||
                                    _Peer.sin_port != From.sin_port)) {
                    _Peer = From;
                    _PeerKnown = true;
                    ESP_LOGI(TAG, "peer %s", inet_ntoa(From.sin_addr));
                }
                if (OnPathRx(kPath_Lan, Buf, now)) {
                    const bool Exchange = Buf[3] != kCwlType_Hello && Buf[3] != kCwlType_HelloProbe && Buf[3] != kCwlType_Caps;
                    if (Exchange) Trace('R', Buf[3], Buf[4], Buf[5]);
                    NoteReply(Buf, now);
                    if (Cwl_OnPacket(&_Cwl, Buf, (size_t)n, now) && Exchange) NoteLinkUse(now);   // the peer's game is using the link
                }
            }
        }
        {
            CwlRadioRx_t r;
            while (xQueueReceive(_LxQueue, &r, 0) == pdTRUE) LxReceive(&r, NowMs());
        }
        now = NowMs();
        if (LinkEn() && CwlEn_On()) NearbyService(now);

        now = NowMs();
        if (_WifiConnected && !WasConnected) { GarpLeft = 3; GarpAt = now; }
        WasConnected = _WifiConnected;
        if (GarpLeft > 0 && _WifiConnected && (int32_t)(now - GarpAt) >= 0) {
            (void)tcpip_callback(GratuitousArp, NULL);
            GarpLeft--;
            GarpAt = now + 150;
        }
        RelayService(now);
        if (_RadioStarted && _Associate) CwlNet_AutoJoinPoll(now, _WifiConnected, _ConnFails);
        if (LinkEn()) Cwl_Poll(&_Cwl, now);
        if (LinkEn() && CwlEn_On()) {
            if (_PingLeft > 0 && (now - _PingLastMs) >= kPingEvery_ms && _EnPeer.locked) {
                LxAddPing(false, _PingId++, now);
                _PingLeft--;
                _PingSent++;
                _PingLastMs = now;
                NoteLinkUse(now);
            }
            LxFlush();   // everything this pass queued: one ESP-NOW frame
        }
        const bool Alive = LinkEn() && Cwl_PeerAlive(&_Cwl, now);
        if (Alive && !WasAlive) {
            Trace('W', 4, 0, 0);
            if (_Pwr.WakeTiming && _Pwr.LastPeerMs < 0) _Pwr.LastPeerMs = (int32_t)(now - _Pwr.WakeStartMs);
        }
        WasAlive = Alive;
        if (_Pwr.WakeTiming && (now - _Pwr.WakeStartMs) > 10000) _Pwr.WakeTiming = false;

        SetPower(WantedPower(now), now);
        if (now - LastConfig >= kConfigResend_ms) {
            LastConfig = now;
            FpgaSend(kFpgaCmd_Config, ConfigByte());   // survives an FPGA reset
            // linkperf1: at full power Wi-Fi power save must be off (anything
            // that changed it behind our back, e.g. a radio restart path, is
            // put right within a second and counted in xstat).
            wifi_ps_type_t Ps;
            if (_Power == kPower_Full && _RadioStarted && esp_wifi_get_ps(&Ps) == ESP_OK && Ps != WIFI_PS_NONE) {
                (void)esp_wifi_set_ps(WIFI_PS_NONE);
                _XPsFixes++;
            }
        }
    }

    RelayStop();                     // BYE before the radio goes
    SetPower(kPower_Off, NowMs());   // CONFIG(0), Wi-Fi off
    _InSession = false;
}

static void LinkTask(void *arg)
{
    (void)arg;
    for (;;) {
        // The link runs without an AP in AUTO / NEARBY (ESP-NOW); the relay,
        // pairing and the network window need one.
        const bool LinkCan = LinkEn() && (_Settings.Ssid[0] != '\0' || EspNowOn());
        const bool NetCan = (_NwWanted || _RecvHoldF || CwlOnline_NeedsRadio() || TimeWantsRadio()) && _Settings.Ssid[0] != '\0';
        bool Ended = false;
        if (CwlLogic_LinkMayRun(LinkCan, NetCan, _NetHoldF, _CartHoldF)) {
            LinkSession();
            Ended = true;
        } else {
            // Physical link port (the FPGA may still have the network link
            // on from before an ESP32 reset).
            FpgaSend(kFpgaCmd_Config, 0);
        }
        // Off: wait for CWL_SetEnabled / `cwl on`. Not after a session: what ended it may
        // want the next one now, and its notification is gone (cwl_logic.h).
        if (CwlLogic_TaskMayBlock(Ended)) (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

// ---------------------------------------------------------------- API

uint32_t CWL_SleepTimerMs(void)
{
    if (!_KeepArmed || _Power != kPower_Dormant) return 0;
    const int32_t left = (int32_t)(_KeepDueMs - NowMs());
    return left < 1000 ? 1000 : (uint32_t)left;
}

// 0.17.7: a console sequence that must not be cut by light sleep (cwl cartcycle:
// asleep, the FPGA task pauses and the OSD state is not reported).
static volatile uint32_t _AwakeUntilMs;

bool CWL_KeepAwake(void)
{
    const Power_t p = _Power;
    if (_AwakeUntilMs != 0 && (int32_t)(_AwakeUntilMs - NowMs()) > 0) return true;
    if (_KeepActive) return true;
    if (_NetHoldF) return true;     // cwl_net runs the radio (scan, join, phone setup): no light sleep
    if (!_SleepOk || p == kPower_Low || p == kPower_Full || _WakeReq) return true;
    if (_NwUnlocked) return true;   // a game has the network window open
    // Dormant without the FPGA wake preamble: the first link event after a
    // sleep would be lost, so only Wi-Fi goes off.
    if (p == kPower_Dormant && LinkEn() && !FpgaHasWakePreamble()) return true;
    return false;
}

// ---------------------------------------------------------------- cwl_net hooks (cwl_priv.h)

void CWL_NetHold(bool On)
{
    _NetHoldF = On;
    _ModeGen++;
    if (_Task != NULL) xTaskNotifyGive(_Task);
    if (!On) return;
    for (int i = 0; i < 400 && _InSession; i++) vTaskDelay(pdMS_TO_TICKS(10));
    _Associate = false;   // a STA_START from cwl_net's radio start must not join the old network
}

bool CWL_NetHeld(void) { return _NetHoldF; }
uint8_t CWL_ShareChannel(void) { return _ChannelHint ? _ChannelHint : (_Settings.EnChan >= 1 && _Settings.EnChan <= 13 ? _Settings.EnChan : 1); }

// A cart job needs the heap the radio and its sessions hold (usb-cart-5, unit 2 ONLINE:
// "busy memory"). On: the running session ends the way a mode change ends it (relay BYE,
// radio stopped, Wi-Fi buffers freed) and no new one starts; waits up to wait_ms for it.
// Off: the session the mode wants starts again by itself (Wi-Fi joins, the relay reseats,
// ESP-NOW comes back). Nothing is saved: a restart is "off".
bool CWL_CartHold(bool On, uint32_t wait_ms)
{
    if (On != _CartHoldF) {
        _CartHoldF = On;
        _ModeGen++;
        if (_Task != NULL) xTaskNotifyGive(_Task);
    }
    if (!On) return true;
    for (uint32_t t = 0; _InSession && t < wait_ms; t += 10) vTaskDelay(pdMS_TO_TICKS(10));
    return !_InSession;
}

bool CWL_CartHeld(void) { return _CartHoldF; }

// wifirecv1: receive mode. On: the running session ends (relay BYE, ESP-NOW and the LAN link
// stop, Wi-Fi buffers freed with the radio) and the next one runs with the link off and the
// radio up for the device calls. Off: the session the mode wants starts again. Never blocks.
void CWL_RecvHold(bool On)
{
    if (On == _RecvHoldF) return;
    _RecvHoldF = On;
    _ModeGen++;
    if (_Task != NULL) xTaskNotifyGive(_Task);
}
// The session from before the hold has ended (the running one, if any, started under it).
bool CWL_RecvHeld(void) { return !_InSession || _SessModeGen == _ModeGen; }

esp_err_t CWL_WifiDriverInit(void) { return WifiInit(); }

void CWL_GetCurrentCredentials(char *Ssid, size_t SsidLen, char *Pass, size_t PassLen)
{
    snprintf(Ssid, SsidLen, "%s", _Settings.Ssid);
    snprintf(Pass, PassLen, "%s", _Settings.Pass);
}

void CWL_SetCurrentNetwork(const char *Ssid, const char *Pass)
{
    if (strcmp(Ssid, _Settings.Ssid) != 0) {
        _LeaseValid = false;      // another network: a real DHCP, a full channel scan
        _ChannelHint = 0;
    }
    snprintf(_Settings.Ssid, sizeof(_Settings.Ssid), "%s", Ssid);
    snprintf(_Settings.Pass, sizeof(_Settings.Pass), "%s", Pass);
    SaveSettings();
}

void CWL_SetChannelHint(uint8_t Channel)
{
    if (Channel >= 1 && Channel <= 14) _ChannelHint = Channel;
}

void CWL_RejoinNow(void)
{
    if (!_RadioStarted || !_Associate) return;
    ApplyStaConfig();
    _ConnFails = 0;
    _ReconnectAt = 0;
    (void)esp_wifi_connect();
}

bool CWL_IsEnabled(void) { return _Settings.Mode != kCwlMode_Off; }

void CWL_SetEnabled(bool Enabled)
{
    if (CWL_IsEnabled() == Enabled) return;
    CWL_SetMode(Enabled ? (CwlMode_t)_Settings.LastMode : kCwlMode_Off);
}

CwlMode_t CWL_GetMode(void) { return (CwlMode_t)_Settings.Mode; }

void CWL_SetMode(CwlMode_t Mode)
{
    if (Mode >= kCwlNumMode || Mode == (CwlMode_t)_Settings.Mode) return;
    _Settings.Mode = (uint8_t)Mode;
    if (Mode != kCwlMode_Off) _Settings.LastMode = (uint8_t)Mode;
    _Settings.Enabled = Mode != kCwlMode_Off;
    SaveSettings();
    _Mode = Mode;
    _Enabled = Mode != kCwlMode_Off;
    _ModeGen++;                       // a running session ends; the task starts the new one
    if (_Task != NULL) xTaskNotifyGive(_Task);
}

const char *CWL_ModeText(CwlMode_t Mode)
{
    switch (Mode) {
    case kCwlMode_Auto:   return "AUTO";
    case kCwlMode_Nearby: return "NEARBY";
    case kCwlMode_Online: return "ONLINE";
    case kCwlMode_Off:    return "OFF";
    default:              return "";
    }
}

void CWL_GetDetailText(char *Buf, unsigned Len)
{
    if (Buf == NULL || Len == 0) return;
    Buf[0] = '\0';
    const uint32_t now = NowMs();
    const Path_t p = PathFresh(_Path, now) ? _Path : kPath_None;
    if (p == kPath_EspNow && _EnPeer.locked) {
        const char *n = CandName(_EnPeer.mac);
        char def[16];
        if (n == NULL) { CwlLx_DefaultName(_EnPeer.mac, def, sizeof(def)); n = def; }
        snprintf(Buf, Len, "%s", n);
    } else if (p == kPath_Relay) {
        snprintf(Buf, Len, "VIA  INTERNET");
    } else if (_WifiConnected) {
        CWL_GetIpText(Buf, Len);   // LAN partner or none: our address
    } else if (CwlEn_On()) {
        snprintf(Buf, Len, "NO  ROUTER  CH %u", (unsigned)EnChannelNow());
    }
}

int CWL_GetNearby(CwlNearby_t *Out, int Max)
{
    CwlLxCand_t c[kCwlLx_MaxCand];
    const int n = CwlLx_CandList(&_Cands, NowMs(), c, kCwlLx_MaxCand);
    int k = 0;
    for (int i = 0; i < n && k < Max; i++, k++) {
        memcpy(Out[k].Mac, c[i].mac, 6);
        snprintf(Out[k].Name, sizeof(Out[k].Name), "%s", c[i].name);
        Out[k].Rssi = c[i].rssi;
        Out[k].Seen = true;
    }
    return k;
}

bool CWL_GetPartner(CwlNearby_t *Out)
{
    uint8_t m[6];
    if (_Settings.EnPeer[0] == '\0' || !ParseMac(_Settings.EnPeer, m)) return false;
    memcpy(Out->Mac, m, 6);
    const char *n = CandName(m);
    if (n != NULL) snprintf(Out->Name, sizeof(Out->Name), "%s", n);
    else CwlLx_DefaultName(m, Out->Name, sizeof(Out->Name));
    Out->Seen = false;
    CwlLxCand_t c[kCwlLx_MaxCand];
    const int k = CwlLx_CandList(&_Cands, NowMs(), c, kCwlLx_MaxCand);
    for (int i = 0; i < k; i++) if (memcmp(c[i].mac, m, 6) == 0) { Out->Seen = true; Out->Rssi = c[i].rssi; }
    return true;
}

void CWL_SetPartner(const uint8_t *Mac)
{
    CtlMsg_t m = {.Kind = kCtl_SetPartner, .A = Mac != NULL};
    if (Mac != NULL) {
        MacText(Mac, _Settings.EnPeer, sizeof(_Settings.EnPeer));
        memcpy(m.Mac, Mac, 6);
    } else {
        _Settings.EnPeer[0] = '\0';
    }
    SaveSettings();
    if (_Queue != NULL) (void)xQueueSend(_Queue, &m, 0);
}

bool CWL_GetToast(char *Buf, unsigned Len)
{
    if ((int32_t)(_ToastUntil - NowMs()) <= 0) return false;
    snprintf(Buf, Len, "%s", _Toast);
    return true;
}

CwlStatus_t CWL_GetStatus(void)
{
    if (_Settings.Mode == kCwlMode_Off) return kCwlStatus_Off;
    if (_Settings.Ssid[0] == '\0' && !EspNowOn()) return kCwlStatus_NoWifi;
    const Power_t p = _Power;
    if (p == kPower_Off || p == kPower_Dormant) return kCwlStatus_Asleep;
    if (!_WifiConnected && !CwlEn_On()) return kCwlStatus_Connecting;
    return Cwl_PeerAlive(&_Cwl, NowMs()) ? kCwlStatus_Partner : kCwlStatus_Searching;
}

const char *CWL_StatusText(CwlStatus_t Status)
{
    switch (Status) {
    case kCwlStatus_Off:        return "OFF";
    case kCwlStatus_NoWifi:     return "NO  WI-FI  SET";
    case kCwlStatus_Asleep:     return "STANDBY";
    case kCwlStatus_Connecting: return "CONNECTING";
    case kCwlStatus_Searching:  return "NO  PARTNER";
    case kCwlStatus_Partner:    return "PARTNER  FOUND";
    default:                    return "";
    }
}

void CWL_GetSsidText(char *Buf, unsigned Len)
{
    if (Buf != NULL && Len > 0) snprintf(Buf, Len, "%s", _Settings.Ssid);
}

const char *CWL_WifiStateText(void)
{
    if (_NetHoldF) return "BUSY";
    if (_WifiConnected) return "CONNECTED";
    if (_Settings.Ssid[0] == '\0') return "NO  NETWORK  SET";
    const Power_t p = _Power;
    if (p == kPower_Off) return _InSession ? "OFF" : "NOT  IN  USE";
    if (p == kPower_Dormant) return "STANDBY";
    if (!_Associate) return "NOT  JOINED";
    return _ConnFails >= kApAbsentFails ? "NOT  IN  RANGE" : "JOINING";
}

void CWL_GetIpText(char *Buf, unsigned Len)
{
    if (Buf == NULL || Len == 0) return;
    Buf[0] = '\0';
    if (!_WifiConnected) return;
    const uint32_t ip = _OwnIp;
    const uint8_t *b = (const uint8_t *)&ip;
    snprintf(Buf, Len, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

void CWL_NoteLightSleep(uint32_t Slept_ms, CwlWakeCause_t Cause)
{
    _Pwr.Sleeps++;
    _Pwr.SleptMs += Slept_ms;
    if (Cause == kCwlWake_Fpga) _Pwr.SleepsFpga++;
    else if (Cause == kCwlWake_Console) _Pwr.SleepsConsole++;
}

void CWL_NoteFpgaPacket(uint8_t Addr, uint16_t Data)
{
    _FpgaPackets++;
    _FpgaLastAddr = Addr;
    _FpgaAddrCount[Addr < 16 ? Addr : 16]++;
    if (Addr < 16) _FpgaLast[Addr] = Data;
}

void CWL_NoteFpgaRxBytes(uint32_t Count) { _FpgaRxBytes += Count; }

void CWL_NoteFpgaRxError(bool Crc)
{
    if (Crc) _FpgaRxCrcErrors++;
    else _FpgaRxLenErrors++;
}

void CWL_OnFpgaEvent(uint8_t Event, uint8_t Byte)
{
    _FpgaLinkEvents++;
    if (_Queue == NULL || !LinkEn()) return;
    // Keep the MCU awake from this moment: the link task wakes Wi-Fi.
    if (_Power == kPower_Dormant) _WakeReq = true;
    const CtlMsg_t m = {.Kind = kCtl_FpgaEvent, .A = Event, .B = Byte, .U = NowMs()};
    (void)xQueueSend(_Queue, &m, 0);
    if (_Task != NULL) xTaskNotifyGive(_Task);   // linkperf1: the link task handles it now, not at its next tick
}

uint16_t CWL_FpgaVersionRaw(void) { return _FpgaLast[kStatusAddr_FWVer]; }

void CWL_SetNetworkWanted(bool On)
{
    if (_NwWanted == On) return;
    _NwWanted = On;
    if (!On) _NwUnlocked = false;
    if (_Task != NULL) xTaskNotifyGive(_Task);
}

void CWL_NetworkUse(bool Unlocked, bool Activity)
{
    if (!_NwWanted) return;
    _NwUnlocked = Unlocked;
    if (Activity) {
        const uint32_t now = NowMs();
        _NwUseMs = now;
        _NwUsed = true;
        _LastUseMs = now;            // window use counts as link use
        if (_Power == kPower_Dormant) _WakeReq = true;   // stay awake: the link task brings Wi-Fi up
    }
}

bool CWL_IsOnline(void) { return _WifiConnected && (_Power == kPower_Low || _Power == kPower_Full); }

bool CWL_LinkBusy(void) { return LinkEn() && (_Power == kPower_Full) && Cwl_Busy(&_Cwl); }

void CWL_SetStatusExtra(int (*Fn)(char *Buf, int Len)) { _StatusExtra = Fn; }

void CWL_Wake(void)
{
    if (_Task != NULL) xTaskNotifyGive(_Task);
}

const char *CWL_RelayStatusText(void)
{
    if (!CwlOnline_Paired()) return "NOT  PAIRED";
    if (_Mode == kCwlMode_Nearby) return "NEARBY  ONLY";
    if (!LinkEn()) return "LINK  OFF";
    if (_Settings.Ssid[0] == '\0') return "NO  WI-FI  SET";
    if (_Power == kPower_Dormant || _Power == kPower_Off) return "STANDBY";
    if (!_WifiConnected) return "CONNECTING";
    const uint32_t now = NowMs();
    if (_Mode == kCwlMode_Auto && _LanPeerSeen && (now - _LanPeerMs) < kLanPeerFresh_ms) return "LAN  PARTNER";
    switch (_Relay.state) {
    case kRelay_Idle:
        if (_RelayStarted && _Relay.stats.last_refuse == kSl_RNoRoom) return "NO  SUCH  ROOM";
        if (_RelayStarted && _Relay.stats.last_refuse == kSl_RRoomFull) return "ROOM  FULL";
        return _RelayStarted ? "REFUSED" : "GETTING  TICKET";
    case kRelay_NeedTicket: return "GETTING  TICKET";
    case kRelay_Joining: return "JOINING";
    case kRelay_Seated: break;
    }
    if (!_Relay.peer_present) return "WAITING  PARTNER";
    return Relay_DirectAlive(&_Relay, now) ? "PARTNER  DIRECT" : "PARTNER  RELAY";
}

void CWL_RelayRoomText(char *Buf, unsigned Len)
{
    if (Buf == NULL || Len == 0) return;
    Buf[0] = '\0';
    if (_Relay.state == kRelay_Seated) {
        if (_Relay.peer_present) {
            const uint32_t now = NowMs();
            snprintf(Buf, Len, "ROOM %s %ums", _Relay.code,
                     (unsigned)(Relay_DirectAlive(&_Relay, now) ? _Relay.direct_rtt_ms : _Relay.relay_rtt_ms));
        } else {
            snprintf(Buf, Len, "ROOM %s", _Relay.code);
        }
    }
}

static void PrintRelayStatus(void)
{
    char *on = malloc(900);   // fwshrink1: from the heap for this print (was 900 B of static RAM)
    if (!on) { printf("{ \"error\": \"no memory\" }\n"); return; }
    CwlOnline_StatusJson(on, 900);
    const uint32_t now = NowMs();
    static const char *const kSt[] = {"idle", "need_ticket", "joining", "seated"};
    const RelayStats_t *r = &_Relay.stats;
    printf("{ %s, \"text\": \"%s\", \"state\": \"%s\", \"code\": \"%s\", \"slot\": %u, \"partner\": %d, "
           "\"relay_only\": %d, \"path\": \"%s\", \"relay_rtt_ms\": %u, \"direct_rtt_ms\": %u, \"punching\": %d, "
           "\"gen\": %u, \"lan_peer_age_ms\": %ld, \"tx_relay\": %lu, \"tx_direct\": %lu, \"rx_relay\": %lu, "
           "\"rx_direct\": %lu, \"joins\": %lu, \"welcomes\": %lu, \"refusals\": %lu, \"last_refuse\": \"%s\", "
           "\"rejoins\": %lu, \"direct_ups\": %lu, \"direct_downs\": %lu, \"punches\": %lu, \"pings\": %lu, "
           "\"ping_ms\": %lu, \"delivered\": %lu, \"keeps\": %lu, \"keeps_answered\": %lu, \"keep_armed\": %d, "
           "\"keep_due_s\": %ld, \"free_heap\": %lu, \"min_free_heap\": %lu }\n",
           on, CWL_RelayStatusText(), kSt[_Relay.state % 4], _Relay.code, _Relay.slot, _Relay.peer_present,
           _Relay.peer_relay_only, Relay_DirectAlive(&_Relay, now) ? "direct" : "relay", _Relay.relay_rtt_ms,
           _Relay.direct_rtt_ms, _Relay.punching, _Relay.gen, _LanPeerSeen ? (long)(now - _LanPeerMs) : -1L,
           (unsigned long)r->tx_relay, (unsigned long)r->tx_direct, (unsigned long)r->rx_relay,
           (unsigned long)r->rx_direct, (unsigned long)r->joins_sent, (unsigned long)r->welcomes,
           (unsigned long)r->refusals, r->last_refuse ? Relay_RefuseName(r->last_refuse) : "",
           (unsigned long)r->rejoins, (unsigned long)r->direct_ups, (unsigned long)r->direct_downs,
           (unsigned long)r->punches_sent, (unsigned long)r->pings_sent, (unsigned long)_Relay.relay_ping_ms,
           (unsigned long)_RelayDelivered, (unsigned long)_Keeps, (unsigned long)_KeepsAnswered, _KeepArmed,
           _KeepArmed ? (long)((int32_t)(_KeepDueMs - now) / 1000) : -1L, (unsigned long)esp_get_free_heap_size(),
           (unsigned long)esp_get_minimum_free_heap_size());
    free(on);
}

// ---------------------------------------------------------------- console

static void PrintStatus(void)
{
    const uint32_t now = NowMs();
    char ip[16] = "-";
    if (_WifiConnected) CWL_GetIpText(ip, sizeof(ip));
    char peer[16] = "-";
    if (_PeerKnown) snprintf(peer, sizeof(peer), "%s", inet_ntoa(_Peer.sin_addr));
    uint32_t StateMs[kNumPower];
    for (int i = 0; i < kNumPower; i++) StateMs[i] = _Pwr.StateMs[i] + (_Power == (Power_t)i ? now - _Pwr.StateSinceMs : 0);
    printf("{ \"enabled\": %d, \"power\": \"%s\", \"status\": \"%s\", \"keep_awake\": %d, \"fpga_wake_preamble\": %d, "
           "\"ssid\": \"%s\", \"wifi\": %d, \"ip\": \"%s\", "
           "\"peer\": \"%s\", \"peer_fixed\": %d, \"peer_alive\": %d, \"timeout_ms\": %u, "
           "\"active_s\": %u, \"standby_s\": %u, \"armed_s\": %u, \"idle_ms\": %lu, \"port_armed\": %d, "
           "\"wakes\": %lu, \"held_masters\": %lu, \"wake_assoc_ms\": %ld, \"wake_ip_ms\": %ld, \"wake_peer_ms\": %ld, "
           "\"radio_starts\": %lu, \"static_rejoin\": %d, \"lease_age_s\": %ld, \"sleeps\": %lu, \"slept_ms\": %lu, \"sleeps_fpga\": %lu, \"sleeps_console\": %lu, "
           "\"ms_off\": %lu, \"ms_dormant\": %lu, \"ms_low\": %lu, \"ms_full\": %lu, \"uptime_ms\": %lu, ",
           _Settings.Enabled, kPowerName[_Power], CWL_StatusText(CWL_GetStatus()), CWL_KeepAwake(),
           FpgaHasWakePreamble(), _Settings.Ssid, _WifiConnected, ip, peer, _PeerFixed,
           _Power >= kPower_Low && Cwl_PeerAlive(&_Cwl, now), (unsigned)_Settings.Timeout_ms,
           (unsigned)_Settings.Active_s, (unsigned)_Settings.Standby_s, (unsigned)_Settings.Armed_s,
           (unsigned long)(now - _LastUseMs), _PortArmed,
           (unsigned long)_Pwr.Wakes, (unsigned long)_Cwl.stats.held_masters,
           (long)_Pwr.LastAssocMs, (long)_Pwr.LastIpMs, (long)_Pwr.LastPeerMs,
           (unsigned long)_Pwr.RadioStarts, _StaticRejoin, _LeaseValid ? (long)((now - _LeaseMs) / 1000) : -1L,
           (unsigned long)_Pwr.Sleeps, (unsigned long)_Pwr.SleptMs,
           (unsigned long)_Pwr.SleepsFpga, (unsigned long)_Pwr.SleepsConsole,
           (unsigned long)StateMs[kPower_Off], (unsigned long)StateMs[kPower_Dormant],
           (unsigned long)StateMs[kPower_Low], (unsigned long)StateMs[kPower_Full], (unsigned long)now);
    printf("\"master_done\": %lu, \"master_timeouts\": %lu, \"slave_done\": %lu, \"slave_refused\": %lu, \"cancels\": %lu, "
           "\"rx_packets\": %lu, \"rx_bad\": %lu, \"max_master_wait_ms\": %lu, "
           "\"master_restarts\": %lu, \"offer_probes\": %lu, \"peer_resets\": %lu, \"probes_answered\": %lu, "
           "\"master_abandons\": %lu, \"unknown_abandons\": %lu, \"master_collisions\": %lu, \"standing\": %d, "
           "\"fpga_pkts\": %lu, \"fpga_events\": %lu, \"uart_bytes\": %lu, \"crc_err\": %lu, \"len_err\": %lu }\n",
           (unsigned long)_Cwl.stats.master_done, (unsigned long)_Cwl.stats.master_timeouts,
           (unsigned long)_Cwl.stats.slave_done, (unsigned long)_Cwl.stats.slave_offers_refused,
           (unsigned long)_Cwl.stats.cancels,
           (unsigned long)_Cwl.stats.rx_packets, (unsigned long)_Cwl.stats.rx_bad,
           (unsigned long)_Cwl.stats.max_master_wait_ms,
           (unsigned long)_Cwl.stats.master_restarts, (unsigned long)_Cwl.stats.offer_probes,
           (unsigned long)_Cwl.stats.peer_resets, (unsigned long)_Cwl.stats.probes_answered,
           (unsigned long)_Cwl.stats.master_abandons, (unsigned long)_Cwl.stats.unknown_abandons,
           (unsigned long)_Cwl.stats.master_collisions, _Cwl.standing ? 1 : 0,
           (unsigned long)_FpgaPackets, (unsigned long)_FpgaLinkEvents, (unsigned long)_FpgaRxBytes,
           (unsigned long)_FpgaRxCrcErrors, (unsigned long)_FpgaRxLenErrors);
}

// A MENU tap (opens or closes the OSD on the release).
static void TapMenu(void)
{
    Button_Poke(kButtonPoke_Menu);
    vTaskDelay(pdMS_TO_TICKS(120));
    Button_Poke(0);
}

// Wait until the OSD is (not) shown, as the FPGA's button reports say. 0.17.6
// read it once after 400 ms and was wrong 5 of 7 times when the OSD started open.
static bool WaitOsd(bool Open, uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 20) {
        if (OSD_IsVisible() == Open) return true;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return OSD_IsVisible() == Open;
}

static int CmdCwl(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "status";
    if (strcmp(sub, "status") == 0) {
        PrintStatus();
    } else if (strcmp(sub, "wifi") == 0 && argc >= 3) {
        snprintf(_Settings.Ssid, sizeof(_Settings.Ssid), "%s", argv[2]);
        snprintf(_Settings.Pass, sizeof(_Settings.Pass), "%s", argc >= 4 ? argv[3] : "");
        SaveSettings();
        printf("cwl: wifi saved (%s), applies on next start\n", _Settings.Ssid);
    } else if (strcmp(sub, "peer") == 0 && argc >= 3) {
        snprintf(_Settings.Peer, sizeof(_Settings.Peer), "%s", strcmp(argv[2], "auto") == 0 ? "" : argv[2]);
        SaveSettings();
        printf("cwl: peer %s (applies on next start)\n", _Settings.Peer[0] ? _Settings.Peer : "auto");
    } else if (strcmp(sub, "timeout") == 0 && argc >= 3) {
        _Settings.Timeout_ms = (uint16_t)atoi(argv[2]);
        SaveSettings();
        const CtlMsg_t m = {.Kind = kCtl_SetTimeout, .U = _Settings.Timeout_ms};
        if (_Queue) (void)xQueueSend(_Queue, &m, 0);
        printf("cwl: master timeout %u ms\n", (unsigned)_Settings.Timeout_ms);
    } else if (strcmp(sub, "power") == 0) {
        if (argc >= 5) {
            _Settings.Active_s = (uint16_t)atoi(argv[2]);
            _Settings.Standby_s = (uint16_t)atoi(argv[3]);
            _Settings.Armed_s = (uint16_t)atoi(argv[4]);
            SaveSettings();
        }
        printf("cwl: full power %u s after link use, then modem sleep to %u s (%u s while armed), then Wi-Fi off\n",
               (unsigned)_Settings.Active_s, (unsigned)_Settings.Standby_s, (unsigned)_Settings.Armed_s);
    } else if (strcmp(sub, "net") == 0) {
        return CwlNet_Console(argc - 1, argv + 1) ? 1 : 0;
    } else if (strcmp(sub, "mode") == 0) {
        if (argc >= 3) {
            CwlMode_t m = kCwlNumMode;
            for (int i = 0; i < kCwlNumMode; i++) {
                char lower[12];
                const char *t = CWL_ModeText((CwlMode_t)i);
                size_t k = 0;
                for (; t[k] && k < sizeof(lower) - 1; k++) lower[k] = (char)(t[k] >= 'A' && t[k] <= 'Z' ? t[k] + 32 : t[k]);
                lower[k] = 0;
                if (strcmp(argv[2], lower) == 0) m = (CwlMode_t)i;
            }
            if (m == kCwlNumMode) { printf("cwl mode auto|nearby|online|off\n"); return 1; }
            CWL_SetMode(m);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        printf("cwl: mode %s\n", CWL_ModeText(_Mode));
    } else if (strcmp(sub, "name") == 0) {
        if (argc >= 3) {
            char n[16] = "";
            size_t k = 0;
            for (int a = 2; a < argc && k < sizeof(n) - 1; a++) {
                if (a > 2 && k < sizeof(n) - 1) n[k++] = ' ';
                for (const char *c = argv[a]; *c && k < sizeof(n) - 1; c++) {
                    char ch = *c >= 'a' && *c <= 'z' ? (char)(*c - 32) : *c;
                    if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-') n[k++] = ch;
                }
            }
            n[k] = 0;
            snprintf(_Settings.Name, sizeof(_Settings.Name), "%s", strcmp(argv[2], "default") == 0 ? "" : n);
            SaveSettings();
            if (_Settings.Name[0] != '\0') snprintf(_OwnName, sizeof(_OwnName), "%s", _Settings.Name);
            else CwlLx_DefaultName(_OwnMac, _OwnName, sizeof(_OwnName));
        }
        char m[18];
        MacText(_OwnMac, m, sizeof(m));
        printf("cwl: name \"%s\" (esp-now %s)\n", _OwnName, m);
    } else if (strcmp(sub, "nearby") == 0) {
        CwlNearby_t l[kCwlLx_MaxCand];
        const int n = CWL_GetNearby(l, kCwlLx_MaxCand);
        for (int i = 0; i < n; i++) {
            char m[18];
            MacText(l[i].Mac, m, sizeof(m));
            printf("  %s  %-15s rssi %d%s\n", m, l[i].Name, l[i].Rssi,
                   (_EnPeer.locked && memcmp(_EnPeer.mac, l[i].Mac, 6) == 0) ? (_EnPeer.confirmed ? "  partner" : "  locking") : "");
        }
        printf("cwl: %d nearby\n", n);
    } else if (strcmp(sub, "partner") == 0) {
        if (argc >= 3) {
            uint8_t m[6];
            if (strcmp(argv[2], "forget") == 0 || strcmp(argv[2], "auto") == 0) CWL_SetPartner(NULL);
            else if (ParseMac(argv[2], m)) CWL_SetPartner(m);
            else { printf("cwl partner forget|<AA:BB:CC:DD:EE:FF>\n"); return 1; }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        CwlNearby_t pt;
        if (CWL_GetPartner(&pt)) printf("cwl: partner %s (%s)%s\n", _Settings.EnPeer, pt.Name, pt.Seen ? " nearby" : "");
        else printf("cwl: no partner (pairs with the next side that answers)\n");
    } else if (strcmp(sub, "espnow") == 0) {
        const char *e = argc >= 3 ? argv[2] : "status";
        if (strcmp(e, "apjoin") == 0 && argc >= 4) {
            _NoApTest = strcmp(argv[3], "off") == 0;
            printf("cwl: AP join %s (test knob, not saved; the session restarts)\n", _NoApTest ? "off" : "on");
            _ModeGen++;
            if (_Task != NULL) xTaskNotifyGive(_Task);
        } else if (strcmp(e, "channel") == 0 && argc >= 4) {
            const int ch = atoi(argv[3]);
            if (ch < 1 || ch > 13) { printf("channel 1..13\n"); return 1; }
            _Settings.EnChan = (uint8_t)ch;
            SaveSettings();
            printf("cwl: esp-now home channel %d (without an AP; applies on the next radio start)\n", ch);
        } else if (strcmp(e, "ping") == 0) {
            const int count = argc >= 4 ? atoi(argv[3]) : 20;
            memset(&_PingRtt, 0, sizeof(_PingRtt));
            _PingSent = _PingGot = 0;
            _PingLeft = count;
            for (int i = 0; i < count * 3 + 40 && (_PingLeft > 0 || _PingGot < _PingSent); i++) vTaskDelay(pdMS_TO_TICKS(25));
            printf("cwl: esp-now ping sent %lu got %lu  rtt p50 %u p90 %u max %lu ms\n", (unsigned long)_PingSent,
                   (unsigned long)_PingGot, (unsigned)CwlLx_RttPct(&_PingRtt, 50), (unsigned)CwlLx_RttPct(&_PingRtt, 90),
                   (unsigned long)_PingRtt.max_ms);
            _PingLeft = 0;
        } else {
            CwlEnStatus_t st;
            CwlEn_GetStatus(&st);
            char own[18], pm[18] = "-";
            MacText(st.own, own, sizeof(own));
            if (_EnPeer.locked) MacText(_EnPeer.mac, pm, sizeof(pm));
            printf("{ \"on\": %d, \"own\": \"%s\", \"name\": \"%s\", \"channel\": %u, \"home\": %u, \"low_power\": %d, "
                   "\"peer\": \"%s\", \"locked\": %d, \"confirmed\": %d, \"fixed\": %d, \"path\": \"%s\", "
                   "\"tx\": %lu, \"tx_ok\": %lu, \"tx_fail\": %lu, \"tx_err\": %lu, \"rx\": %lu, \"rx_drop\": %lu, "
                   "\"lx_rx\": %lu, \"lx_bad\": %lu, \"foreign\": %lu, \"locks\": %lu, \"snubs\": %lu, \"switches\": %lu, "
                   "\"losses\": %lu, \"pairings\": %lu, \"sweeps\": %lu, \"ap_fails\": %u, \"path_switches\": %lu, "
                   "\"path_filtered\": %lu }\n",
                   st.on, own, _OwnName, (unsigned)st.channel, (unsigned)_EnChan, st.low_power, pm, _EnPeer.locked,
                   _EnPeer.confirmed, _EnPeer.fixed, kPathName[_Path], (unsigned long)st.tx_frames,
                   (unsigned long)st.tx_ok, (unsigned long)st.tx_fail, (unsigned long)st.tx_err, (unsigned long)st.rx_frames,
                   (unsigned long)st.rx_drop, (unsigned long)_LxRxFrames, (unsigned long)_LxBad, (unsigned long)_LxForeign,
                   (unsigned long)_EnPeer.locks, (unsigned long)_EnPeer.snubs, (unsigned long)_EnPeer.switches,
                   (unsigned long)_EnPeer.losses, (unsigned long)_Pairings, (unsigned long)_Sweeps, (unsigned)_ConnFails,
                   (unsigned long)_PathSwitches, (unsigned long)_PathFiltered);
        }
    } else if (strcmp(sub, "heap") == 0) {
        printf("{ \"free\": %u, \"min_free\": %u, \"largest\": %u, \"internal_free\": %u, \"dma_free\": %u }\n",
               (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
    } else if (strcmp(sub, "xstat") == 0) {
        // linkperf1: where the link costs a game frames (see PROTOCOL.md "Measuring")
        const uint32_t now = NowMs();
        if (argc >= 3 && strcmp(argv[2], "reset") == 0) {
            Cwl_XstatReset(&_Cwl, now);
            for (int i = 0; i < kNumPower; i++) _XPwrMs0[i] = _Pwr.StateMs[i] + (_Power == i ? now - _Pwr.StateSinceMs : 0);
            _XPwrSince0 = now;
            _XNotFull = _XPsFixes = 0;
            memset(&_XRtt, 0, sizeof(_XRtt));
            printf("cwl: xstat reset\n");
        } else {
            char *Buf = malloc(1400);   // fwshrink1: the console's, from the heap for this print (was 1.4 KB static RAM)
            if (Buf) { (void)Cwl_XstatFormat(&_Cwl, Buf, 1400, now); fputs(Buf, stdout); free(Buf); }
            else printf("cwl: xstat: no memory\n");
            uint32_t Ms[kNumPower];
            for (int i = 0; i < kNumPower; i++) Ms[i] = _Pwr.StateMs[i] + (_Power == i ? now - _Pwr.StateSinceMs : 0) - _XPwrMs0[i];
            CwlEnStatus_t En;
            CwlEn_GetStatus(&En);
            printf("power now %s; since reset full %lu low %lu dormant %lu ms; REQ/ACK sent below full %lu; ps fixes %lu; "
                   "path %s; espnow tx %lu fail %lu rx %lu drop %lu\n",
                   kPowerName[_Power], (unsigned long)Ms[kPower_Full], (unsigned long)Ms[kPower_Low],
                   (unsigned long)Ms[kPower_Dormant], (unsigned long)_XNotFull, (unsigned long)_XPsFixes, kPathName[_Path],
                   (unsigned long)En.tx_frames, (unsigned long)En.tx_fail, (unsigned long)En.rx_frames, (unsigned long)En.rx_drop);
        }
    } else if (strcmp(sub, "xrtt") == 0) {
        printf("cwl: exchange rtt p50 %u p90 %u max %lu ms over %lu (path %s)\n", (unsigned)CwlLx_RttPct(&_XRtt, 50),
               (unsigned)CwlLx_RttPct(&_XRtt, 90), (unsigned long)_XRtt.max_ms, (unsigned long)_XRtt.n, kPathName[_Path]);
        if (argc >= 3 && strcmp(argv[2], "reset") == 0) memset(&_XRtt, 0, sizeof(_XRtt));
    } else if (strcmp(sub, "on") == 0) {
        CWL_SetEnabled(true);
        vTaskDelay(pdMS_TO_TICKS(50));
        PrintStatus();
    } else if (strcmp(sub, "off") == 0) {
        CWL_SetEnabled(false);
        vTaskDelay(pdMS_TO_TICKS(100));
        PrintStatus();
    } else if (strcmp(sub, "trace") == 0) {
        // Dump and clear the event ring: ms kind a b c (see Trace above)
        const uint32_t n = _TraceN, first = n > kTraceLen ? n - kTraceLen : 0;
        _TraceOn = false;
        for (uint32_t i = first; i < n; i++) {
            const TraceEnt_t *t = &_Trace[i % kTraceLen];
            printf("T %lu %c %u %02x %u\n", (unsigned long)t->Ms, t->Kind, t->A, t->B, t->C);
            if ((i & 31) == 31) vTaskDelay(pdMS_TO_TICKS(5));
        }
        printf("T end %lu\n", (unsigned long)(n - first));
        _TraceN = 0;
        _TraceOn = true;
    } else if (strcmp(sub, "cartcycle") == 0) {
        // Debug: power the cartridge off for ~1 s (FPGA 18.18+), then reset
        // the Game Boy, so a flash cart (EZ-Flash Junior) boots its menu. A
        // soft reset or an FPGA reload keeps the cart powered (game replays).
        // No light sleep until the sequence is over (12 s at most).
        _AwakeUntilMs = (NowMs() + 12000) | 1;
        FpgaSend(kFpgaCmd_CartPowerCycle, 0);
        // Off ~1 s, then the EZ-Flash Junior needs a few seconds before a
        // reset boots its menu (a reset at 1.5 s left a white screen).
        vTaskDelay(pdMS_TO_TICKS(4000));
        // The reset combo only works with the OSD closed, and releasing its
        // MENU bit toggles the OSD (button_poke.h): close it before, and again
        // after the release opened it.
        if (OSD_IsVisible()) {
            TapMenu();
            (void)WaitOsd(false, 1500);
        }
        Button_Poke(kButtonPoke_ResetCombo);
        vTaskDelay(pdMS_TO_TICKS(150));
        Button_Poke(0);
        (void)WaitOsd(true, 1500);
        for (int i = 0; i < 2 && OSD_IsVisible(); i++) {
            TapMenu();
            (void)WaitOsd(false, 1500);
        }
        _AwakeUntilMs = 0;
        printf("cartridge power cycled, GB reset; OSD %s\n", OSD_IsVisible() ? "STILL OPEN" : "closed");
    } else if (strcmp(sub, "relay") == 0) {
        const int r = CwlOnline_Console(argc - 1, argv + 1);
        if (r < 0) {
            PrintRelayStatus();
        } else {
            CWL_Wake();   // pairing may need the radio now
        }
        return r > 0 ? 1 : 0;
    } else if (strcmp(sub, "event") == 0 && argc >= 4) {
        // Inject a link-port event as if the FPGA had sent it (bench testing).
        CWL_OnFpgaEvent((uint8_t)atoi(argv[2]), (uint8_t)strtol(argv[3], NULL, 0));
    } else {
        printf("usage: cwl [status | mode auto|nearby|online|off | on | off | wifi <ssid> [pass] | peer <ip|auto> |\n"
               "            nearby | partner [forget|<mac>] | name <text>|default |\n"
               "            espnow [status|channel <1-13>|ping [n]|apjoin on|off] |\n"
               "            heap | xrtt [reset] | xstat [reset] | net ... (saved networks, scan, phone setup) | timeout <ms> |\n"
               "            power [<active_s> <standby_s> <armed_s>] | trace | event <1..7> <byte> | cartcycle |\n"
               "            relay [status | pair | cancel | unpair | mode auto|lan|relay | room app|mine|host|join <code> |\n"
               "                   force on|off | api <url>|default | relayhost <host:port>|default | ticket]]\n");
        return 1;
    }
    return 0;
}

void CWL_Initialize(void)
{
    _Queue = xQueueCreate(kEventQueueLen, sizeof(CtlMsg_t));
    _LxQueue = xQueueCreate(kLxQueueLen, sizeof(CwlRadioRx_t));
    LoadSettings();
    (void)esp_read_mac(_OwnMac, ESP_MAC_WIFI_STA);   // the ESP-NOW source address
    if (_Settings.Name[0] != '\0') snprintf(_OwnName, sizeof(_OwnName), "%s", _Settings.Name);
    else CwlLx_DefaultName(_OwnMac, _OwnName, sizeof(_OwnName));
    CwlOnline_Init();
    // 0.17.0: the retired ONLINE "via" setting becomes the link cable mode once
    // (lan -> NEARBY, relay -> ONLINE; only over AUTO, an explicit mode wins).
    const uint8_t Via = CwlOnline_TakeLegacyVia();
    if (Via != 0 && _Settings.Mode == kCwlMode_Auto) {
        _Settings.Mode = _Settings.LastMode = (Via == 1) ? kCwlMode_Nearby : kCwlMode_Online;
        SaveSettings();
        ESP_LOGI(TAG, "via %s -> mode %s", Via == 1 ? "lan" : "relay", CWL_ModeText((CwlMode_t)_Settings.Mode));
    }
    CwlNet_Init();
    _Pwr.StateSinceMs = NowMs();

    const esp_console_cmd_t cmd = {
        .command = "cwl",
        .help = "Link cable: cwl [status|mode auto|nearby|online|off|on|off|wifi <ssid> [pass]|peer <ip|auto>|nearby|partner|name|espnow ...|timeout <ms>|power ...|trace|event|heap]",
        .hint = NULL,
        .func = &CmdCwl,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

void CWL_AutoStart(void)
{
    _Mode = (CwlMode_t)_Settings.Mode;
    _Enabled = _Mode != kCwlMode_Off;
    xTaskCreatePinnedToCore(LinkTask, "cwl", kTaskStack, NULL, kTaskPriority, &_Task, 0);
    CwlEn_SetNotifyTask(_Task);   // an ESP-NOW frame wakes the link task at once
    CwlOnline_Start();
}
