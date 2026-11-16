// The one-time question before a public party match. See party_ask.h.
#include "party_ask.h"

#include <stdio.h>
#include <string.h>

void PartyAsk_Init(PartyAsk_t *p)
{
    memset(p, 0, sizeof(*p));
    p->State = kPa_Idle;
}

bool PartyAsk_IsPartyFam(const char *fam)
{
    return fam != NULL && strncmp(fam, "SHPTY1:", 7) == 0;
}

bool PartyAsk_IsPublic(const char *fam, uint8_t mode)
{
    return PartyAsk_IsPartyFam(fam) && (mode == 2 || mode == 3);
}

int PartyAsk_AskMode(const char *fam)
{
    return PartyAsk_IsPartyFam(fam) ? kShl_ModeMine : -1;
}

unsigned PartyAsk_ShownPlayers(const char *fam, uint8_t peers)
{
    if (PartyAsk_IsPartyFam(fam)) return peers ? peers : 1u;
    return peers + 1u;
}

bool PartyAsk_OnPick(PartyAsk_t *p, const char *fam, uint8_t mode, int TicketConsent)
{
    p->Rejoined = false;
    p->LastHttp = 0;
    p->State = kPa_Idle;
    if (!PartyAsk_IsPublic(fam, mode) || TicketConsent != 0) return false;
    p->Mode = mode;
    p->State = kPa_Asking;
    return true;
}

bool PartyAsk_OnEnded(PartyAsk_t *p, const char *fam, uint8_t mode, uint8_t reason)
{
    if (reason != kShl_RPartyConsent || !PartyAsk_IsPublic(fam, mode)) {
        if (p->State == kPa_Rejoin) p->State = kPa_Idle;
        return false;
    }
    if (p->Rejoined) {
        // Refused again right after the answer was saved: say so, never loop.
        p->State = kPa_Failed;
        return false;
    }
    p->Mode = mode;
    p->State = kPa_Asking;
    return true;
}

PaAnswer_t PartyAsk_OnAnswer(PartyAsk_t *p, PaAnswer_t a)
{
    if (p->State != kPa_Asking) return kPa_AnswerNone;
    if (a == kPa_AnswerNone) {
        p->State = kPa_Idle;
        return kPa_AnswerNone;
    }
    p->State = kPa_Posting;
    return a;
}

bool PartyAsk_OnPosted(PartyAsk_t *p, int http)
{
    if (p->State != kPa_Posting) return false;
    p->LastHttp = http;
    if (http == 200 && !p->Rejoined) {
        p->State = kPa_Rejoin;
        return true;
    }
    p->State = kPa_Failed;
    return false;
}

void PartyAsk_OnRejoinStarted(PartyAsk_t *p)
{
    p->Rejoined = true;
    p->State = kPa_Idle;
}

size_t PartyAsk_Body(PaAnswer_t a, char *out, size_t cap)
{
    if (cap > 0) out[0] = '\0';
    if (a != kPa_AnswerOwn && a != kPa_AnswerGeneric) return 0;
    const int n = snprintf(out, cap, "{\"party_consent\":true,\"party_name\":\"%s\"}",
                           a == kPa_AnswerOwn ? "own" : "generic");
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}
