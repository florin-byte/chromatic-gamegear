// nw_link.c: see nw_link.h.
#include "nw_link.h"
#include "nw_proto.h"

#include <string.h>

void NwLink_Init(NwLink_t *l, ShlClient_t *c)
{
    memset(l, 0, sizeof(*l));
    l->c = c;
    l->last_ask = -1;
    l->game_room_mode = 0xFF;
}

void NwLink_Detach(NwLink_t *l)
{
    l->attached = false;
    l->last_ask = -1;
    l->want = 0;
}

bool NwLink_Wants(const NwLink_t *l) { return l->attached && l->want != 0; }

static void CopyField(char *dst, size_t cap, const uint8_t *src, uint8_t n)
{
    if (n >= cap) n = (uint8_t)(cap - 1);
    for (uint8_t i = 0; i < n; i++) dst[i] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : '?';
    dst[n] = '\0';
}

static uint8_t UpFree(const NwLink_t *l)
{
    const ShlClient_t *c = l->c;
    if (Shl_InRoom(c) && c->peers > 0) return Shl_TxFree(c);
    return kNwLink_OfflineFree;
}

uint8_t NwLink_Op(NwLink_t *l, uint8_t op, const uint8_t *b, uint16_t blen, uint8_t *out, uint16_t cap,
                  uint16_t *out_len, uint32_t now_ms)
{
    ShlClient_t *c = l->c;
    *out_len = 0;
    // 1.0.2: the game's own online screens
    if (op == kNwLink_OpStatus) {
        if (cap < 17 + kNwLink_PairCodeMax) return NW_S_BAD_REQUEST;
        NwLinkDev_t dev;
        memset(&dev, 0, sizeof dev);
        if (l->on_status) l->on_status(l->ctx, &dev);
        memset(out, 0, 17);
        out[0] = kNwLink_StatusVer;
        out[1] = (uint8_t)((dev.flags & (kNwLink_SOnline | kNwLink_SWifiSaved | kNwLink_SPaired)) |
                           (l->attached ? kNwLink_SAttached : 0));
        if (c) {
            out[2] = (uint8_t)c->state;
            out[3] = c->state == kShl_Ended ? c->ended_reason : 0;
            out[4] = Shl_InRoom(c) ? 1 : 0;
            out[5] = Shl_InRoom(c) && c->is_host ? 1 : 0;
            out[6] = Shl_InRoom(c) ? c->peers : 0;
            out[7] = Shl_InRoom(c) ? c->max_peers : 0;
            if (Shl_InRoom(c)) memcpy(out + 8, c->room_code, 6);
            out[14] = c->state == kShl_Idle && l->game_room_mode == 0xFF ? 0xFF : c->asked_mode;
        } else {
            out[14] = 0xFF;
        }
        out[15] = dev.pair_state;
        uint8_t n = 0;
        while (n < kNwLink_PairCodeMax && dev.pair_code[n]) n++;
        out[16] = n;
        memcpy(out + 17, dev.pair_code, n);
        *out_len = (uint16_t)(17 + n);
        l->statuses++;
        return NW_S_OK;
    }
    if (op == kNwLink_OpUi) {
        if (blen < 1) return NW_S_BAD_REQUEST;
        if (!l->on_ui) return NW_S_UNSUPPORTED;
        l->uis++;
        l->on_ui(l->ctx, b[0]);
        return NW_S_OK;
    }
    if (op == kNwLink_OpRoom) {
        if (blen < 2 || 2u + b[1] > blen) return NW_S_BAD_REQUEST;
        if (!l->on_room) return NW_S_UNSUPPORTED;
        if (!l->attached) return NW_S_NOT_IDENTIFIED;
        const uint8_t mode = b[0];
        if (mode > kShl_ModeRoom && mode != kNwLink_RoomLeave) return NW_S_BAD_REQUEST;
        char arg[kShl_RoomIdMax + 1];
        CopyField(arg, sizeof arg, b + 2, b[1]);
        l->game_room_mode = mode;
        l->rooms++;
        l->on_room(l->ctx, mode, arg);
        if (cap < 1) return NW_S_OK;
        out[0] = 1;
        *out_len = 1;
        return NW_S_OK;
    }
    if (op == kNwLink_OpJoin) {
        if (blen < 5 || cap < 2) return NW_S_BAD_REQUEST;
        const uint8_t il = b[3];
        if (4u + il + 1u > blen) return NW_S_BAD_REQUEST;
        const uint8_t fl = b[4 + il];
        if (4u + il + 1u + fl > blen) return NW_S_BAD_REQUEST;
        char ident[kShl_IdentMax + 1], fam[kShl_FamMax + 1];
        CopyField(ident, sizeof ident, b + 4, il);
        CopyField(fam, sizeof fam, b + 5 + il, fl);
        // Another game: the room this one does not play is left behind.
        if (l->ident[0] && strcmp(ident, l->ident) != 0 && c->state != kShl_Idle) Shl_Leave(c);
        if (strcmp(ident, l->ident) != 0) l->game_room_mode = 0xFF;   // 1.0.2: another game chooses its own
        memcpy(l->ident, ident, sizeof ident);
        memcpy(l->fam, fam, sizeof fam);
        l->slot_size = b[0];
        l->maxp = b[1];
        l->attached = true;
        l->last_ask = -1;
        // As CartridgeLink on attach: the session moves, the game starts over.
        l->session = (uint8_t)(l->session + 1);
        l->seen_gen = c->seat_gen;
        l->joins++;
        // mode bit 7: the game's room ask arrives WITH this join. The ask baseline
        // was just reset (the next XCHG's ask is taken as-is), so this is the only
        // place it can be heard: the same rule as an ask move, no room live.
        l->join_mode = b[2];
        if ((b[2] & kNwLink_JoinOpenMenu) && !Shl_InRoom(c) && l->on_ask) {
            l->join_asks++;
            l->on_ask(l->ctx);
        }
        out[0] = (uint8_t)c->state;
        out[1] = UpFree(l);
        *out_len = 2;
        return NW_S_OK;
    }
    if (op == kNwLink_OpLeave) {
        if (c->state != kShl_Idle) Shl_Leave(c);
        return NW_S_OK;
    }
    if (op != kNwLink_OpXchg) return NW_S_UNSUPPORTED;
    if (!l->attached) return NW_S_NOT_IDENTIFIED;
    if (blen < 5 || cap < 7) return NW_S_BAD_REQUEST;
    l->xchgs++;
    l->last_xchg_ms = now_ms;
    const uint8_t ack = b[0], want = b[1], ask = b[2], in_free = b[3], n = b[4];
    const bool live = Shl_InRoom(c);
    // a new room: the session moves (and nothing of the old one goes in)
    if (live && c->seat_gen != l->seen_gen) {
        l->seen_gen = c->seat_gen;
        l->session = (uint8_t)(l->session + 1);
    }
    const bool adopted = ack == l->session;
    // the surface: lk_ask moved while no room is live -> the room menu
    if (l->last_ask < 0) l->last_ask = ask;
    else if (ask != (uint8_t)l->last_ask) {
        l->last_ask = ask;
        if (!live && l->on_ask) l->on_ask(l->ctx);
    }
    if (l->want && !want && l->on_unwant) l->on_unwant(l->ctx);
    l->want = want;
    // out: what the game pushed
    uint16_t at = 5;
    for (uint8_t k = 0; k < n; k++) {
        if (at >= blen) return NW_S_BAD_REQUEST;
        const uint8_t m = b[at];
        if (m == 0 || at + 1u + m > blen) return NW_S_BAD_REQUEST;
        if (live && c->peers > 0 && adopted && m <= kShl_MsgMax) {
            if (Shl_Push(c, b + at + 1, m)) l->msgs_up++;
            else l->lost = (uint8_t)(l->lost + 1);
        } else if (live && !adopted) {
            l->stale++;
        } else {
            l->dropped_offline++;
        }
        at = (uint16_t)(at + 1 + m);
    }
    // the answer
    uint8_t link = kNwLink_App;
    if (live) {
        link |= kNwLink_Room;
        if (c->is_host) link |= kNwLink_Host;
        if (c->peers > 0) link |= kNwLink_Peer;
    }
    out[0] = link;
    out[1] = live ? c->peers : 0;
    out[2] = live ? c->seat : 0;
    out[3] = l->session;
    out[4] = l->lost;
    out[5] = UpFree(l);
    uint16_t o = 7;
    uint8_t sent = 0;
    if (adopted) {
        uint8_t tmp[kShl_MsgMax];
        while (sent < in_free && Shl_DownCount(c) > 0) {
            // the next one must fit in the answer
            const ShlItem_t *it = &c->down[c->down_head];
            const uint16_t need = (uint16_t)(2 + (it->len - 1));
            if (o + need > cap) break;
            uint8_t from = 0;
            const uint8_t m = Shl_Pop(c, &from, tmp);
            out[o] = m;
            out[o + 1] = from;
            memcpy(out + o + 2, tmp, m);
            o = (uint16_t)(o + 2 + m);
            sent++;
            l->msgs_down++;
        }
    }
    out[6] = sent;
    *out_len = o;
    return NW_S_OK;
}

// 1.0.3: XCHG and JOIN with no room client (receive mode frees it: its heap goes to the TLS calls), answered as
// the idle client would: no room, the game's pushed messages dropped, nothing down. Every PARTY BATTLE build
// (v3-v11) takes a JOIN that is not OK, and an XCHG answered NOT_IDENTIFIED leads to a JOIN, as "not a
// Chromatic" (its window and no-wait cable driver off until a GB reset): so these never fail while the client
// is away. The room calls themselves stay refused by the hold (nw_dev ShlTick).
uint8_t NwLink_OpNoClient(NwLink_t *l, uint8_t op, const uint8_t *b, uint16_t blen, uint8_t *out, uint16_t cap,
                          uint16_t *out_len, uint32_t now_ms)
{
    *out_len = 0;
    if (op == kNwLink_OpJoin) {
        if (blen < 5 || cap < 2) return NW_S_BAD_REQUEST;
        const uint8_t il = b[3];
        if (4u + il + 1u > blen) return NW_S_BAD_REQUEST;
        const uint8_t fl = b[4 + il];
        if (4u + il + 1u + fl > blen) return NW_S_BAD_REQUEST;
        char ident[kShl_IdentMax + 1], fam[kShl_FamMax + 1];
        CopyField(ident, sizeof ident, b + 4, il);
        CopyField(fam, sizeof fam, b + 5 + il, fl);
        if (strcmp(ident, l->ident) != 0) l->game_room_mode = 0xFF;
        memcpy(l->ident, ident, sizeof ident);
        memcpy(l->fam, fam, sizeof fam);
        l->slot_size = b[0];
        l->maxp = b[1];
        l->attached = true;
        l->last_ask = -1;
        l->session = (uint8_t)(l->session + 1);
        l->joins++;
        l->join_mode = b[2];
        out[0] = (uint8_t)kShl_Idle;
        out[1] = kNwLink_OfflineFree;
        *out_len = 2;
        return NW_S_OK;
    }
    if (op != kNwLink_OpXchg) return NW_S_UNSUPPORTED;
    if (!l->attached) return NW_S_NOT_IDENTIFIED;
    if (blen < 5 || cap < 7) return NW_S_BAD_REQUEST;
    l->xchgs++;
    l->last_xchg_ms = now_ms;
    const uint8_t want = b[1], ask = b[2], n = b[4];
    if (l->last_ask < 0) l->last_ask = ask;
    else if (ask != (uint8_t)l->last_ask) l->last_ask = ask;   // (no room menu while the client is away)
    l->want = want;
    uint16_t at = 5;
    for (uint8_t k = 0; k < n; k++) {
        if (at >= blen) return NW_S_BAD_REQUEST;
        const uint8_t m = b[at];
        if (m == 0 || at + 1u + m > blen) return NW_S_BAD_REQUEST;
        l->dropped_offline++;
        at = (uint16_t)(at + 1 + m);
    }
    out[0] = kNwLink_App;
    out[1] = 0;
    out[2] = 0;
    out[3] = l->session;
    out[4] = l->lost;
    out[5] = kNwLink_OfflineFree;
    out[6] = 0;
    *out_len = 7;
    return NW_S_OK;
}
