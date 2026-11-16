// cwl_relay.c: Shiny Link relay client (envelope v1). See cwl_relay.h.
//
// Mirrors shiny_gen_3 shiny_server_rs/services/shiny_multiplayer/src/link_wire.rs
// (byte layouts, golden bytes pinned in tools/cwl_relay_test.c) and the
// JOIN / PEER / punch / fallback state machine of src/bin/cwl_relay_shim.rs.
#include "cwl_relay.h"

#include <string.h>

const char kSl_CodeAlphabet[] = "BCDFGHJKMNPQRSTVWXZ23456789";

// ---------------------------------------------------------------- bytes

static void Wr16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)(v >> 8); b[1] = (uint8_t)v; }
static void Wr32(uint8_t *b, uint32_t v) { for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (24 - 8 * i)); }
static void Wr64(uint8_t *b, uint64_t v) { for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (56 - 8 * i)); }
static uint16_t Rd16(const uint8_t *b) { return (uint16_t)((b[0] << 8) | b[1]); }
static uint32_t Rd32(const uint8_t *b) { return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3]; }
static uint64_t Rd64(const uint8_t *b) { return ((uint64_t)Rd32(b) << 32) | Rd32(b + 4); }
static void WrEp(uint8_t *b, SlEp_t e) { Wr32(b, e.ip); Wr16(b + 4, e.port); }
static SlEp_t RdEp(const uint8_t *b) { SlEp_t e = {Rd32(b), Rd16(b + 4)}; return e; }
static bool EpEq(SlEp_t a, SlEp_t b) { return a.ip == b.ip && a.port == b.port; }

static void Hdr(uint8_t *out, uint8_t t)
{
    out[0] = 'S';
    out[1] = 'L';
    out[2] = kSl_Version;
    out[3] = t;
}

int Sl_Type(const uint8_t *b, size_t n)
{
    if (n >= kSl_Hdr && b[0] == 'S' && b[1] == 'L' && b[2] == kSl_Version) return b[3];
    return -1;
}

bool Sl_IsCwlPayload(const uint8_t *p, size_t n)
{
    return n >= kSl_CwlMin && n <= kSl_CwlMax && p[0] == 'C' && p[1] == 'W';
}

size_t Sl_EncodeJoin(uint8_t *out, size_t cap, uint32_t nonce, uint8_t mode, const char *code, uint64_t client_id,
                     SlEp_t local, const uint8_t *ticket, size_t ticket_len)
{
    const size_t n = kSl_JoinFixed + ticket_len;
    if (ticket_len == 0 || ticket_len > kSl_MaxTicket || n > cap) return 0;
    Hdr(out, kSl_Join);
    Wr32(out + 4, nonce);
    out[8] = mode;
    out[9] = kSl_KindCwl2;
    out[10] = 2;   // max_peers: a Game Boy link is 1:1
    memset(out + 11, 0, kSl_CodeLen);
    if (mode == kSl_ModeJoin && code != NULL) {
        for (int i = 0; i < kSl_CodeLen && code[i]; i++) {
            const char c = code[i];
            out[11 + i] = (uint8_t)((c >= 'a' && c <= 'z') ? c - 32 : c);
        }
    }
    Wr64(out + 17, client_id);
    WrEp(out + 25, local);
    Wr16(out + 31, (uint16_t)ticket_len);
    memcpy(out + 33, ticket, ticket_len);
    return n;
}

size_t Sl_EncodeData(uint8_t *out, size_t cap, uint64_t token, const uint8_t *payload, size_t n)
{
    if (kSl_DataHdr + n > cap) return 0;
    Hdr(out, kSl_Data);
    Wr64(out + 4, token);
    memcpy(out + kSl_DataHdr, payload, n);
    return kSl_DataHdr + n;
}

size_t Sl_EncodePing(uint8_t *out, uint8_t type, const SlPing_t *p)
{
    Hdr(out, type);
    Wr64(out + 4, p->token);
    Wr32(out + 12, p->stamp);
    out[16] = p->gen;
    out[17] = p->path;
    Wr16(out + 18, p->rtt_ms);
    return kSl_PingLen;
}

size_t Sl_EncodePunch(uint8_t *out, uint8_t type, uint64_t key, uint8_t slot, uint32_t stamp)
{
    Hdr(out, type);
    Wr64(out + 4, key);
    out[12] = slot;
    Wr32(out + 13, stamp);
    return kSl_PunchLen;
}

size_t Sl_EncodeBye(uint8_t *out, uint64_t token)
{
    Hdr(out, kSl_Bye);
    Wr64(out + 4, token);
    return kSl_ByeLen;
}

bool Sl_DecodeWelcome(const uint8_t *b, size_t n, SlWelcome_t *w)
{
    if (Sl_Type(b, n) != kSl_Welcome || n != kSl_WelcomeLen) return false;
    w->nonce = Rd32(b + 4);
    w->token = Rd64(b + 8);
    w->slot = b[16];
    w->max_peers = b[17];
    int k = 0;
    for (; k < kSl_CodeLen && b[18 + k] != 0; k++) w->code[k] = (char)b[18 + k];
    w->code[k] = '\0';
    w->observed = RdEp(b + 24);
    return true;
}

bool Sl_DecodeRefuse(const uint8_t *b, size_t n, uint32_t *nonce, uint8_t *reason)
{
    if (Sl_Type(b, n) != kSl_Refuse || n != kSl_RefuseLen) return false;
    *nonce = Rd32(b + 4);
    *reason = b[8];
    return true;
}

bool Sl_DecodeData(const uint8_t *b, size_t n, uint64_t *token, const uint8_t **payload, size_t *plen)
{
    if (Sl_Type(b, n) != kSl_Data || n < kSl_DataHdr) return false;
    *token = Rd64(b + 4);
    *payload = b + kSl_DataHdr;
    *plen = n - kSl_DataHdr;
    return true;
}

bool Sl_DecodePeer(const uint8_t *b, size_t n, SlPeer_t *p)
{
    if (Sl_Type(b, n) != kSl_Peer || n != kSl_PeerLen) return false;
    p->token = Rd64(b + 4);
    p->gen = b[12];
    p->slot = b[13];
    p->flags = b[14];
    p->p2p_key = Rd64(b + 15);
    p->pub = RdEp(b + 23);
    p->local = RdEp(b + 29);
    return true;
}

bool Sl_DecodePing(const uint8_t *b, size_t n, SlPing_t *p)
{
    const int t = Sl_Type(b, n);
    if ((t != kSl_Ping && t != kSl_Pong) || n != kSl_PingLen) return false;
    p->token = Rd64(b + 4);
    p->stamp = Rd32(b + 12);
    p->gen = b[16];
    p->path = b[17];
    p->rtt_ms = Rd16(b + 18);
    return true;
}

bool Sl_DecodePunch(const uint8_t *b, size_t n, uint64_t *key, uint8_t *slot, uint32_t *stamp)
{
    const int t = Sl_Type(b, n);
    if ((t != kSl_Punch && t != kSl_PunchAck) || n != kSl_PunchLen) return false;
    *key = Rd64(b + 4);
    *slot = b[12];
    *stamp = Rd32(b + 13);
    return true;
}

bool Sl_ValidCode(const char *code)
{
    if (code == NULL || strlen(code) != kSl_CodeLen) return false;
    for (int i = 0; i < kSl_CodeLen; i++) {
        if (strchr(kSl_CodeAlphabet, code[i]) == NULL || code[i] == '\0') return false;
    }
    return true;
}

const char *Relay_RefuseName(uint8_t reason)
{
    switch (reason) {
    case kSl_RBadTicket: return "bad_ticket";
    case kSl_RNoRoom: return "no_room";
    case kSl_RRoomFull: return "room_full";
    case kSl_RRate: return "rate";
    case kSl_RBadPacket: return "bad_packet";
    case kSl_RRelayFull: return "relay_full";
    case kSl_RReplay: return "replay";
    case kSl_RUnknownSession: return "unknown_session";
    case kSl_RKindMismatch: return "kind_mismatch";
    default: return "?";
    }
}

// ---------------------------------------------------------------- client

static void Event(RelayClient_t *r, RelayEvent_t ev, int arg)
{
    if (r->host.event) r->host.event(r->host.ctx, ev, arg);
}

static void Send(RelayClient_t *r, SlEp_t to, const uint8_t *b, size_t n)
{
    if (n > 0 && to.ip != 0 && to.port != 0) r->host.send(r->host.ctx, to, b, n);
}

static uint32_t Stamp(const RelayClient_t *r, uint32_t now) { return now - r->epoch_ms; }

void Relay_Init(RelayClient_t *r, const RelayHost_t *host, uint32_t now_ms)
{
    memset(r, 0, sizeof(*r));
    r->host = *host;
    r->p2p = true;
    r->relay_ping_ms = kRelay_PingFull_ms;
    r->epoch_ms = now_ms;
}

void Relay_SetTicket(RelayClient_t *r, const char *ticket, size_t len, uint32_t now_ms)
{
    if (len == 0 || len > kSl_MaxTicket) return;
    memcpy(r->ticket, ticket, len);
    r->ticket_len = len;
    r->ticket_ms = now_ms;
}

bool Relay_HasTicket(const RelayClient_t *r, uint32_t now_ms)
{
    return r->ticket_len > 0 && (now_ms - r->ticket_ms) < kRelay_TicketMaxAge_ms;
}

static void ClearSeat(RelayClient_t *r)
{
    r->token = 0;
    r->gen = 0;
    r->p2p_key = 0;
    r->peer_present = r->peer_relay_only = false;
    r->have_pub = r->have_local = false;
    r->punching = false;
    r->have_direct = false;
}

static void SendJoin(RelayClient_t *r, uint32_t now)
{
    static uint8_t b[kSl_JoinFixed + kSl_MaxTicket];   // off the (small) task stack
    const size_t n = Sl_EncodeJoin(b, sizeof(b), r->nonce, r->mode, r->code, r->client_id, r->local, r->ticket,
                                   r->ticket_len);
    Send(r, r->relay, b, n);
    r->join_last_ms = now;
    r->stats.joins_sent++;
}

// Begin a JOIN with the ticket in hand, or ask for one.
static void BeginJoin(RelayClient_t *r, uint32_t now)
{
    r->retry_pending = false;
    if (!Relay_HasTicket(r, now)) {
        r->ticket_len = 0;
        r->state = kRelay_NeedTicket;
        Event(r, kRelayEv_NeedTicket, 0);
        return;
    }
    r->state = kRelay_Joining;
    r->nonce = r->host.rand32 ? r->host.rand32(r->host.ctx) : now;
    r->join_start_ms = now;
    SendJoin(r, now);
}

void Relay_Start(RelayClient_t *r, SlEp_t relay, uint8_t mode, const char *code, uint64_t client_id, SlEp_t local,
                 uint32_t now_ms)
{
    if (r->state == kRelay_Seated) Relay_Stop(r);
    ClearSeat(r);
    r->relay = relay;
    r->mode = r->orig_mode = mode;
    memset(r->code, 0, sizeof(r->code));
    if (mode == kSl_ModeJoin && code != NULL) strncpy(r->code, code, kSl_CodeLen);
    r->client_id = client_id;
    r->local = local;
    BeginJoin(r, now_ms);
}

void Relay_SetLocal(RelayClient_t *r, SlEp_t local) { r->local = local; }

void Relay_SetP2p(RelayClient_t *r, bool on)
{
    r->p2p = on;
    if (!on) {
        r->have_direct = false;
        r->punching = false;
    }
}

void Relay_SetLowPower(RelayClient_t *r, bool low) { r->relay_ping_ms = low ? kRelay_PingLow_ms : kRelay_PingFull_ms; }

static void SendRelayPing(RelayClient_t *r, uint32_t now)
{
    uint8_t b[kSl_PingLen];
    const bool direct = Relay_DirectAlive(r, now);
    const SlPing_t p = {r->token, Stamp(r, now), r->gen, direct ? kSl_PathDirect : kSl_PathRelay,
                        direct ? r->direct_rtt_ms : 0};
    Send(r, r->relay, b, Sl_EncodePing(b, kSl_Ping, &p));
    r->last_relay_ping_ms = now;
    r->stats.pings_sent++;
}

void Relay_Resume(RelayClient_t *r, uint32_t now_ms)
{
    if (r->state == kRelay_Seated) {
        SendRelayPing(r, now_ms);
    } else if (r->state == kRelay_Joining) {
        SendJoin(r, now_ms);
    } else if (r->state == kRelay_NeedTicket && Relay_HasTicket(r, now_ms)) {
        BeginJoin(r, now_ms);
    }
}

void Relay_Stop(RelayClient_t *r)
{
    if (r->state == kRelay_Seated && r->token != 0) {
        uint8_t b[kSl_ByeLen];
        Send(r, r->relay, b, Sl_EncodeBye(b, r->token));
    }
    ClearSeat(r);
    r->state = kRelay_Idle;
    r->retry_pending = false;
}

bool Relay_DirectAlive(const RelayClient_t *r, uint32_t now_ms)
{
    return r->p2p && r->have_direct && (now_ms - r->direct_heard_ms) < kRelay_DirectDead_ms;
}

bool Relay_Seated(const RelayClient_t *r) { return r->state == kRelay_Seated; }
bool Relay_PeerPresent(const RelayClient_t *r) { return r->state == kRelay_Seated && r->peer_present; }

static void SetDirect(RelayClient_t *r, SlEp_t from, uint32_t now)
{
    if (r->have_direct && EpEq(r->direct, from)) return;
    // Both candidates answered (two players behind one router: the LAN
    // endpoint and the public one, hairpinned through the router): keep the
    // path we have unless the new one is the partner's LAN endpoint, the
    // shorter way. A dead path is dropped by Relay_Poll first.
    if (r->have_direct && !(r->have_local && EpEq(from, r->peer_local))) return;
    const bool was = r->have_direct;
    r->direct = from;
    r->have_direct = true;
    r->direct_heard_ms = now;
    r->last_direct_ping_ms = now;
    if (!was) {
        r->stats.direct_ups++;
        Event(r, kRelayEv_DirectUp, r->direct_rtt_ms);
    }
}

static void StartPunch(RelayClient_t *r, uint32_t now)
{
    if (!r->p2p || r->peer_relay_only || !r->have_pub || r->p2p_key == 0) return;
    r->punching = true;
    r->punch_until_ms = now + kRelay_PunchFor_ms;
    r->last_punch_ms = now - kRelay_Punch_ms;   // first PUNCH on the next poll
}

static void OnRefuse(RelayClient_t *r, uint32_t nonce, uint8_t reason, uint32_t now)
{
    if (r->state == kRelay_Joining && nonce == r->nonce) {
        r->stats.refusals++;
        r->stats.last_refuse = reason;
        switch (reason) {
        case kSl_RRate:
        case kSl_RRelayFull:
            // Refused before the ticket was spent: same ticket, later.
            r->retry_pending = true;
            r->retry_at_ms = now + (reason == kSl_RRate ? kRelay_RateRetry_ms : 5000);
            r->state = kRelay_NeedTicket;
            return;
        case kSl_RNoRoom:
            if (r->orig_mode != kSl_ModeJoin) {
                // Rejoining a room that died meanwhile: take a new one, as asked.
                r->mode = r->orig_mode;
                memset(r->code, 0, sizeof(r->code));
                r->ticket_len = 0;
                BeginJoin(r, now);
                return;
            }
            // a wrong code is final
            __attribute__((fallthrough));
        case kSl_RRoomFull:
        case kSl_RKindMismatch:
            r->ticket_len = 0;
            r->state = kRelay_Idle;
            Event(r, kRelayEv_Refused, reason);
            return;
        default:
            // bad_ticket / replay / bad_packet: the ticket is no good, get another.
            r->ticket_len = 0;
            BeginJoin(r, now);
            return;
        }
    }
    if (r->state == kRelay_Seated && reason == kSl_RUnknownSession) {
        // The relay forgot our seat (10 min silent, a relay restart): JOIN
        // again with the same client id, which takes the old seat back.
        r->stats.rejoins++;
        ClearSeat(r);
        Event(r, kRelayEv_Rejoin, 0);
        BeginJoin(r, now);
    }
}

bool Relay_OnDatagram(RelayClient_t *r, SlEp_t from, const uint8_t *buf, size_t n, uint32_t now)
{
    const int t = Sl_Type(buf, n);
    if (t < 0) return false;
    const bool from_relay = EpEq(from, r->relay);
    if (from_relay) r->last_relay_heard_ms = now;
    switch (t) {
    case kSl_Welcome: {
        SlWelcome_t w;
        if (!from_relay || r->state != kRelay_Joining || !Sl_DecodeWelcome(buf, n, &w) || w.nonce != r->nonce) break;
        r->ticket_len = 0;   // spent
        r->token = w.token;
        r->slot = w.slot;
        strncpy(r->code, w.code, kSl_CodeLen);
        r->code[kSl_CodeLen] = '\0';
        // Later JOINs (after an unknown_session) rejoin this room.
        r->mode = kSl_ModeJoin;
        r->state = kRelay_Seated;
        r->stats.welcomes++;
        Event(r, kRelayEv_Seated, w.slot);
        SendRelayPing(r, now);   // gen 0 never matches: the relay sends PEERs now
        break;
    }
    case kSl_Refuse: {
        uint32_t nonce;
        uint8_t reason;
        if (from_relay && Sl_DecodeRefuse(buf, n, &nonce, &reason)) OnRefuse(r, nonce, reason, now);
        break;
    }
    case kSl_Data: {
        uint64_t tok;
        const uint8_t *p;
        size_t pl;
        if (r->state != kRelay_Seated || !Sl_DecodeData(buf, n, &tok, &p, &pl)) break;
        if (from_relay && tok == r->token) {
            r->stats.rx_relay++;
            r->host.deliver(r->host.ctx, p, pl, false);
        } else if (!from_relay && r->p2p_key != 0 && tok == r->p2p_key) {
            r->stats.rx_direct++;
            r->direct_heard_ms = now;
            if (!r->have_direct && r->p2p) SetDirect(r, from, now);
            r->host.deliver(r->host.ctx, p, pl, true);
        } else {
            r->stats.unknown++;
        }
        break;
    }
    case kSl_Peer: {
        SlPeer_t p;
        if (!from_relay || r->state != kRelay_Seated || !Sl_DecodePeer(buf, n, &p) || p.token != r->token) break;
        r->gen = p.gen;
        r->p2p_key = p.p2p_key;
        if (!(p.flags & kSl_PeerPresent)) {
            const bool was = r->peer_present;
            r->peer_present = false;
            r->have_pub = r->have_local = false;
            r->have_direct = false;
            r->punching = false;
            if (was) Event(r, kRelayEv_PeerAbsent, 0);
            break;
        }
        const bool was = r->peer_present;
        r->peer_present = true;
        r->peer_relay_only = (p.flags & kSl_PeerRelayOnly) != 0;
        const bool changed = !r->have_pub || !EpEq(r->peer_pub, p.pub);
        r->peer_pub = p.pub;
        r->have_pub = true;
        r->have_local = !EpEq(p.local, p.pub) && p.local.ip != 0;
        r->peer_local = p.local;
        if (!was) Event(r, kRelayEv_PeerPresent, r->peer_relay_only ? 1 : 0);
        if (changed && r->p2p && !r->peer_relay_only) {
            r->have_direct = false;
            StartPunch(r, now);
        }
        break;
    }
    case kSl_Ping:
    case kSl_Pong: {
        SlPing_t p;
        if (r->state != kRelay_Seated || !Sl_DecodePing(buf, n, &p)) break;
        const bool ours = (from_relay && p.token == r->token) || (!from_relay && r->p2p_key != 0 && p.token == r->p2p_key);
        if (!ours) break;
        if (!from_relay) r->direct_heard_ms = now;
        if (t == kSl_Ping) {
            uint8_t b[kSl_PingLen];
            SlPing_t q = p;
            q.path = Relay_DirectAlive(r, now) ? kSl_PathDirect : kSl_PathRelay;
            Send(r, from, b, Sl_EncodePing(b, kSl_Pong, &q));
        } else {
            uint32_t rtt = Stamp(r, now) - p.stamp;
            if (rtt > 60000) rtt = 60000;
            if (from_relay) r->relay_rtt_ms = (uint16_t)rtt;
            else r->direct_rtt_ms = (uint16_t)rtt;
        }
        break;
    }
    case kSl_Punch:
    case kSl_PunchAck: {
        uint64_t key;
        uint8_t slot;
        uint32_t stamp;
        if (from_relay || r->state != kRelay_Seated || !r->p2p || !Sl_DecodePunch(buf, n, &key, &slot, &stamp)) break;
        if (key == 0 || key != r->p2p_key) break;
        r->direct_heard_ms = now;
        if (t == kSl_Punch) {
            uint8_t b[kSl_PunchLen];
            Send(r, from, b, Sl_EncodePunch(b, kSl_PunchAck, key, r->slot, stamp));
        } else {
            uint32_t rtt = Stamp(r, now) - stamp;
            r->direct_rtt_ms = (uint16_t)(rtt > 60000 ? 60000 : rtt);
        }
        SetDirect(r, from, now);
        break;
    }
    default:
        break;
    }
    return true;
}

void Relay_Poll(RelayClient_t *r, uint32_t now)
{
    switch (r->state) {
    case kRelay_Idle:
        return;
    case kRelay_NeedTicket:
        if (r->retry_pending && (int32_t)(now - r->retry_at_ms) >= 0 && Relay_HasTicket(r, now)) BeginJoin(r, now);
        return;
    case kRelay_Joining: {
        const uint32_t every = (now - r->join_start_ms) < kRelay_JoinFast_ms ? kRelay_JoinResend_ms : 1000;
        if (!Relay_HasTicket(r, now)) {
            BeginJoin(r, now);   // the ticket aged out unanswered: a fresh one
        } else if (now - r->join_last_ms >= every) {
            SendJoin(r, now);    // same nonce + ticket: the relay answers a re-sent JOIN idempotently
        }
        return;
    }
    case kRelay_Seated:
        break;
    }
    if (r->punching) {
        if (r->have_direct || (int32_t)(now - r->punch_until_ms) >= 0) {
            r->punching = false;   // done, or failed: stay on the relay
        } else if (now - r->last_punch_ms >= kRelay_Punch_ms) {
            r->last_punch_ms = now;
            uint8_t b[kSl_PunchLen];
            const size_t n = Sl_EncodePunch(b, kSl_Punch, r->p2p_key, r->slot, Stamp(r, now));
            if (r->have_pub) Send(r, r->peer_pub, b, n);
            if (r->have_local) Send(r, r->peer_local, b, n);
            r->stats.punches_sent++;
        }
    }
    if (r->have_direct && (now - r->direct_heard_ms) >= kRelay_DirectDead_ms) {
        r->have_direct = false;   // back to the relay, punch again
        r->stats.direct_downs++;
        Event(r, kRelayEv_DirectDown, 0);
        StartPunch(r, now);
    }
    if (r->have_direct && now - r->last_direct_ping_ms >= kRelay_DirectPing_ms) {
        r->last_direct_ping_ms = now;
        uint8_t b[kSl_PingLen];
        const SlPing_t p = {r->p2p_key, Stamp(r, now), 0, kSl_PathDirect, 0};
        Send(r, r->direct, b, Sl_EncodePing(b, kSl_Ping, &p));
    }
    if (now - r->last_relay_ping_ms >= r->relay_ping_ms) SendRelayPing(r, now);
}

bool Relay_Send(RelayClient_t *r, const uint8_t *payload, size_t n, uint32_t now)
{
    if (r->state != kRelay_Seated || !Sl_IsCwlPayload(payload, n)) return false;
    uint8_t b[kSl_DataHdr + kSl_CwlMax];
    if (Relay_DirectAlive(r, now)) {
        Send(r, r->direct, b, Sl_EncodeData(b, sizeof(b), r->p2p_key, payload, n));
        r->stats.tx_direct++;
    } else {
        Send(r, r->relay, b, Sl_EncodeData(b, sizeof(b), r->token, payload, n));
        r->stats.tx_relay++;
    }
    return true;
}
