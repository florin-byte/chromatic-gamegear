// shl_client.c: see shl_client.h.
#include "shl_client.h"

#include <string.h>

static void Wr16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)(v >> 8); b[1] = (uint8_t)v; }
static void Wr32(uint8_t *b, uint32_t v) { for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (24 - 8 * i)); }
static void Wr64(uint8_t *b, uint64_t v) { for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (56 - 8 * i)); }
static uint16_t Rd16(const uint8_t *b) { return (uint16_t)((b[0] << 8) | b[1]); }
static uint32_t Rd32(const uint8_t *b) { return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3]; }
static uint64_t Rd64(const uint8_t *b) { return ((uint64_t)Rd32(b) << 32) | Rd32(b + 4); }
static bool EpEq(ShlEp_t a, ShlEp_t b) { return a.ip == b.ip && a.port == b.port; }
static bool SeqLt(uint16_t a, uint16_t b) { return (int16_t)(uint16_t)(a - b) < 0; }

static void Hdr(uint8_t *o, uint8_t t)
{
    o[0] = 'S';
    o[1] = 'L';
    o[2] = 1;
    o[3] = t;
}

static size_t Field(uint8_t *o, size_t at, size_t cap, const char *s, size_t max)
{
    size_t n = s ? strlen(s) : 0;
    if (n > max) n = max;
    if (at + 1 + n > cap) return 0;
    o[at] = (uint8_t)n;
    if (n) memcpy(o + at + 1, s, n);
    return at + 1 + n;
}

size_t Shl_EncodeJoin(uint8_t *out, size_t cap, uint32_t nonce, uint8_t mode, uint8_t maxp, const char *code,
                      uint64_t client_id, const char *room, const char *ident, const char *fam,
                      const uint8_t *ticket, size_t ticket_len)
{
    if (cap < 24 || ticket_len == 0 || ticket_len > 1024) return 0;
    Hdr(out, kShl_TSJoin);
    Wr32(out + 4, nonce);
    out[8] = mode;
    out[9] = maxp;
    memset(out + 10, 0, 6);
    if (mode == kShl_ModeJoin && code) {
        for (int i = 0; i < 6 && code[i]; i++) {
            const char ch = code[i];
            out[10 + i] = (uint8_t)((ch >= 'a' && ch <= 'z') ? ch - 32 : ch);
        }
    }
    Wr64(out + 16, client_id);
    size_t at = 24;
    if (!(at = Field(out, at, cap, mode == kShl_ModeRoom ? room : "", kShl_RoomIdMax))) return 0;
    if (!(at = Field(out, at, cap, ident, kShl_IdentMax))) return 0;
    if (!(at = Field(out, at, cap, fam, kShl_FamMax))) return 0;
    if (at + 2 + ticket_len > cap) return 0;
    Wr16(out + at, (uint16_t)ticket_len);
    memcpy(out + at + 2, ticket, ticket_len);
    return at + 2 + ticket_len;
}

size_t Shl_EncodeData(uint8_t *out, size_t cap, uint64_t token, uint16_t ack, uint16_t seq,
                      const ShlItem_t *items, uint8_t n)
{
    if (cap < 17) return 0;
    Hdr(out, kShl_TSData);
    Wr64(out + 4, token);
    Wr16(out + 12, ack);
    Wr16(out + 14, seq);
    size_t at = 17;
    uint8_t k = 0;
    for (; k < n; k++) {
        if (at + 1 + items[k].len > cap) break;
        out[at] = items[k].len;
        memcpy(out + at + 1, items[k].b, items[k].len);
        at += 1 + (size_t)items[k].len;
    }
    out[16] = k;
    return at;
}

const char *Shl_ReasonName(uint8_t r)
{
    switch (r) {
    case 1: return "bad_ticket";
    case kShl_RNoRoom: return "no_room";
    case kShl_RRoomFull: return "room_full";
    case kShl_RRate: return "rate";
    case 5: return "bad_packet";
    case 6: return "relay_full";
    case 7: return "replay";
    case kShl_RUnknownSession: return "unknown_session";
    case 9: return "no_rooms_here";
    case kShl_RRoomClosed: return "room_closed";
    case kShl_RKicked: return "kicked";
    case kShl_RTaken: return "taken";
    case kShl_RPartyConsent: return "party_consent";
    default: return "?";
    }
}

const char *Shl_StateName(const ShlClient_t *c)
{
    switch (c->state) {
    case kShl_Idle: return "idle";
    case kShl_NeedTicket: return "ticket";
    case kShl_Joining: return "joining";
    case kShl_Seated: return c->in_room ? "in_room" : "seated";
    case kShl_Ended: return "ended";
    }
    return "?";
}

void Shl_Init(ShlClient_t *c, const ShlHost_t *host)
{
    memset(c, 0, sizeof(*c));
    c->host = *host;
    c->rto_ms = kShl_RtoInit_ms;
}

static void ClearSeat(ShlClient_t *c)
{
    c->token = 0;
    c->seat_has_state = false;
    c->in_room = c->is_host = false;
    c->peers = c->seat = 0;
    c->tx_n = 0;
    c->tx_base = 0;
    c->rx_expect = 0;
    c->dirty = c->ack_owed = false;
    c->front_sent = c->front_resent = false;
    c->down_n = c->down_head = 0;
    c->rto_ms = c->have_srtt ? c->rto_ms : kShl_RtoInit_ms;
}

static void Send(ShlClient_t *c, const uint8_t *b, size_t n)
{
    if (n && c->relay.ip && c->relay.port) c->host.send(c->host.ctx, c->relay, b, n);
}

static void AskTicket(ShlClient_t *c)
{
    c->state = kShl_NeedTicket;
    c->join_len = 0;
    if (c->host.need_ticket) c->host.need_ticket(c->host.ctx);
}

void Shl_Start(ShlClient_t *c, ShlEp_t relay, uint8_t mode, const char *code, const char *room,
               const char *ident, const char *fam, uint8_t maxp, uint64_t client_id, uint32_t now_ms)
{
    (void)now_ms;
    if (c->state == kShl_Seated) Shl_Leave(c);
    ClearSeat(c);
    c->relay = relay;
    c->mode = c->asked_mode = mode;
    memset(c->code, 0, sizeof(c->code));
    if (code) strncpy(c->code, code, 6);
    memset(c->room, 0, sizeof(c->room));
    if (room) strncpy(c->room, room, kShl_RoomIdMax);
    memset(c->ident, 0, sizeof(c->ident));
    if (ident) strncpy(c->ident, ident, kShl_IdentMax);
    memset(c->fam, 0, sizeof(c->fam));
    if (fam) strncpy(c->fam, fam, kShl_FamMax);
    c->maxp = maxp;
    c->client_id = client_id;
    c->ended_reason = 0;
    memset(c->room_code, 0, sizeof(c->room_code));
    AskTicket(c);
}

void Shl_SetTicket(ShlClient_t *c, const char *ticket, size_t len, uint32_t now_ms)
{
    if (c->state != kShl_NeedTicket) return;
    c->nonce = c->host.rand32 ? c->host.rand32(c->host.ctx) : now_ms;
    if (c->nonce == 0) c->nonce = 1;
    c->join_len = (uint16_t)Shl_EncodeJoin(c->join, sizeof(c->join), c->nonce, c->mode, c->maxp, c->code,
                                           c->client_id, c->room, c->ident, c->fam, (const uint8_t *)ticket, len);
    if (c->join_len == 0) {
        c->state = kShl_Ended;
        c->ended_reason = 5;
        return;
    }
    c->state = kShl_Joining;
    c->join_start_ms = c->join_last_ms = now_ms;
    Send(c, c->join, c->join_len);
    c->stats.joins_sent++;
}

void Shl_Leave(ShlClient_t *c)
{
    if (c->state == kShl_Seated && c->token) {
        uint8_t b[12];
        Hdr(b, kShl_TBye);
        Wr64(b + 4, c->token);
        Send(c, b, sizeof b);
    }
    ClearSeat(c);
    c->state = kShl_Idle;
}

bool Shl_InRoom(const ShlClient_t *c) { return c->state == kShl_Seated && c->in_room; }

uint8_t Shl_TxFree(const ShlClient_t *c)
{
    return c->state == kShl_Seated ? (uint8_t)(kShl_TxItems - c->tx_n) : 0;
}

bool Shl_Push(ShlClient_t *c, const uint8_t *payload, uint8_t n)
{
    if (c->state != kShl_Seated || n == 0 || n > kShl_MsgMax || c->tx_n >= kShl_TxItems) return false;
    ShlItem_t *it = &c->tx[c->tx_n++];
    it->len = (uint8_t)(1 + n);
    it->b[0] = kShl_IMsg;
    memcpy(it->b + 1, payload, n);
    c->dirty = true;
    c->stats.msgs_up++;
    return true;
}

uint8_t Shl_DownCount(const ShlClient_t *c) { return c->down_n; }

uint8_t Shl_Pop(ShlClient_t *c, uint8_t *from, uint8_t *out)
{
    if (c->down_n == 0) return 0;
    const ShlItem_t *it = &c->down[c->down_head];
    *from = it->b[0];
    const uint8_t n = (uint8_t)(it->len - 1);
    memcpy(out, it->b + 1, n);
    c->down_head = (uint8_t)((c->down_head + 1) % kShl_DownItems);
    c->down_n--;
    return n;
}

// One in-order item from the relay. False = no room for it now (not acked).
static bool Accept(ShlClient_t *c, const uint8_t *it, uint8_t n)
{
    switch (it[0]) {
    case kShl_IMsg:
        if (n < 3 || n - 2 > kShl_MsgMax) return true;   // malformed: skip it
        if (c->down_n >= kShl_DownItems) {
            c->stats.held_back++;
            return false;
        }
        {
            ShlItem_t *d = &c->down[(c->down_head + c->down_n) % kShl_DownItems];
            d->len = (uint8_t)(n - 1);
            memcpy(d->b, it + 1, (size_t)(n - 1));
            c->down_n++;
            c->stats.msgs_down++;
        }
        return true;
    case kShl_IState:
        if (n < 13) return true;
        c->in_room = (it[1] & kShl_FRoom) != 0;
        c->is_host = (it[1] & kShl_FHost) != 0;
        c->peers = it[2];
        c->seat = it[3];
        c->max_peers = it[5];
        memcpy(c->room_code, it + 6, 6);
        c->room_code[6] = '\0';
        {
            const uint8_t rl = it[12];
            if (rl <= kShl_RoomIdMax && 13 + rl <= n) {
                memcpy(c->room, it + 13, rl);
                c->room[rl] = '\0';
            }
        }
        if (!c->seat_has_state) {
            c->seat_has_state = true;
            c->seat_gen++;
        }
        c->stats.states++;
        return true;
    case kShl_IEnded:
        c->ended_reason = n >= 2 ? it[1] : 0;
        c->in_room = false;
        c->state = kShl_Ended;
        return true;
    default:
        return true;
    }
}

static void OnData(ShlClient_t *c, const uint8_t *b, size_t n, uint32_t now)
{
    if (n < 17 || Rd64(b + 4) != c->token) return;
    const uint16_t ack = Rd16(b + 12), seq = Rd16(b + 14);
    const uint8_t cnt = b[16];
    c->stats.pkts_in++;
    c->last_heard_ms = now;
    // acks
    const uint16_t acked = (uint16_t)(ack - c->tx_base);
    if (acked > 0 && acked <= c->tx_n) {
        if (c->front_sent && !c->front_resent) {
            const uint32_t rtt = now - c->front_sent_ms;
            c->srtt_ms = c->have_srtt ? (c->srtt_ms * 7 + rtt) / 8 : rtt;
            c->have_srtt = true;
        }
        c->rto_ms = c->have_srtt ? c->srtt_ms * 2 + 30 : kShl_RtoInit_ms;
        if (c->rto_ms < kShl_RtoMin_ms) c->rto_ms = kShl_RtoMin_ms;
        if (c->rto_ms > kShl_RtoMax_ms) c->rto_ms = kShl_RtoMax_ms;
        memmove(c->tx, c->tx + acked, sizeof(ShlItem_t) * (size_t)(c->tx_n - acked));
        c->tx_n = (uint8_t)(c->tx_n - acked);
        c->tx_base = ack;
        c->front_sent = c->front_resent = false;
        if (c->tx_n) c->dirty = true;
    }
    // items
    size_t at = 17;
    if (cnt) c->ack_owed = true;
    for (uint8_t i = 0; i < cnt; i++) {
        if (at >= n) return;
        const uint8_t l = b[at];
        if (l == 0 || at + 1 + l > n) return;
        const uint16_t s = (uint16_t)(seq + i);
        if (s == c->rx_expect) {
            if (!Accept(c, b + at + 1, l)) return;
            c->rx_expect++;
            c->stats.items_in++;
            if (c->state != kShl_Seated) return;   // ENDED
        } else if (SeqLt(s, c->rx_expect)) {
            c->stats.dups_in++;
        } else {
            return;
        }
        at += 1 + (size_t)l;
    }
}

static void Rejoin(ShlClient_t *c)
{
    // The relay forgot the seat (a relay restart, a long silence): back into
    // the same room by its id when we know it, else as first asked.
    c->stats.rejoins++;
    const bool by_room = c->seat_has_state && c->room[0] != '\0';
    ClearSeat(c);
    c->mode = by_room ? kShl_ModeRoom : c->asked_mode;
    AskTicket(c);
}

bool Shl_OnDatagram(ShlClient_t *c, ShlEp_t from, const uint8_t *b, size_t n, uint32_t now)
{
    if (n < 4 || b[0] != 'S' || b[1] != 'L' || b[2] != 1) return false;
    const uint8_t t = b[3];
    if (t != kShl_TSWelcome && t != kShl_TSData && t != kShl_TRefuse) return false;
    if (!EpEq(from, c->relay)) return false;
    switch (t) {
    case kShl_TSWelcome:
        if (n == 16 && c->state == kShl_Joining && Rd32(b + 4) == c->nonce) {
            ClearSeat(c);
            c->token = Rd64(b + 8);
            c->state = kShl_Seated;
            c->last_heard_ms = now;
            c->last_send_ms = now;
            c->ack_owed = true;   // say hello at once (the relay learns our address)
            c->stats.welcomes++;
        }
        return true;
    case kShl_TSData:
        if (c->state == kShl_Seated) OnData(c, b, n, now);
        return true;
    case kShl_TRefuse:
        if (n == 9) {
            const uint32_t nonce = Rd32(b + 4);
            const uint8_t r = b[8];
            if (c->state == kShl_Joining && nonce == c->nonce) {
                c->stats.refusals++;
                c->stats.last_refuse = r;
                if (r == 1 || r == 7) AskTicket(c);   // bad ticket / replay: another one
                else {
                    c->state = kShl_Ended;
                    c->ended_reason = r;
                }
            } else if (c->state == kShl_Seated && nonce == 0 && r == kShl_RUnknownSession) {
                Rejoin(c);
            }
        }
        return true;
    }
    return true;
}

void Shl_Poll(ShlClient_t *c, uint32_t now)
{
    if (c->state == kShl_Joining) {
        if (now - c->join_start_ms >= kShl_TicketMaxAge_ms) {
            AskTicket(c);   // the ticket aged out unanswered
        } else if (now - c->join_last_ms >= kShl_JoinResend_ms) {
            c->join_last_ms = now;
            Send(c, c->join, c->join_len);   // same nonce + ticket: answered idempotently
            c->stats.joins_sent++;
        }
        return;
    }
    if (c->state != kShl_Seated) return;
    const uint32_t since = now - c->last_send_ms;
    const bool timed_out = c->tx_n && since >= c->rto_ms;
    if (!(c->dirty || c->ack_owed || timed_out || since >= kShl_Keepalive_ms)) return;
    if (timed_out && !c->dirty) {
        c->stats.resends++;
        c->front_resent = true;
        c->rto_ms = c->rto_ms * 2 > kShl_RtoMax_ms ? kShl_RtoMax_ms : c->rto_ms * 2;
    } else if (c->front_sent && c->tx_n) {
        c->front_resent = true;
    }
    uint8_t *const pkt = c->join;   /* seated: the SJOIN is spent, its buffer builds the SDATA */
    uint8_t k = 0;
    size_t bytes = 0;
    while (k < c->tx_n && bytes + 1 + c->tx[k].len <= kShl_Budget) bytes += 1 + (size_t)c->tx[k++].len;
    const size_t n = Shl_EncodeData(pkt, sizeof c->join, c->token, c->rx_expect, c->tx_base, c->tx, k);
    Send(c, pkt, n);
    if (c->tx_n && !c->front_sent) {
        c->front_sent = true;
        c->front_sent_ms = now;
    }
    c->dirty = c->ack_owed = false;
    c->last_send_ms = now;
    c->stats.pkts_out++;
    c->stats.items_out += k;
}
