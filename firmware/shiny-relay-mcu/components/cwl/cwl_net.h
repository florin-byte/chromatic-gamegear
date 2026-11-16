// cwl_net.h: which Wi-Fi network the Chromatic joins (ESP 0.16+). Saved
// networks (up to 5, newest first, NVS "cwlnet"), scans, joins with plain-word
// failure reasons, auto-join of the best known network, and phone setup (a
// temporary "ShinyRelay-XXXX" network with a tiny web page).
//
// Passwords stay in NVS and in this firmware: no function returns one, and
// nothing prints one (console, CWS?, web page).
//
// Scans, joins and phone setup take the Wi-Fi radio for themselves: the link
// session pauses meanwhile (CWL_NetHold) and resumes on the joined network.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cwl_sec.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    kCwlNet_MaxSaved = 5,
    kCwlNet_MaxScan = 12,
    kCwlNet_SsidMax = 32,
    kCwlNet_PassMax = 64,
    kCwlNet_PortalTimeout_s = 300,
};

typedef struct {
    char Ssid[kCwlNet_SsidMax + 1];
    int8_t Rssi;
    bool Secure;          // needs a password
    bool Saved;           // in the saved list
    bool Current;         // the network the link uses now
} CwlNetAp_t;

typedef enum {
    kCwlNetOp_Idle,
    kCwlNetOp_Scanning,
    kCwlNetOp_Joining,
    kCwlNetOp_Portal,
} CwlNetOp_t;

typedef enum {
    kCwlNetRes_None,
    kCwlNetRes_Ok,            // joined (and saved)
    kCwlNetRes_WrongPassword,
    kCwlNetRes_NotFound,
    kCwlNetRes_Refused,       // the access point refused the connection
    kCwlNetRes_NoAddress,     // joined, but no IP address (DHCP)
    kCwlNetRes_Timeout,
    kCwlNetRes_Failed,        // anything else
} CwlNetResult_t;

void CwlNet_Init(void);           // after CWL settings are loaded: migrates the single network

// ---- saved networks (SSIDs only leave this module)
int CwlNet_SavedCount(void);
bool CwlNet_SavedSsid(int Index, char *Out, unsigned Len);   // 0 = newest
void CwlNet_Forget(int Index);
bool CwlNet_IsSaved(const char *Ssid);

// ---- operations (asynchronous; poll CwlNet_Op / CwlNet_LastResult)
CwlNetOp_t CwlNet_Op(void);
bool CwlNet_ScanStart(void);
int CwlNet_ScanResults(CwlNetAp_t *Out, int Max);   // strongest first, one per SSID
// The same, without a copy (valid until the next scan finishes).
const CwlNetAp_t *CwlNet_ScanList(int *Count);
uint32_t CwlNet_ScanSeq(void);                     // bumps when a scan finishes
// Join Ssid with Pass (NULL: the saved password). Saved (newest first) and
// made current only when it worked; otherwise the old network stays current.
bool CwlNet_Join(const char *Ssid, const char *Pass);
CwlNetResult_t CwlNet_LastResult(void);
const char *CwlNet_ResultText(CwlNetResult_t Result);   // "CONNECTED", "WRONG  PASSWORD", ...
const char *CwlNet_LastSsid(void);                       // what the last join tried

// ---- phone setup (captive portal)
bool CwlNet_PortalStart(void);
void CwlNet_PortalStop(void);
typedef struct {
    bool Active;
    char Ssid[24];        // "ShinyRelay-1A2B"
    char Pass[kCwlSec_SetupPassLen + 1];   // the setup network's own password, "XXXXX-XXXXX-XXXXX" (shown on the OSD and in its QR code; not a secret of the user)
    uint32_t SecondsLeft;
    uint8_t Clients;
    bool Joining;         // the phone sent a network, trying it
    CwlNetResult_t Result;   // of the last network the phone sent
    char JoinSsid[kCwlNet_SsidMax + 1];
    uint32_t HeapFree, HeapMin;   // measured while it runs
} CwlNetPortal_t;
void CwlNet_PortalState(CwlNetPortal_t *Out);

// ---- link session hooks (cwl_esp.c)
// The link could not join its network kConnFails times: scan (rate-limited)
// and switch to the strongest saved network in range. Call from the link task.
void CwlNet_AutoJoinPoll(uint32_t NowMs, bool Associated, int ConnFails);

// Console: cwl net ...
int CwlNet_Console(int argc, char **argv);
// "net=<ssid> saved=N" for CWS? (no password, ever).
int CwlNet_StatusExtra(char *Buf, int Len);

#ifdef __cplusplus
}
#endif
