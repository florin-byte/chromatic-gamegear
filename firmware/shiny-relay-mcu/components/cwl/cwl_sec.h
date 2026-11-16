// Security rules shared by the link, the pairing and the network window
// (0.17.1, lane fwfix1). Pure C, no ESP-IDF: tools/host builds test it.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- API / gateway URLs
// Every request to the device API or the network-window gateway carries a
// token (the pairing's device token, a pairing poll's answer, a ticket), so a
// release build talks to them over https:// only. AllowHttp is true only in a
// debug build (CONFIG_SHINY_RELAY_DEBUG_ALLOW_HTTP_API, for a LAN mock).
// "https://" + at least one host character; lower-case scheme only.
bool CwlSec_UrlAllowed(const char *Url, bool AllowHttp);
// The same, with this build's setting.
bool CwlSec_ApiUrlOk(const char *Url);
// True when this build accepts http:// API URLs (a debug build).
bool CwlSec_HttpAllowed(void);

// ---- the network-window gateway's base URL (0.17.2: one rule, host-tested)
// Override: NVS "nw/url" ("" = none); Api: the paired API host. Writes the
// base the gateway calls use into Out: Override when set, else Api +
// "/gbnet/v1" (https://api.shinyrelay.com/gbnet/v1 by default). Returns true
// when that base may carry the device token (CwlSec_UrlAllowed); false
// (Out still written, for status output) when it may not or does not fit.
bool CwlSec_GatewayBase(const char *Override, const char *Api, char *Out, size_t Cap, bool AllowHttp);

// ---- which relay the tickets are used on (0.17.5, lane fwfix2)
// A ticket names its relay ("relay": "host:port"), but the beta API's
// tickets named the PROD relay (its auth service had no SHINY_LINK_RELAY_UDP),
// so after `cwl relay api https://betaapi...` a beta room id was asked of the
// prod relay ("no_room"). The known stacks pair an API host with its relay:
//   api.shinyrelay.com      -> relay.shinyrelay.com:5861
//   betaapi.shinyrelay.com  -> betarelay.shinyrelay.com:6861
// Writes the relay to use into Out: for a known API stack, its own relay
// when the ticket names none or names ANOTHER known stack's relay, else the
// ticket's (a new host the backend chose for this stack); for an unknown API
// (a LAN mock) the ticket's. Returns true when Out differs from the ticket's.
// (The console override `cwl relay relayhost` is applied by the caller first.)
bool CwlSec_RelayFor(const char *Api, const char *TicketRelay, char *Out, size_t Cap);
// The known stack's relay for an API URL, or NULL (an unknown API).
const char *CwlSec_StackRelay(const char *Api);

// ---- phone setup network password
// 15 random characters in three groups of five, "XXXXX-XXXXX-XXXXX" (17 with
// the dashes): 15 x log2(18) = 62.5 bits. The alphabet is the upper-case
// letters and digits that the OSD's 5 px font (fingfai) draws unlike any
// other and 4 px wide (M and W are 6 px): no 0/O/D/Q, 1/I, 2/Z, 5/S, 8/B,
// U/V, 6/G pairs, no Y (reads as 4). Upper case: one shift-lock to type it.
#define CWLSEC_SETUP_ALPHABET "ACEFGHJKLNPRTX3479"
enum {
    kCwlSec_SetupAlphabetLen = sizeof(CWLSEC_SETUP_ALPHABET) - 1,   // 18
    kCwlSec_SetupGroups = 3,
    kCwlSec_SetupGroupLen = 5,
    kCwlSec_SetupRandomChars = kCwlSec_SetupGroups * kCwlSec_SetupGroupLen,              // 15
    kCwlSec_SetupPassLen = kCwlSec_SetupRandomChars + kCwlSec_SetupGroups - 1,           // 17
};
// Out gets kCwlSec_SetupPassLen characters + NUL (Cap >= 18). Rnd returns 32
// uniform random bits (esp_random on the device); rejection sampling keeps
// every character uniform. Returns false (Out = "") when Cap is too small.
bool CwlSec_MakeSetupPass(char *Out, size_t Cap, uint32_t (*Rnd)(void));

#ifdef __cplusplus
}
#endif
