// party_test.c (0.17.7): the one-time question before a public party match,
// PARTY_ROOMS.md 8.4 / CHROMATIC_RELAY.md 14. Mac only:
//   make -C components/nw/host party_test && ./party_test
#include "party_ask.h"
#include "shl_client.h"

#include <stdio.h>
#include <string.h>

static int _Checks, _Fails;
#define CHECK(c) do { _Checks++; if (!(c)) { _Fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const char *kParty = "SHPTY1:br-blocks";
static const char *kGame = "SHFAM1:mrt14";

int main(void)
{
    PartyAsk_t p;
    char b[96];

    // The constant and its name, as the relay sends them.
    CHECK(kShl_RPartyConsent == 14);
    CHECK(strcmp(Shl_ReasonName(14), "party_consent") == 0);
    CHECK(strcmp(Shl_ReasonName(2), "no_room") == 0);

    // What is public: a SHPTY1: family in mode 2 (quick match) or 3 (room by id).
    CHECK(PartyAsk_IsPartyFam(kParty) && !PartyAsk_IsPartyFam(kGame) && !PartyAsk_IsPartyFam("") && !PartyAsk_IsPartyFam(NULL));
    CHECK(PartyAsk_IsPublic(kParty, 2) && PartyAsk_IsPublic(kParty, 3));
    CHECK(!PartyAsk_IsPublic(kParty, 0) && !PartyAsk_IsPublic(kParty, 1));   // HOST, JOIN CODE: private
    CHECK(!PartyAsk_IsPublic(kGame, 2));

    // Before joining, from the ticket answer.
    PartyAsk_Init(&p);
    CHECK(p.State == kPa_Idle);
    CHECK(PartyAsk_OnPick(&p, kParty, 2, 0) && p.State == kPa_Asking && p.Mode == 2);
    PartyAsk_Init(&p);
    CHECK(!PartyAsk_OnPick(&p, kParty, 2, 1) && p.State == kPa_Idle);     // already said OK
    CHECK(!PartyAsk_OnPick(&p, kParty, 2, -1) && p.State == kPa_Idle);    // not known: join, the relay decides
    CHECK(!PartyAsk_OnPick(&p, kParty, 0, 0) && !PartyAsk_OnPick(&p, kParty, 1, 0));   // never for HOST / JOIN CODE
    CHECK(!PartyAsk_OnPick(&p, kGame, 2, 0));                              // a game room

    // The authoritative signal: the seat ENDED with reason 14.
    PartyAsk_Init(&p);
    CHECK(!PartyAsk_OnEnded(&p, kParty, 2, kShl_RNoRoom) && p.State == kPa_Idle);
    CHECK(!PartyAsk_OnEnded(&p, kGame, 2, kShl_RPartyConsent));
    CHECK(!PartyAsk_OnEnded(&p, kParty, 0, kShl_RPartyConsent));
    CHECK(PartyAsk_OnEnded(&p, kParty, 3, kShl_RPartyConsent) && p.State == kPa_Asking && p.Mode == 3);

    // B: no call, back to the menu.
    CHECK(PartyAsk_OnAnswer(&p, kPa_AnswerNone) == kPa_AnswerNone && p.State == kPa_Idle);
    CHECK(PartyAsk_OnAnswer(&p, kPa_AnswerOwn) == kPa_AnswerNone);         // not asking: nothing

    // A: OK with the account's name; the body; 200: a new ticket and the SJOIN once.
    CHECK(PartyAsk_OnEnded(&p, kParty, 2, kShl_RPartyConsent));
    CHECK(PartyAsk_OnAnswer(&p, kPa_AnswerOwn) == kPa_AnswerOwn && p.State == kPa_Posting);
    CHECK(PartyAsk_Body(kPa_AnswerOwn, b, sizeof b) > 0 && strcmp(b, "{\"party_consent\":true,\"party_name\":\"own\"}") == 0);
    CHECK(PartyAsk_OnPosted(&p, 200) && p.State == kPa_Rejoin);
    PartyAsk_OnRejoinStarted(&p);
    CHECK(p.State == kPa_Idle && p.Rejoined);
    // Refused again right after: an error line, never a second question.
    CHECK(!PartyAsk_OnEnded(&p, kParty, 2, kShl_RPartyConsent) && p.State == kPa_Failed);
    CHECK(!PartyAsk_OnPosted(&p, 200));                                    // nothing is posting
    // The player picks again: a fresh start.
    CHECK(PartyAsk_OnPick(&p, kParty, 2, 0) && p.State == kPa_Asking && !p.Rejoined);

    // SELECT: a generic name.
    CHECK(PartyAsk_OnAnswer(&p, kPa_AnswerGeneric) == kPa_AnswerGeneric);
    CHECK(PartyAsk_Body(kPa_AnswerGeneric, b, sizeof b) > 0 && strcmp(b, "{\"party_consent\":true,\"party_name\":\"generic\"}") == 0);
    // Any non-200, or no network: the error line, no retry.
    CHECK(!PartyAsk_OnPosted(&p, 500) && p.State == kPa_Failed && p.LastHttp == 500);
    PartyAsk_Init(&p);
    CHECK(PartyAsk_OnEnded(&p, kParty, 2, kShl_RPartyConsent) && PartyAsk_OnAnswer(&p, kPa_AnswerOwn) == kPa_AnswerOwn);
    CHECK(!PartyAsk_OnPosted(&p, -1) && p.State == kPa_Failed);
    PartyAsk_Init(&p);
    CHECK(PartyAsk_OnEnded(&p, kParty, 2, kShl_RPartyConsent) && PartyAsk_OnAnswer(&p, kPa_AnswerOwn) == kPa_AnswerOwn);
    CHECK(!PartyAsk_OnPosted(&p, 401) && p.State == kPa_Failed);          // unpaired

    // A rejoin that ends for another reason just ends.
    PartyAsk_Init(&p);
    CHECK(PartyAsk_OnEnded(&p, kParty, 2, kShl_RPartyConsent) && PartyAsk_OnAnswer(&p, kPa_AnswerOwn));
    CHECK(PartyAsk_OnPosted(&p, 200));
    CHECK(!PartyAsk_OnEnded(&p, kParty, 2, kShl_RRoomFull) && p.State == kPa_Idle);

    // No body for B; a buffer too small gets none.
    CHECK(PartyAsk_Body(kPa_AnswerNone, b, sizeof b) == 0 && b[0] == '\0');
    CHECK(PartyAsk_Body(kPa_AnswerOwn, b, 10) == 0);

    // 1.0.1: a party game's room ask takes the quick match (MY DEVICES) at once, with no
    // room menu; every other family (2-player HOST / JOIN) still opens the menu.
    CHECK(PartyAsk_AskMode(kParty) == kShl_ModeMine && kShl_ModeMine == 2);
    CHECK(PartyAsk_AskMode("SHPTY1:other") == kShl_ModeMine);
    CHECK(PartyAsk_AskMode(kGame) == -1 && PartyAsk_AskMode("") == -1 && PartyAsk_AskMode(NULL) == -1);
    CHECK(PartyAsk_IsPublic(kParty, (uint8_t)PartyAsk_AskMode(kParty)));   // so the question still comes first
    PartyAsk_Init(&p);
    CHECK(PartyAsk_OnPick(&p, kParty, (uint8_t)PartyAsk_AskMode(kParty), 0) && p.State == kPa_Asking);
    // 1.0.1: the OSD count. A game room's peers are the others (+1 = you); a party seat's
    // peers already count you (relay party/shlink_seat.rs state_item, >= 1).
    CHECK(PartyAsk_ShownPlayers(kGame, 0) == 1 && PartyAsk_ShownPlayers(kGame, 1) == 2);
    CHECK(PartyAsk_ShownPlayers(kGame, 255) == 256);
    CHECK(PartyAsk_ShownPlayers(kParty, 4) == 4 && PartyAsk_ShownPlayers(kParty, 1) == 1);
    CHECK(PartyAsk_ShownPlayers(kParty, 0) == 1);

    printf("party_test: %d checks, %d failed\n", _Checks, _Fails);
    return _Fails ? 1 : 0;
}
