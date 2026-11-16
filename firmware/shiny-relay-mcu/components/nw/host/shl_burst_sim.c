// shl_burst_sim.c (lane fwfix2): why a Chromatic sees dozens of duplicate
// items in the first second of a SHLINK room, with no loss and no resends of
// its own (unit 2, 2026-10-10: 64 dups).
//
// The REAL ESP32 client (shl_client.c) against a C port of the relay seat
// (shiny_multiplayer shlink.rs: Arq::poll / on_packet, and run_seat calling
// send() after every event, up to 64 items in flight to the device). Peer
// messages arrive at the relay at 60 Hz plus a join backlog; one-way delay
// D ms; the game pops one message per frame.
//
// Variants of the RELAY (the device is the same in all):
//   A  as deployed: every packet carries every unacked item (up to 32)
//   B  a packet carries only items never sent; the whole window only on RTO
//   C  B + at most 12 items in flight (the device holds 12: kShl_DownItems);
//      printed to show it is NOT a fix (it caps throughput at 12 per RTT)
// Every variant must deliver every message once, in order; the test then
// prints the device's dups_in per variant and checks that B/C cut them.
#include "shl_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---------------------------------------------------------------- a tiny network
enum { kQ = 4096, kPkt = 1100 };
typedef struct { uint32_t at; int to_dev; size_t n; uint8_t b[kPkt]; } Pkt_t;
static Pkt_t _Net[kQ];
static int _NetN;
static uint32_t _D;   // one-way delay
static void NetPut(uint32_t now, int to_dev, const uint8_t *b, size_t n)
{
    if (_NetN >= kQ || n > kPkt) { printf("net overflow\n"); exit(2); }
    Pkt_t *p = &_Net[_NetN++];
    p->at = now + _D;
    p->to_dev = to_dev;
    p->n = n;
    memcpy(p->b, b, n);
}
static int NetTake(uint32_t now, int to_dev, uint8_t *b, size_t *n)
{
    for (int i = 0; i < _NetN; i++) {
        if (_Net[i].to_dev == to_dev && _Net[i].at <= now) {
            memcpy(b, _Net[i].b, _Net[i].n);
            *n = _Net[i].n;
            memmove(&_Net[i], &_Net[i + 1], sizeof(Pkt_t) * (size_t)(_NetN - i - 1));
            _NetN--;
            return 1;
        }
    }
    return 0;
}

static const ShlEp_t RELAY = { 0x7F000001, 5861 };
static uint32_t _Now;
static void DevSend(void *c, ShlEp_t to, const uint8_t *b, size_t n) { (void)c; (void)to; NetPut(_Now, 0, b, n); }
static uint32_t DevRand(void *c) { (void)c; return 0x01020304; }
static void DevNeed(void *c) { (void)c; }

// ---------------------------------------------------------------- the relay seat (port)
enum { kMaxTx = 256, kItemMax = 70, kSdataMaxItems = 32, kSdataBudget = 1000, kRtoInit = 200, kRtoMin = 60, kRtoMax = 2000 };
typedef struct { uint8_t len; uint8_t b[kItemMax]; uint32_t at; } Item_t;   // at: first sent
typedef struct {
    Item_t tx[kMaxTx];
    int tx_n, sent;          // sent: items from tx_base already out at least once (variants B/C)
    uint16_t tx_base, rx_expect;
    int ack_owed, dirty, has_last, front_resent, has_front;
    uint32_t last_send, front_first;
    double srtt; int has_srtt;
    uint32_t rto;
    int resends;
} Arq_t;
static int _Variant;   // 0 A, 1 B, 2 C
static uint64_t kTok = 9;

static void ArqQueue(Arq_t *a, const uint8_t *b, uint8_t n)
{
    if (a->tx_n >= kMaxTx) { printf("relay tx overflow\n"); exit(2); }
    a->tx[a->tx_n].len = n;
    memcpy(a->tx[a->tx_n].b, b, n);
    a->tx_n++;
    a->dirty = 1;
}
static uint32_t Rto(const Arq_t *a)
{
    if (!a->has_srtt) return kRtoInit;
    uint32_t r = (uint32_t)(a->srtt * 2.0) + 30;
    return r < kRtoMin ? kRtoMin : r > kRtoMax ? kRtoMax : r;
}
static void ArqOnPacket(Arq_t *a, uint16_t ack, uint16_t seq, int cnt, uint32_t now)
{
    const int acked = (uint16_t)(ack - a->tx_base);
    if (acked > 0 && acked <= a->tx_n) {
        if (a->has_front && !a->front_resent) {
            const double rtt = now - a->front_first;
            a->srtt = a->has_srtt ? a->srtt * 0.875 + rtt * 0.125 : rtt;
            a->has_srtt = 1;
        }
        memmove(a->tx, a->tx + acked, sizeof(Item_t) * (size_t)(a->tx_n - acked));
        a->tx_n -= acked;
        a->sent = a->sent > acked ? a->sent - acked : 0;
        a->tx_base = ack;
        a->front_resent = 0;
        a->has_front = 0;
        a->rto = Rto(a);
        if (a->tx_n) a->dirty = 1;
    }
    // B/C: the device says it still expects tx_base although tx_base went out
    // more than an RTT ago: it had no room (flow control) or the packet was
    // lost. Go back to tx_base once (the next send repeats the window).
    if (_Variant != 0 && a->tx_n && a->sent > 0 && ack == a->tx_base &&
        now - a->tx[0].at > (a->has_srtt ? (uint32_t)(a->srtt * 1.5) : kRtoInit) + 30) {
        a->sent = 0;
        a->dirty = 1;
        for (int i = 0; i < a->tx_n; i++) a->tx[i].at = now;   // one rewind per RTT, not per ack
    }
    if (cnt) a->ack_owed = 1;
    (void)seq;   // the device sends no messages in this sim (acks only)
}
static void ArqSend(Arq_t *a, uint32_t now)
{
    const int timed_out = a->tx_n && a->has_last && now - a->last_send >= a->rto;
    if (!(a->dirty || a->ack_owed || timed_out)) return;
    if (timed_out && !a->dirty) {
        a->resends++;
        a->front_resent = 1;
        a->rto = a->rto * 2 > kRtoMax ? kRtoMax : a->rto * 2;
    } else if (a->has_front && a->tx_n) {
        a->front_resent = 1;
    }
    // A: from tx_base always. B/C: only what never went out, unless the timer
    // fired (then go back to tx_base, as now).
    int from = (_Variant == 0 || (timed_out && !a->dirty)) ? 0 : a->sent;
    if (from > a->tx_n) from = a->tx_n;
    const int cap = _Variant == 2 ? 12 : kSdataMaxItems + 1000;
    ShlItem_t items[kSdataMaxItems];
    int k = 0, bytes = 0;
    for (int i = from; i < a->tx_n && i < cap; i++) {
        if (k >= kSdataMaxItems || bytes + 1 + a->tx[i].len > kSdataBudget) break;
        bytes += 1 + a->tx[i].len;
        if (i >= a->sent) a->tx[i].at = now;
        items[k].len = a->tx[i].len;
        memcpy(items[k].b, a->tx[i].b, a->tx[i].len);
        k++;
    }
    if (from + k > a->sent) a->sent = from + k;
    if (a->tx_n && !a->has_front) { a->has_front = 1; a->front_first = now; }
    uint8_t pkt[kPkt];
    const size_t n = Shl_EncodeData(pkt, sizeof pkt, kTok, a->rx_expect, (uint16_t)(a->tx_base + from), items, k);
    NetPut(now, 1, pkt, n);
    a->dirty = a->ack_owed = 0;
    a->has_last = 1;
    a->last_send = now;
}

// ---------------------------------------------------------------- one run
typedef struct { unsigned dups, delivered, held, in_order, relay_resends, pkts_to_dev; } Res_t;

static uint32_t _PopMs = 17;   // the game takes one message per frame (a slower game: flow control)
static Res_t Run(int variant, uint32_t delay, int backlog, uint32_t ms)
{
    _Variant = variant;
    _D = delay;
    _NetN = 0;
    static ShlClient_t C;
    static Arq_t R;
    memset(&R, 0, sizeof R);
    R.rto = kRtoInit;
    const ShlHost_t host = { NULL, DevSend, DevRand, DevNeed };
    _Now = 0;
    Shl_Init(&C, &host);
    Shl_Start(&C, RELAY, kShl_ModeHost, "", "", "SHGID1:g:0", "", 2, 0xAB, 0);
    Shl_SetTicket(&C, "a.b.c", 5, 0);
    uint8_t w[16] = { 'S', 'L', 1, kShl_TSWelcome, 1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 9 };
    (void)Shl_OnDatagram(&C, RELAY, w, 16, 0);
    // the relay seat starts: STATE + the join backlog (messages queued while seating)
    uint8_t st[16] = { kShl_IState, kShl_FRoom | kShl_FHost, 1, 0, 0, 2, 'Q', 'R', 'S', '7', '8', '9', 0 };
    ArqQueue(&R, st, 13);
    uint16_t next_msg = 0;
    uint8_t m[4] = { kShl_IMsg, 1, 0, 0 };
    for (int i = 0; i < backlog; i++) { m[2] = (uint8_t)(next_msg >> 8); m[3] = (uint8_t)next_msg++; ArqQueue(&R, m, 4); }
    ArqSend(&R, 0);
    Res_t r = {0};
    r.in_order = 1;
    uint16_t want = 0;
    uint32_t next_peer = 17, next_frame = 17, next_tick = 20;
    for (_Now = 1; _Now <= ms; _Now++) {
        uint8_t b[kPkt];
        size_t n;
        // relay side: the device's packets, the peer's messages, the 20 ms tick: send() after each
        while (NetTake(_Now, 0, b, &n)) {
            if (n >= 17 && b[3] == kShl_TSData) ArqOnPacket(&R, (uint16_t)(b[12] << 8 | b[13]), (uint16_t)(b[14] << 8 | b[15]), b[16], _Now);
            ArqSend(&R, _Now);
        }
        if (_Now >= next_peer) {   // a 60 Hz peer: one message per frame
            next_peer += 17;
            m[2] = (uint8_t)(next_msg >> 8); m[3] = (uint8_t)next_msg++;
            ArqQueue(&R, m, 4);
            ArqSend(&R, _Now);
        }
        if (_Now >= next_tick) { next_tick += 20; ArqSend(&R, _Now); }
        // device side: datagrams, its 5 ms poll, the game's frame
        while (NetTake(_Now, 1, b, &n)) { r.pkts_to_dev++; (void)Shl_OnDatagram(&C, RELAY, b, n, _Now); }
        if (_Now % 5 == 0) Shl_Poll(&C, _Now);
        if (_Now >= next_frame) {
            next_frame += _PopMs;
            uint8_t from, p[80];
            if (Shl_Pop(&C, &from, p) > 0) {
                const uint16_t got = (uint16_t)(p[0] << 8 | p[1]);
                if (got != want) r.in_order = 0;
                want++;
                r.delivered++;
            }
        }
    }
    r.dups = C.stats.dups_in;
    r.held = C.stats.held_back;
    r.relay_resends = (unsigned)R.resends;
    return r;
}

int main(void)
{
    static const char *const kName[] = {"A as deployed", "B new items only", "C B + 12 in flight"};
    for (int d = 0; d < 3; d++) {
        const uint32_t delay = d == 0 ? 25 : d == 1 ? 50 : 120;   // LAN-ish / prod relay p50 / a slow path
        Res_t res[3];
        for (int v = 0; v < 3; v++) {
            res[v] = Run(v, delay, 12, 1000);
            printf("one-way %3u ms  %-20s dups_in %4u  delivered %3u  held_back %3u  pkts to device %3u  relay resends %u\n", delay,
                   kName[v], res[v].dups, res[v].delivered, res[v].held, res[v].pkts_to_dev, res[v].relay_resends);
            CHECK(res[v].in_order, "variant %d delivered out of order", v);
        }
        CHECK(res[0].dups >= 20, "the deployed relay floods duplicates (%u at %u ms)", res[0].dups, delay);
        CHECK(res[1].dups * 4 <= res[0].dups, "B cuts duplicates at least 4x (%u vs %u)", res[1].dups, res[0].dups);
        CHECK(res[1].delivered + 1 >= res[0].delivered, "B delivers as much (%u vs %u)", res[1].delivered, res[0].delivered);
        // C shows why the window must not be capped at the device's 12: at a
        // long RTT 12 items per round trip is below a 60 Hz peer (printed only)
    }
    // a longer run: B/C still deliver everything in order over 10 s
    for (int v = 1; v < 2; v++) {
        const Res_t r = Run(v, 50, 12, 10000);
        CHECK(r.in_order && r.delivered > 550, "variant %d over 10 s: %u delivered, in order %d", v, r.delivered, r.in_order);
    }
    // flow control: a game taking one message every 34 ms from a 60 Hz peer
    // (the device's 12-item queue fills, held_back, nothing acked past it):
    // B must deliver exactly what A delivers, in order (the rewind on a stale ack)
    _PopMs = 34;
    for (int d = 0; d < 2; d++) {
        const uint32_t delay = d ? 120 : 50;
        const Res_t a = Run(0, delay, 12, 5000), b = Run(1, delay, 12, 5000);
        printf("flow control, one-way %3u ms: A dups %u delivered %u held %u | B dups %u delivered %u held %u resends %u\n",
               delay, a.dups, a.delivered, a.held, b.dups, b.delivered, b.held, b.relay_resends);
        CHECK(a.held > 0 && b.held > 0, "the device queue filled (%u, %u)", a.held, b.held);
        CHECK(b.in_order && b.delivered + 2 >= a.delivered, "B under flow control: %u delivered (A %u), in order %d", b.delivered, a.delivered, b.in_order);
        CHECK(b.dups < a.dups, "B fewer dups under flow control (%u vs %u)", b.dups, a.dups);
    }
    _PopMs = 17;
    printf("shl_burst_sim: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
