#pragma once
// The one-time question before a PUBLIC party match (0.17.7; PARTY_ROOMS.md 8.4,
// CHROMATIC_RELAY.md 14). Portable: nw_dev.c drives it on the device, and
// host/party_test.c on the Mac.
//
// A public party seat is a SHLINK seat whose family starts "SHPTY1:" taken in
// mode 2 (the quick match, MY DEVICES) or 3 (a public room by id). HOST and
// JOIN CODE are private lobbies: never asked.
//
//   idle --(ticket says no consent, public pick)--> asking
//   idle --(public seat ENDED with reason 14)-----> asking
//   asking --B--> idle (no call)
//   asking --A / SELECT--> posting (POST /link_device/party_consent)
//   posting --200--> rejoin: a NEW ticket, then the same SJOIN once
//   posting --anything else--> failed (the room menu's error line; no retry)
//   a rejoin that ENDs with reason 14 again --> failed (never asks twice in a row)

#include "shl_client.h"   // kShl_RPartyConsent

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    kPa_Idle,
    kPa_Asking,      // the OSD shows the question
    kPa_Posting,     // the answer is on its way to the API
    kPa_Rejoin,      // 200: a new ticket and the SJOIN again
    kPa_Failed,      // the answer could not be saved, or the relay refused again
} PaState_t;

typedef enum {
    kPa_AnswerNone,      // B: no call, back to the room menu
    kPa_AnswerOwn,       // A: OK, with the account's player name
    kPa_AnswerGeneric,   // SELECT: OK, as "Player NN"
} PaAnswer_t;

typedef struct {
    PaState_t State;
    uint8_t Mode;        // the public mode the player picked (2 or 3)
    bool Rejoined;       // the one retry after a 200 has been made
    int LastHttp;        // the POST's status (0 none yet, -1 no network)
} PartyAsk_t;

void PartyAsk_Init(PartyAsk_t *p);
bool PartyAsk_IsPartyFam(const char *fam);
bool PartyAsk_IsPublic(const char *fam, uint8_t mode);
// 1.0.1: the game asked for its room (lk_ask) and no room is live. A party game
// (family "SHPTY1:") takes the quick match (MY DEVICES, kShl_ModeMine) at once,
// with no room menu: PARTY means the public match (owner 2026-10-12). Returns
// the mode to take, or -1 = open the room menu as before (every other family).
// The question above still comes first for an account that never answered
// (PartyAsk_OnPick on that pick, as when the player picks MY DEVICES).
int PartyAsk_AskMode(const char *fam);
// 1.0.1: the OSD's player count. The relay's state item says `peers` = the
// OTHER players for a game room, but ALL players (you included) for a party
// seat (PARTY_ROOMS.md 1): the count to show is peers + 1, or peers for a party.
unsigned PartyAsk_ShownPlayers(const char *fam, uint8_t peers);

// The player picked a seat (from the room menu, not the rejoin). TicketConsent:
// the last ticket answer's "party_consent" (-1 not known, 0 false, 1 true).
// True: ask now instead of sending the SJOIN.
bool PartyAsk_OnPick(PartyAsk_t *p, const char *fam, uint8_t mode, int TicketConsent);
// The seat ENDED. True: ask now.
bool PartyAsk_OnEnded(PartyAsk_t *p, const char *fam, uint8_t mode, uint8_t reason);
// The answer while asking. Returns what to POST (kPa_AnswerNone = no call).
PaAnswer_t PartyAsk_OnAnswer(PartyAsk_t *p, PaAnswer_t a);
// The POST's result (HTTP status, -1 no network). True: buy a new ticket and
// send the SJOIN again (once).
bool PartyAsk_OnPosted(PartyAsk_t *p, int http);
// The rejoin's SJOIN went out.
void PartyAsk_OnRejoinStarted(PartyAsk_t *p);
// The body for POST /link_device/party_consent; 0 for kPa_AnswerNone.
size_t PartyAsk_Body(PaAnswer_t a, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
