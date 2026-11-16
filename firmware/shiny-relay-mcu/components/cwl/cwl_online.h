// cwl_online.h: Shiny Link account side of the Chromatic (ESP32): pairing with
// a Shiny Gen account (device-code flow, scope "link"), relay tickets,
// host-code reports and unpairing, all over HTTPS to api.shinyrelay.com
// (shiny_gen_3 notes/CHROMATIC_RELAY.md §4, §5, §10). The HTTPS work runs in
// its own task; the link task (cwl_esp.c) only takes finished tickets.
//
// Secrets: the device token lives in NVS namespace "cwlrel" (plain NVS: this
// unit has no flash encryption, and the ESP32's NVS encryption needs it or an
// HMAC eFuse key the original ESP32 lacks). It is never printed or logged.
#pragma once

#include "cwl_relay.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Which room the relay seat is in (NVS "room").
typedef enum {
    kCwlRoom_App,      // what the Shiny Gen app set (default: mine)
    kCwlRoom_Mine,     // this account's other device, no code
    kCwlRoom_Host,     // a new room; its code is shown and reported
    kCwlRoom_Join,     // a friend's room by code (NVS "code")
    kCwlNumRoom,
} CwlRoom_t;

typedef enum {
    kCwlPair_Idle,
    kCwlPair_Starting,   // asking the backend for a code
    kCwlPair_Waiting,    // showing the code, polling
    kCwlPair_Done,       // approved: token stored
    kCwlPair_Failed,     // see CwlOnline_PairError
} CwlPairState_t;

// A ticket the link task may JOIN with.
typedef struct {
    char ticket[kSl_MaxTicket + 1];
    size_t len;
    SlEp_t relay;
    uint8_t mode;              // kSl_Mode*
    char code[kSl_CodeLen + 1];
    uint64_t client_id;
    uint32_t fetched_ms;
} CwlTicket_t;

void CwlOnline_Init(void);       // after NVS; loads settings
void CwlOnline_Start(void);      // starts the HTTPS task

bool CwlOnline_Paired(void);
// The ONE device token (the pairing's), for the network window's gateway
// calls (nw_dev.c): copied into out (NUL-terminated), returns its length, 0
// when not paired or out is too small. The caller wipes its copy after use.
size_t CwlOnline_CopyToken(char *out, size_t cap);
// The API host the pairing talks to ("https://api.shinyrelay.com" by default).
const char *CwlOnline_Api(void);
// This device's id (16 hex) from the pairing, "" when not paired.
const char *CwlOnline_DeviceId(void);
// The app's last room instruction, from the last ticket: 'm' mine, 'h' host,
// 'j' join (arg = code), 'r' room (arg = game room id); 0 = none seen yet.
char CwlOnline_Instruction(char *arg, size_t cap, uint32_t *age_ms);
// 0.17.0 retired the ONLINE "via auto/lan/relay" setting (the LINK CABLE mode
// AUTO / NEARBY / ONLINE says it). The old NVS value, once: 0 none/auto,
// 1 lan, 2 relay; the key is erased by this call.
uint8_t CwlOnline_TakeLegacyVia(void);
// Hidden console option (`cwl relay force on|off`): relay only, no P2P punch.
bool CwlOnline_ForceRelay(void);
void CwlOnline_SetForceRelay(bool on);
CwlRoom_t CwlOnline_Room(void);
void CwlOnline_SetRoom(CwlRoom_t r, const char *code);   // code for kCwlRoom_Join
const char *CwlOnline_JoinCode(void);
// Bumped by every setting that needs the relay session restarted.
uint32_t CwlOnline_Generation(void);

// Tickets. Want: fetch one in the background (no-op while one is fresh or a
// fetch runs). Take: hand over a fresh ticket (single use) -> true.
void CwlOnline_WantTicket(void);
bool CwlOnline_HaveTicket(uint32_t now_ms);
bool CwlOnline_TakeTicket(CwlTicket_t *out, uint32_t now_ms);
// The relay seated us as host with this code: tell the backend (the app
// shows it), once per code.
void CwlOnline_ReportCode(const char *code);

// Pairing (OSD SYSTEM > ONLINE, console `cwl relay pair`).
void CwlOnline_PairStart(void);
void CwlOnline_PairCancel(void);
void CwlOnline_Unpair(void);
CwlPairState_t CwlOnline_PairState(void);
const char *CwlOnline_PairError(void);
// "ABCD-EFGH" and the approval URL with the code (QR) while waiting.
void CwlOnline_PairCode(char *code, size_t code_len, char *url, size_t url_len);
// Pairing or an HTTPS call is waiting for Wi-Fi: keep the radio up.
bool CwlOnline_NeedsRadio(void);

// 0.17.7, public party matches (PARTY_ROOMS.md 8.4): the account's answer from
// the last ticket (-1 not known, 0 not given, 1 given); the player's answer to
// POST once (false: not paired, or one is on its way); its HTTP status when
// done (-1 no network), taken once; and drop the spare ticket (the next join
// buys a new one, which carries the answer).
int  CwlOnline_PartyConsent(void);
bool CwlOnline_PostPartyConsent(bool Generic);
// One GET (Body ignored) or POST JSON to the device API with the device token, made on the
// link task (lane cartflash1). Out (Cap bytes) must stay valid until the result is taken.
// Start: false if not paired or one is running. Result: 0 while running (or none), then the
// HTTP status once (-1 = no connection; 401 = not paired).
bool CwlOnline_DeviceCallStart(bool Get, const char *Path, const char *Body, char *Out, int Cap);
bool CwlOnline_DeviceCallBusy(void);
// wifirecv1 (receive mode): the same one call, with the unit's REGISTRATION token instead of the
// pairing's (no pairing needed). Path "/link_device/register" goes with no token; the host keeps
// the token from its answer (NVS) and Out gets "{}". The result: CwlOnline_DeviceCallResult.
bool CwlOnline_RecvCallStart(const char *Path, const char *Body, char *Out, int Cap);
bool CwlOnline_Registered(void);
void CwlOnline_RecvForget(void);   // the server refused the registration token: drop it
int CwlOnline_DeviceCallResult(void);
bool CwlOnline_PartyConsentResult(int *pHttp);
void CwlOnline_DropTicket(void);
// Last HTTPS result, for status ("" before any).
void CwlOnline_StatusJson(char *buf, size_t len);

// Console: `cwl relay ...` (called from cwl_esp.c's `cwl` command).
int CwlOnline_Console(int argc, char **argv);

// True while something on the device must not start TLS (the cart-flash bench jobs: the
// handshake needs ~30 KB of heap that a cartflash build does not have beside a running
// SHLINK game). Weak default: false; components/nw/cartflash/cf_dev.c overrides it.
bool CwlOnline_TlsHold(void);
// A cart job: the HTTPS call in flight ends (up to wait_ms), the kept client is freed.
bool CwlOnline_Quiet(uint32_t wait_ms);

#ifdef __cplusplus
}

#endif
