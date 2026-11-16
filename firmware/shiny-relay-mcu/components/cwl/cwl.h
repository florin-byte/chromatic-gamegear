// cwl.h: CWL2 Chromatic Wi-Fi Link, ESP32 side.
//
// Carries the GB link port over Wi-Fi (UDP). The FPGA reports link-port
// events (ARMED / DISARM / MASTER / SLAVE_DONE / SLAVE_NAK) on UART channel 0x0A; this module runs the
// protocol (cwl_proto.c) with a peer and answers the FPGA on address 0x10.
//
// Power: the radio runs flat out only while the link is in use (link-port
// events or peer exchanges in the last few seconds). Otherwise it drops to
// Wi-Fi modem sleep, and after a quiet spell it turns Wi-Fi off and lets the
// MCU light-sleep as stock firmware does; the next link-port event wakes it
// (the FPGA sends its stock wake preamble while CONFIG says the ESP32 may
// sleep) and a master transfer is held for the peer while Wi-Fi reconnects.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    kCwlFpgaEvent_Armed     = 1,   // byte: SB
    kCwlFpgaEvent_Disarm    = 2,
    kCwlFpgaEvent_Master    = 3,   // byte: SB (held until MASTER_REPLY)
    kCwlFpgaEvent_SlaveDone = 4,   // byte: SB shifted out to the peer master
    kCwlFpgaEvent_SlaveNak  = 5,   // offered byte refused: port not armed
    kCwlFpgaEvent_Abandon   = 6,   // SB/SC written under the held master: CPU stalled
                                   // until MASTER_REPLY / MASTER_DROP (FPGA write barrier)
    kCwlFpgaEvent_SlaveNakStalled = 7, // offer refused while holding a master with the CPU
                                       // stalled (fast clock): master collision
} CwlFpgaEvent_t;

// What the OSD shows.
typedef enum {
    kCwlStatus_Off,          // link switched off
    kCwlStatus_NoWifi,       // no Wi-Fi network configured (console: cwl wifi)
    kCwlStatus_Asleep,       // Wi-Fi off until a game uses the link port
    kCwlStatus_Connecting,
    kCwlStatus_Searching,    // on Wi-Fi, no partner heard
    kCwlStatus_Partner,      // a partner is answering
} CwlStatus_t;

typedef enum {
    kCwlWake_Other,
    kCwlWake_Fpga,           // FPGA UART (GPIO)
    kCwlWake_Console,        // console UART
} CwlWakeCause_t;

// Call once after NVS is initialized: loads settings and registers the `cwl`
// console command.
void CWL_Initialize(void);

// Call once the FPGA UART is up: starts the link task (idle when disabled).
void CWL_AutoStart(void);

// True while the MCU must not enter light sleep (Wi-Fi is on, or a wake is
// pending).
bool CWL_KeepAwake(void);
// > 0: light sleep must also wake on a timer after this many ms (the relay
// seat keeper pings the relay while Wi-Fi is off). 0: no timer.
uint32_t CWL_SleepTimerMs(void);

// The OSD switch. Persists in NVS ("cwl" namespace, key "enabled").
bool CWL_IsEnabled(void);
void CWL_SetEnabled(bool Enabled);
// A cart job holds the link: the session ends (radio off) and starts again by itself when
// released. RAM only. Returns whether the session had ended within wait_ms.
bool CWL_CartHold(bool On, uint32_t wait_ms);
bool CWL_CartHeld(void);
// wifirecv1: receive mode (SYSTEM > DOWNLOAD > RECEIVE GAME). On: no link session (game link,
// relay seat, ESP-NOW, LAN) and the radio up for the device calls; Off: the mode's session
// again. RAM only, never blocks. Held: the session from before the hold has ended.
void CWL_RecvHold(bool On);
bool CWL_RecvHeld(void);
CwlStatus_t CWL_GetStatus(void);
const char *CWL_StatusText(CwlStatus_t Status);
// The network the link joins ("" if none; never the password).
void CWL_GetSsidText(char *Buf, unsigned Len);
// "CONNECTED", "JOINING", "STANDBY" (Wi-Fi off until the link is used),
// "NOT  JOINED" (no AP needed in this mode / none set), "OFF".
const char *CWL_WifiStateText(void);
// Own IP while on Wi-Fi ("" otherwise).
void CWL_GetIpText(char *Buf, unsigned Len);

// The power manager reports each light sleep (for the status counters).
void CWL_NoteLightSleep(uint32_t Slept_ms, CwlWakeCause_t Cause);

// A link event from the FPGA (called from the FPGA RX task).
void CWL_OnFpgaEvent(uint8_t Event, uint8_t Byte);

// Every packet the FPGA sends (called from the FPGA RX task): counted for the
// Wi-Fi status reply, which works when USB is unavailable.
void CWL_NoteFpgaPacket(uint8_t Addr, uint16_t Data);

// FPGA RX framing diagnostics (bytes read, CRC failures, bad lengths), shown
// in the "CWS?" status reply.
void CWL_NoteFpgaRxBytes(uint32_t Count);
void CWL_NoteFpgaRxError(bool Crc);

// ---- network window service (components/nw) ----
// Raw FPGA version word (status address 6): minor << 6 | major, bit 12 debug.
uint16_t CWL_FpgaVersionRaw(void);
// The network service is on: keep the Wi-Fi session running even while the
// link cable is switched off (Wi-Fi still sleeps when nothing uses it).
void CWL_SetNetworkWanted(bool On);
// Window state / activity: while Unlocked the radio stays on (at least modem
// sleep) and the MCU awake; Activity counts as link use (full power a while).
void CWL_NetworkUse(bool Unlocked, bool Activity);
// On Wi-Fi with an address.
bool CWL_IsOnline(void);
// The link cable is exchanging right now (share the FPGA UART politely).
bool CWL_LinkBusy(void);
// ---- Shiny Link relay (internet play; cwl_relay.c, cwl_online.c) ----
// Run the link task now (pairing needs the radio even with the link off).
void CWL_Wake(void);
// What the relay is doing, for the OSD ("NOT  PAIRED", "PARTNER  DIRECT", ...).
const char *CWL_RelayStatusText(void);
// "ROOM BCD234 45ms" while seated ("" otherwise).
void CWL_RelayRoomText(char *Buf, unsigned Len);

// Extra fields appended to the "CWS?" status line.
void CWL_SetStatusExtra(int (*Fn)(char *Buf, int Len));

// ---- LINK CABLE mode (PROTOCOL.md "Link modes") ----
// AUTO: nearby (ESP-NOW + LAN UDP) and the Shiny Link relay at once, all on the
//       Wi-Fi radio; the first partner heard wins, a nearby one beats the relay.
// NEARBY: ESP-NOW + LAN only (ESP-NOW works with no router at all).
// ONLINE: the relay only.
// OFF: the physical link port.
// Persists in NVS ("cwl"/"mode"; older builds' "enabled" maps to AUTO / OFF).
typedef enum {
    kCwlMode_Auto,
    kCwlMode_Nearby,
    kCwlMode_Online,
    kCwlMode_Off,
    kCwlNumMode,
} CwlMode_t;
CwlMode_t CWL_GetMode(void);
void CWL_SetMode(CwlMode_t Mode);
const char *CWL_ModeText(CwlMode_t Mode);       // "AUTO", "NEARBY", ...
// Second OSD line: the partner's name and path, or the Wi-Fi address.
void CWL_GetDetailText(char *Buf, unsigned Len);

// ---- nearby partner (ESP-NOW pairing) ----
typedef struct {
    uint8_t Mac[6];
    char Name[16];
    int8_t Rssi;
    bool Seen;           // heard in the last 10 s
} CwlNearby_t;
// Sides heard recently (strongest first), for the pick list.
int CWL_GetNearby(CwlNearby_t *Out, int Max);
// The remembered partner (false: none, the next side that answers pairs).
bool CWL_GetPartner(CwlNearby_t *Out);
// Pick a partner (NULL: forget, pair with the next side that answers).
void CWL_SetPartner(const uint8_t *Mac);
// A short notice for the OSD ("LINKED  CHROMATIC 1A2B"): true while it shows.
bool CWL_GetToast(char *Buf, unsigned Len);

#ifdef __cplusplus
}
#endif
