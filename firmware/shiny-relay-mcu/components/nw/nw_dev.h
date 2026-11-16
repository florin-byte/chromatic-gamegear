// nw_dev.h: network window service on the ESP32 (NETWORK_WINDOW.md).
//
// The FPGA (18.21+) exposes a mailbox at GB FF58-FF5F; its packets reach the
// ESP32 on UART address 0x0B (fpga_rx.c -> NW_OnFpgaPacket) and the ESP32
// answers on address 0x11. nw_esp.c (shared with the Mac reference in
// tools/nw) runs the transport and policy; this file adds the task, the HTTP
// backend (gateway base URL in NVS "nw"/"url"), the OSD switch and the `nw`
// console command. Wi-Fi itself belongs to the CWL link task (cwl.h): this
// service asks it to stay up while a game has the window unlocked.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// After NVS and CWL_Initialize: loads settings, registers `nw`.
void NW_Initialize(void);
// After CWL_AutoStart: starts the service and HTTP tasks.
void NW_Start(void);
// a cart job: wait out a request in flight, then free the network window's HTTPS client
bool NW_DropClient(uint32_t wait_ms);

// A packet the FPGA sent on address 0x0B (called from the FPGA RX task).
void NW_OnFpgaPacket(const uint8_t *Payload, uint8_t Len);
// cartspeed1: an express cart stream status (UART address 0x0C), handed to the cart job on the nw task
void NW_OnCartXsPacket(const uint8_t *Payload, uint8_t Len);

// The OSD switch (SYSTEM > NETWORK). Persists in NVS ("nw", key "enabled").
bool NW_IsEnabled(void);
void NW_SetEnabled(bool Enabled);
// Short OSD texts.
const char *NW_StatusText(void);
void NW_GetDetailText(char *Buf, unsigned Len);

// SHLINK (lane chromshlink1): the running game's multiplayer room.
typedef struct {
    bool Attached;     // a SHLINK v1.1 game is running (it sent LINK_JOIN)
    int State;         // ShlState_t
    bool InRoom, Host;
    uint8_t Peers, MaxPeers;
    uint16_t Players;  // 1.0.1: the count to show (PartyAsk_ShownPlayers: a party seat's peers already count you)
    char Code[7];      // the room's join code (show it when hosting)
    char Ended[16];    // why the last room attempt ended ("" = it did not)
    // 0.17.7: the one-time question before a public party match (party_ask.h)
    bool AskConsent;   // show PUBLIC MATCH / YOUR NAME WILL BE VISIBLE
    bool AskBusy;      // the answer is being saved, or the match joined again
    bool AskFailed;    // it could not be saved / the relay refused again
} NwRoomInfo_t;
bool NW_LinkAttached(void);
// Mode: 0 host, 1 join (Arg = code), 2 my devices (mine), 3 room (Arg = room id), 100 leave.
void NW_RoomRequest(int Mode, const char *Arg);
void NW_GetRoomInfo(NwRoomInfo_t *Out);
// The player's answer to the public-match question: 0 B (back), 1 A (OK, own
// name), 2 SELECT (OK, generic name).
void NW_PartyAnswer(int Answer);
// SYSTEM > DOWNLOAD (lane wifirecv1): "Send to my Chromatic" in receive mode. What the page
// shows (the install machine owns it: cartflash/cf_install.h CV_*, same numbers) and its buttons
// (1 A, 0 B). Kind -1: this build has no Wi-Fi install (no DOWNLOAD item).
enum { kNwCv_Menu, kNwCv_Getting, kNwCv_Code, kNwCv_Expired, kNwCv_Fail, kNwCv_Arrived, kNwCv_Battery,
       kNwCv_Installing, kNwCv_Result, kNwCv_Unfinished };
typedef struct {
    int Kind;
    char Title[24];     // (m110t: 17 cut "NOT  ON  THE  CARD" to "NOT  ON  THE  CA")
    char Note[48];      // kNwCv_Fail / kNwCv_Result: "ROW1|ROW2"; kNwCv_Installing: "NN%"; kNwCv_Unfinished: a row or "" (m110t: 40 cut "...SEND  AGAIN")
    char Code[8];       // kNwCv_Code
    unsigned Mins;      // kNwCv_Code: whole minutes left (0: a game came)
    int Anyway;         // kNwCv_Installing: INSTALL ANYWAY on a low battery (1.0.5): the warning rows
    int Backup;         // kNwCv_Installing: 1.0.6, a backup of the cart to the website (BACKING UP, B: STOP)
} NwCartView_t;
void NW_CartView(NwCartView_t *Out);
void NW_CartButton(int A);
// SYSTEM > UPDATE (1.1.0): Shiny Relay's own update over Wi-Fi (cartflash/cf_ota.h OV_*, same numbers).
// Kind -1: this build has no update over Wi-Fi.
enum { kNwFv_Menu, kNwFv_Busy, kNwFv_Download, kNwFv_Ready, kNwFv_Switch, kNwFv_Result,
       kNwFv_Fpga,     // fpgacomp1: the FPGA install / the recovery design (Title, Note "ROW1|ROW2", Pct; -1 = no %)
       kNwFv_Offer };  // 1.1.12, SYSTEM > CUSTOM: a custom build waits on the server (Name, Version; A downloads it)
typedef struct {
    int Kind;
    char Title[24];
    char Note[40];      // kNwFv_Result: "ROW1|ROW2"
    char Version[16];   // the running one (menu), or the update's
    char Running[16];   // always the running one
    int Pct;            // kNwFv_Download
    int ReadyWaits;     // kNwFv_Menu: an update is in the slot (UPDATE READY)
    int Custom;         // 1.1.12: what is shown (or waits in the slot) is a custom build, not a release
    char Name[24];      // the custom build's project name ("" not known)
} NwFwView_t;
void NW_FwView(NwFwView_t *Out);
// 1.1.12, SYSTEM > CUSTOM: check for the custom build this unit's account chose (the update machine's second channel)
void NW_FwCustom(void);
// SYSTEM > PLAY (1.1.0, FPGA 18.30): the game kept in this Chromatic, played with no cartridge
// (cartflash/cf_rc_dev.c). Kind -1: not in this build.
enum { kNwPv_None, kNwPv_Ready, kNwPv_Loading, kNwPv_Playing, kNwPv_Result, kNwPv_NoFpga };
typedef struct {
    int Kind;
    char Title[17];
    char Note[40];      // kNwPv_Result: "ROW1|ROW2"; kNwPv_Playing: the save's state
    int Pct;            // kNwPv_Loading
} NwPlayView_t;
void NW_PlayView(NwPlayView_t *Out);
void NW_PlayButton(int A);
void NW_PlayOpenMenu(void);
// lane sdcard1: a game on the microSD card played with no cartridge
void NW_PlaySd(const char *Path);
// lane gamecomp1: the games kept on the FPGA's flash (SYSTEM > PLAY's list; cartflash/cf_lib.c)
typedef struct { uint32_t Uid, RomSize, StoredSize, SaveSize; uint8_t Sgz; char Title[24]; } NwLibGame_t;
int  NW_LibCount(uint32_t *FreeB, uint32_t *TotalB, uint32_t *Gen);   // games; -1 no library on this unit, -2 being read
int  NW_LibGet(int I, NwLibGame_t *Out);
void NW_LibHold(bool On);                                              // the page is open: read it, keep it read
int  NW_LibBusy(void);
int  NW_LibResult(void);
int  NW_LibDelete(uint32_t Uid);
void NW_LibPlay(uint32_t Uid);
int  NW_LibPlayingUid(void);
// lane ui110dev (SYSTEM > GAMES): a game played once the one playing has stopped (its save kept): card 1 = Path on the
// card, else the kept game Uid; the cartridge's title when a cart job has read it this boot ("" unknown); the card game
// playing
void NW_PlayAfterStop(int Card, uint32_t Uid, const char *Path);
void NW_CartTitle(char *Out, unsigned Len);
bool NW_SdPlayingPath(const char *Path);
// lane ui110dev: SYSTEM > GET GAMES over the install machine (cartflash cf_install.c's catalog mode): receive mode with no
// code on the screen; the page's own calls between the polls (Body: JSON with one %s = the code); whether one has its code;
// whether this receive mode is GET GAMES's; its page put on the screen (an install's result)
void NW_GgStart(void);
int  NW_GgSide(const char *Path, const char *Body, char *Out, int Cap);
int  NW_GgSideDone(int *Code);
int  NW_GgReady(void);
int  NW_GgActive(void);
int  NW_Paired(void);
// lane savecloud1 (SAVE TO MY GAMES): 1 = paired and a Wi-Fi network saved; whether the kept game has a stored save (1 yes,
// 0 none, -1 not known yet: it is being looked up); the answer forgotten (a game's page opens / closes)
int  NW_SaveCloudReady(void);
int  NW_SaveCloudHas(uint32_t Uid);
void NW_SaveCloudForget(void);
void NW_GgOwnJob(int On);        // lane mygames1: the card job running is GET GAMES's (its end reopens GET GAMES, not GAMES)
int  NW_GgOwnsJob(void);            // lane mygames1: 1 = this unit is paired to a Shiny Gen account (MY GAMES)
void NW_GgOpenMenu(void);
void NW_FwButton(int A);
// Receive mode holds the network window (game traffic refused, the room left, the idle
// connection closed) so its heap is for the device calls. RAM only.
void NW_RecvHold(bool On);
bool NW_RecvHeld(void);
// Called (from the nw task) when the game asks for the room menu (lk_ask).
void NW_SetRoomMenuOpener(void (*Fn)(void));
// 1.0.2: opens the OSD on a SYSTEM item by name ("WI-FI"), for a game's LINK_UI.
void NW_SetPageOpener(void (*Fn)(const char *Item));
void NW_OpenPage(const char *Item);

#ifdef __cplusplus
}
#endif
