/* cf_share_test.c: the share protocol (cf_share.c) between two simulated units (lane share1).
 * A simulated radio: per-direction latency, random loss, cuts (both ways, for a time), B's receive queue of CF_SH_QUEUE
 * frames (full: dropped, as the ESP-NOW callback's queue), A's radio taking at most a few frames per millisecond, the
 * channel (B on one, A sweeping), a slow sink (the FPGA flash), B restarted in the middle, frames corrupted. Every run
 * checks what B kept against what A had, byte for byte, and that B let go of a place it did not finish. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cf_share.h"
#include "cf_sha256.h"

uint32_t cf_crc32(uint32_t crc, const uint8_t *b, uint32_t n);

static int _Fails, _Passes;
#define CHECK(c, ...) do { if (c) _Passes++; else { _Fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------------------------------------------------------------- the world */
typedef struct { uint32_t at; uint8_t to_b; uint8_t src[6]; uint8_t n; uint8_t f[CF_SH_FRAME_MAX]; } pkt_t;
#define AIR_MAX 4096
static pkt_t _Air[AIR_MAX];
static int _AirN;
static uint32_t _Now;
static unsigned _Seed = 1;
static unsigned rnd(void) { _Seed = _Seed * 1103515245u + 12345u; return (_Seed >> 16) & 0x7FFF; }

typedef struct {
    double loss;                  /* per frame */
    uint32_t lat_ms;              /* one way */
    uint32_t cut_from, cut_to;    /* nothing gets through in [from, to) */
    uint8_t chan_b;               /* B's channel */
    uint32_t tx_per_ms;           /* frames A's radio takes per millisecond */
    uint32_t sink_us_per_kib;     /* B's place: microseconds per KiB written (the FPGA flash: ~15,000) */
    uint32_t finish_ticks;        /* B's finish says "working" this many times first */
    int corrupt_at;               /* the n-th DATA frame gets a byte flipped (its crc made right again): -1 none */
    int sink_fail_at;             /* B's write fails at this offset (-1 none) */
    uint32_t reset_b_at;          /* B restarts (power) at this time (0 none) and listens again with the same code */
    uint32_t a_cancel_at, b_cancel_at;
    int b_decline;
    int wrong_code;
    unsigned reorder;             /* percent of due pairs swapped (the radio does not; a stress) */
    uint32_t stall_every, stall_ms; /* B's place stops for stall_ms at every stall_every bytes (an erase) */
    uint32_t finish_deaf_ms;      /* m110s: B's finish pauses its radio this long (PLAY NOW: the game loads, nothing in or out) */
    int finish_err;               /* m110s: then says this (a CF_SH_E_*, negated: the game did not start), 0 kept */
} world_t;
static world_t W;

static uint8_t _MacA[6] = { 0x24, 0x0a, 0xc4, 0x11, 0x1a, 0x2b }, _MacB[6] = { 0x24, 0x0a, 0xc4, 0x22, 0x3c, 0x4d };
static uint8_t _ChanA = 1;
static uint32_t _TxThisMs, _TxMs;
static int _DataSeen;

static int air_put(int to_b, const uint8_t src[6], const uint8_t *f, uint32_t n)
{
    if (_AirN >= AIR_MAX) return 0;
    pkt_t *p = &_Air[_AirN++];
    p->at = _Now + W.lat_ms; p->to_b = (uint8_t)to_b; memcpy(p->src, src, 6); p->n = (uint8_t)n; memcpy(p->f, f, n);
    return 0;
}
static int lost_now(void) { return (_Now >= W.cut_from && _Now < W.cut_to) || (W.loss > 0 && (rnd() % 10000) < (unsigned)(W.loss * 10000)); }

/* ---------------------------------------------------------------- A */
static uint8_t *_Src;                /* the stream A sends: game then save */
static uint32_t _SrcN, _SrcReads, _SrcReadBytes;
static uint32_t _SrcNext;            /* each byte asked once, in order */
static int _SrcOrderBad;
static int a_send(void *u, const uint8_t dst[6], const uint8_t *f, uint32_t n)
{
    (void)u; (void)dst;
    if (_TxMs != _Now) { _TxMs = _Now; _TxThisMs = 0; }
    if (_TxThisMs >= W.tx_per_ms) return -1;
    _TxThisMs++;
    uint8_t g[CF_SH_FRAME_MAX];
    memcpy(g, f, n);
    if (g[2] == CF_SH_T_DATA) {
        if (_DataSeen++ == W.corrupt_at && n > 20) {
            g[15] ^= 0x40;
            uint32_t c = cf_crc32(0, g, n - 4);
            g[n - 4] = (uint8_t)c; g[n - 3] = (uint8_t)(c >> 8); g[n - 2] = (uint8_t)(c >> 16); g[n - 1] = (uint8_t)(c >> 24);
        }
    }
    if (_ChanA != W.chan_b || lost_now()) return 0;
    return air_put(1, _MacA, g, n);
}
static void a_chan(void *u, uint8_t ch) { (void)u; _ChanA = ch; }
static int a_read(void *u, uint32_t off, uint8_t *b, uint32_t n)
{
    (void)u;
    if (off != _SrcNext) _SrcOrderBad++;
    if (off + n > _SrcN) return -1;
    memcpy(b, _Src + off, n);
    _SrcNext = off + n;
    _SrcReads++; _SrcReadBytes += n;
    return 0;
}

/* ---------------------------------------------------------------- B */
typedef struct { uint8_t f[CF_SH_FRAME_MAX]; uint8_t n; uint8_t src[6]; } q_t;
static q_t _Q[CF_SH_QUEUE];
static int _Qn, _Qdrop;
static uint8_t *_Dst;
static uint32_t _DstN, _DstHave;
static int _Begun, _Kept, _LetGo, _FinishCalls, _Offers;
static uint8_t _Dest = CF_SH_DEST_LIB;
static uint32_t _BusyUntilUs;         /* the sink's time: B handles nothing until then */
static uint64_t _NowUs;
static int b_send(void *u, const uint8_t dst[6], const uint8_t *f, uint32_t n)
{
    (void)u; (void)dst;
    if (_ChanA != W.chan_b || lost_now()) return 0;
    return air_put(0, _MacB, f, n);
}
static void b_offer(void *u, const cf_sh_offer_t *o, uint16_t unit) { (void)u; (void)o; (void)unit; _Offers++; }
static int _BeginErr;
static int b_begin(void *u, const cf_sh_offer_t *o, uint8_t dest)
{
    (void)u; (void)dest;
    if (_BeginErr) return _BeginErr;
    _Begun++; _DstHave = 0;
    _DstN = o->game_size + o->save_size;
    free(_Dst); _Dst = calloc(1, _DstN + 1);
    return 0;
}
static int b_write(void *u, uint32_t off, const uint8_t *b, uint32_t n)
{
    (void)u;
    if (W.sink_fail_at >= 0 && off + n > (uint32_t)W.sink_fail_at) return -1;
    if (off != _DstHave || off + n > _DstN) return -1;
    memcpy(_Dst + off, b, n);
    _DstHave += n;
    _BusyUntilUs = (uint32_t)(_NowUs + (uint64_t)n * W.sink_us_per_kib / 1024u);
    if (W.stall_every && (off / W.stall_every) != ((off + n) / W.stall_every)) _BusyUntilUs += W.stall_ms * 1000u;
    return 0;
}
static int b_finish(void *u, int ok)
{
    (void)u;
    _FinishCalls++;
    if (!ok) { _LetGo++; return 0; }
    if (W.finish_deaf_ms && !W.cut_to) { W.cut_from = _Now; W.cut_to = _Now + W.finish_deaf_ms; }   /* (the radio paused: both ways) */
    if ((uint32_t)_FinishCalls <= W.finish_ticks) return 1;
    if (W.finish_err) return W.finish_err;
    _Kept++;
    return 0;
}

static cf_sh_tx_t A;
static cf_sh_rx_t B;
static uint8_t _Ring[CF_SH_RING];
static uint8_t *_Seg[CF_SH_NSEG];
static cf_sha256_t _ShaA, _ShaB;
static cf_sh_radio_t _RA = { NULL, a_send, a_chan, NULL }, _RB = { NULL, b_send, NULL, NULL };

static void b_restart(const char *code)
{
    _Qn = 0;
    cf_sh_rx_init(&B, &_RB, &_ShaB, _MacB, _Now);
    B.on_offer = b_offer; B.begin = b_begin; B.write = b_write; B.finish = b_finish;
    cf_sh_rx_start(&B, code, _Now);
}

typedef struct { int a_st, a_err, b_st, b_err; uint32_t ms, a_done_ms; } res_t;
static res_t run(uint32_t game, uint32_t save, uint32_t max_ms)
{
    _AirN = 0; _Now = 1000; _NowUs = 1000000; _ChanA = 1; _DataSeen = 0; _Qn = 0; _Qdrop = 0;
    _Begun = _Kept = _LetGo = _FinishCalls = _Offers = 0; _BusyUntilUs = 0;
    _SrcN = game + save; _SrcNext = 0; _SrcOrderBad = 0; _SrcReads = _SrcReadBytes = 0;
    free(_Src); _Src = malloc(_SrcN);
    for (uint32_t i = 0; i < _SrcN; i++) _Src[i] = (uint8_t)(rnd() ^ (i * 7));
    cf_sh_offer_t o;
    memset(&o, 0, sizeof o);
    o.flags = save ? CF_SH_F_SAVE : 0; o.src = CF_SH_SRC_LIB; o.game_size = game; o.rom_size = game; o.save_size = save;
    o.cart_type = 0x1B; o.cart_ram = 3;
    snprintf(o.title, sizeof o.title, "TEST GAME");
    for (unsigned i = 0; i < CF_SH_NSEG; i++) _Seg[CF_SH_NSEG - 1u - i] = _Ring + i * CF_SH_SEG;   /* (out of order on purpose: pieces need not touch) */
    cf_sh_tx_init(&A, &_RA, a_read, _Seg, &_ShaA, _MacA, 1, 0x1234567u + rnd(), _Now);
    b_restart("4729");
    cf_sh_tx_start(&A, W.wrong_code ? "4728" : "4729", &o, _Now);
    res_t r = { 0 };
    int answered = 0;
    for (; _Now < 1000 + max_ms; _Now++, _NowUs += 1000) {
        /* the air: frames due now */
        for (int i = 0; i < _AirN; ) {
            if (_Air[i].at > _Now) { i++; continue; }
            int pick = i;
            if (W.reorder && i + 1 < _AirN && _Air[i + 1].at <= _Now && (rnd() % 100) < W.reorder) pick = i + 1;   /* (two due frames swapped) */
            pkt_t p = _Air[pick];
            memmove(&_Air[pick], &_Air[pick + 1], sizeof(pkt_t) * (size_t)(_AirN - pick - 1));   /* (in order: the radio does not reorder) */
            _AirN--;
            if (p.to_b) {
                if (_Qn >= (int)CF_SH_QUEUE) { _Qdrop++; continue; }
                memcpy(_Q[_Qn].f, p.f, p.n); _Q[_Qn].n = p.n; memcpy(_Q[_Qn].src, p.src, 6); _Qn++;
            } else cf_sh_tx_frame(&A, p.src, p.f, p.n, _Now);
        }
        /* B's task: its queue in order, unless its sink is busy */
        while (_Qn && _NowUs >= _BusyUntilUs) {
            q_t q = _Q[0];
            memmove(_Q, _Q + 1, sizeof(q_t) * (size_t)(--_Qn));
            cf_sh_rx_frame(&B, q.src, q.f, q.n, _Now);
        }
        if (W.reset_b_at && _Now == W.reset_b_at) b_restart("4729");
        if (B.st == CF_SH_RX_ASK && !answered && _Now - B.t_state > 300) { answered = 1; cf_sh_rx_answer(&B, !W.b_decline, _Dest, _Now); }
        if (W.a_cancel_at && _Now == W.a_cancel_at) cf_sh_tx_cancel(&A, _Now);
        if (W.b_cancel_at && _Now == W.b_cancel_at) cf_sh_rx_cancel(&B, _Now);
        cf_sh_tx_tick(&A, _Now);
        if (_NowUs >= _BusyUntilUs) cf_sh_rx_tick(&B, _Now);
        if (!r.a_done_ms && !cf_sh_tx_busy(&A)) r.a_done_ms = _Now - 1000;
        if (!cf_sh_tx_busy(&A) && !cf_sh_rx_busy(&B) && _Now > 1200) break;
    }
    r.a_st = A.st; r.a_err = A.err; r.b_st = B.st; r.b_err = B.err; r.ms = _Now - 1000;
    return r;
}
static void reset_world(void)
{
    memset(&W, 0, sizeof W);
    W.lat_ms = 1; W.chan_b = 1; W.tx_per_ms = 4; W.corrupt_at = -1; W.sink_fail_at = -1; W.sink_us_per_kib = 2000;
}
static int same(void) { return _DstHave == _SrcN && !memcmp(_Dst, _Src, _SrcN); }

int main(void)
{
    /* ---- frames */
    {
        uint8_t f[CF_SH_FRAME_MAX], t; uint32_t sid, bn; const uint8_t *b;
        const uint8_t body[5] = { 1, 2, 3, 4, 5 };
        uint32_t n = cf_sh_build(f, CF_SH_T_ACK, 0xA1B2C3D4u, body, 5);
        CHECK(n == 17, "frame length %u", n);
        CHECK(cf_sh_parse(f, n, &t, &sid, &b, &bn) && t == CF_SH_T_ACK && sid == 0xA1B2C3D4u && bn == 5 && !memcmp(b, body, 5), "parse");
        f[9] ^= 1; CHECK(!cf_sh_parse(f, n, &t, &sid, &b, &bn), "a flipped bit is refused"); f[9] ^= 1;
        CHECK(!cf_sh_parse(f, n - 1, &t, &sid, &b, &bn), "a short frame is refused");
        f[0] = 'X'; CHECK(!cf_sh_parse(f, n, &t, &sid, &b, &bn), "not ours"); f[0] = 'S';
        f[3] = 2; CHECK(!cf_sh_parse(f, n, &t, &sid, &b, &bn), "another version"); f[3] = 1;
        CHECK(cf_sh_parse(f, n, &t, &sid, &b, &bn), "parse again");
        uint8_t big[300]; memset(big, 7, sizeof big);
        n = cf_sh_build(f, CF_SH_T_DATA, 1, big, 238);
        CHECK(n == CF_SH_FRAME_MAX, "the longest frame is %u", n);
        CHECK(4 + CF_SH_DATA_MAX + 12 == CF_SH_FRAME_MAX, "CF_SH_DATA_MAX fills a frame");
        cf_sh_offer_t o, p;
        memset(&o, 0, sizeof o);
        o.flags = CF_SH_F_PACKED | CF_SH_F_SAVE; o.src = CF_SH_SRC_CART; o.cart_type = 0x10; o.cart_ram = 3; o.game_size = 1084734; o.rom_size = 2097152;
        o.rom_crc32 = 0xDEADBEEF; o.save_size = 32768; snprintf(o.title, sizeof o.title, "PM_CRYSTAL"); snprintf(o.file, sizeof o.file, "Pokemon Crystal.gbc");
        uint8_t ob[CF_SH_OFFER_LEN];
        cf_sh_offer_put(ob, &o);
        CHECK(cf_sh_offer_get(ob, sizeof ob, &p) == 0 && !memcmp(&o, &p, sizeof o), "offer round trip");
        CHECK(cf_sh_offer_get(ob, sizeof ob - 1, &p) != 0, "short offer refused");
        ob[1] = 9; CHECK(cf_sh_offer_get(ob, sizeof ob, &p) != 0, "bad source refused"); ob[1] = 0;
        ob[0] = CF_SH_F_PACKED; CHECK(cf_sh_offer_get(ob, sizeof ob, &p) != 0, "a save size without WITH SAVE refused");
        ob[0] = 3; ob[20] = 0x01; CHECK(cf_sh_offer_get(ob, sizeof ob, &p) == 0 && p.title[0] == '?', "control characters in a title made safe");
        CHECK(cf_sh_code_ok("0042") && !cf_sh_code_ok("42") && !cf_sh_code_ok("12a4") && !cf_sh_code_ok("12345") && !cf_sh_code_ok(NULL), "codes");
        CHECK(cf_sh_tag("4729", _MacB) != cf_sh_tag("4728", _MacB) && cf_sh_tag("4729", _MacB) != cf_sh_tag("4729", _MacA) &&
              cf_sh_tag("4729", _MacB) == cf_sh_tag("4729", _MacB), "tags");
    }
    res_t r;
    /* ---- a clean 1 MiB, then sizes around the frame and ring edges, with and without save */
    reset_world();
    r = run(1u << 20, 0, 600000);
    CHECK(r.a_st == CF_SH_TX_DONE && r.b_st == CF_SH_RX_DONE && same() && _Kept == 1 && _LetGo == 0, "1 MiB clean: a %d/%s b %d/%s", r.a_st, cf_sh_why(r.a_err), r.b_st, cf_sh_why(r.b_err));
    CHECK(!_SrcOrderBad && _SrcReadBytes == _SrcN, "A read every byte once, in order (%u bytes for %u)", _SrcReadBytes, _SrcN);
    printf("1 MiB clean (latency 1 ms, sink 2 ms/KiB): %u ms simulated, %u frames, %u resends, %u naks, queue drops %d\n", r.a_done_ms, A.n_data, A.n_resend, A.n_nak, _Qdrop);
    {
        static const uint32_t sizes[][2] = { { 1, 0 }, { 233, 0 }, { 234, 0 }, { 235, 0 }, { 8191, 1 }, { 8192, 0 }, { 8193, 2048 }, { 32768, 32768 },
                                             { 0x150, 0x2000 }, { 2097152, 32768 }, { 65536 + 17, 131072 + 48 } };
        for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
            reset_world();
            W.sink_us_per_kib = 200;
            r = run(sizes[i][0], sizes[i][1], 900000);
            CHECK(r.a_st == CF_SH_TX_DONE && r.b_st == CF_SH_RX_DONE && same() && _Kept == 1, "size %u + save %u: a %s b %s", sizes[i][0], sizes[i][1], cf_sh_why(r.a_err), cf_sh_why(r.b_err));
            CHECK(!_SrcOrderBad && _SrcReadBytes == _SrcN, "size %u: each byte read once", sizes[i][0]);
        }
    }
    /* ---- the channel sweep: B on 11, A starts on 1 */
    reset_world(); W.chan_b = 11;
    r = run(100000, 0, 120000);
    CHECK(r.a_st == CF_SH_TX_DONE && same(), "B on channel 11 found by the sweep (%s)", cf_sh_why(r.a_err));
    reset_world(); W.chan_b = 13;
    r = run(5000, 0, 120000);
    CHECK(r.a_st == CF_SH_TX_DONE && same(), "B on channel 13 (%s)", cf_sh_why(r.a_err));
    /* ---- loss: 2 %, 10 %, 30 % */
    {
        const double l[3] = { 0.02, 0.10, 0.30 };
        for (int i = 0; i < 3; i++) {
            reset_world(); W.loss = l[i]; _Seed = 77 + i;
            r = run(300000, 32768, 1200000);
            CHECK(r.a_st == CF_SH_TX_DONE && r.b_st == CF_SH_RX_DONE && same() && _Kept == 1, "loss %.0f%%: a %s b %s", l[i] * 100, cf_sh_why(r.a_err), cf_sh_why(r.b_err));
            printf("loss %2.0f%%: 300 KB + 32 KiB in %u ms, %u frames, %u resends, %u naks, %u dups at B, queue drops %d\n", l[i] * 100, r.a_done_ms, A.n_data, A.n_resend, A.n_nak, B.n_dup, _Qdrop);
        }
    }
    reset_world(); W.reorder = 5; _Seed = 99;
    r = run(300000, 1000, 1200000);
    CHECK(r.a_st == CF_SH_TX_DONE && same() && _Kept == 1, "5%% of frames swapped: %s", cf_sh_why(r.a_err));
    printf("reorder 5%%: 300 KB in %u ms, %u naks\n", r.a_done_ms, A.n_nak);
    /* ---- a slow sink (the FPGA flash: 15 ms a KiB): the window keeps B's queue from overflowing */
    reset_world(); W.sink_us_per_kib = 15000;
    r = run(200000, 0, 900000);
    CHECK(r.a_st == CF_SH_TX_DONE && same() && _Qdrop == 0, "slow sink: %s, queue drops %d", cf_sh_why(r.a_err), _Qdrop);
    printf("slow sink 15 ms/KiB: 200 KB in %u ms (%.1f KB/s), %u resends, queue drops %d\n", r.a_done_ms, 200000.0 / r.a_done_ms, A.n_resend, _Qdrop);
    /* ---- the unit's first run (unit 2, the store's sink): a 600 ms stall every 64 KiB; no frame may be dropped */
    reset_world(); W.stall_every = 65536; W.stall_ms = 600; W.sink_us_per_kib = 15000;
    r = run(300000, 0, 900000);
    CHECK(r.a_st == CF_SH_TX_DONE && same() && _Qdrop == 0 && A.n_resend == 0, "600 ms stalls: %s, queue drops %d, resends %u", cf_sh_why(r.a_err), _Qdrop, A.n_resend);
    printf("600 ms stall every 64 KiB: 300 KB in %u ms, queue drops %d, resends %u, naks %u\n", r.a_done_ms, _Qdrop, A.n_resend, A.n_nak);
    /* ---- latency 5 ms */
    reset_world(); W.lat_ms = 5;
    r = run(300000, 0, 900000);
    CHECK(r.a_st == CF_SH_TX_DONE && same(), "latency 5 ms (%s)", cf_sh_why(r.a_err));
    /* ---- cuts: 5 s and 50 s in the middle (resumed), 70 s (both give up, B lets go) */
    {
        const uint32_t cut[2] = { 5000, 50000 };
        for (int i = 0; i < 2; i++) {
            reset_world(); W.sink_us_per_kib = 4000; W.cut_from = 1000 + 2000; W.cut_to = W.cut_from + cut[i];
            r = run(1u << 20, 32768, 1200000);
            CHECK(r.a_st == CF_SH_TX_DONE && r.b_st == CF_SH_RX_DONE && same() && _Kept == 1 && !_LetGo, "a %u s cut: resumed (a %s b %s)", cut[i] / 1000, cf_sh_why(r.a_err), cf_sh_why(r.b_err));
            CHECK(!_SrcOrderBad && _SrcReadBytes == _SrcN, "a %u s cut: no byte read twice", cut[i] / 1000);
            printf("cut %2u s mid-transfer: done in %u ms, %u probes, %u resends\n", cut[i] / 1000, r.a_done_ms, A.n_probe, A.n_resend);
        }
        reset_world(); W.sink_us_per_kib = 4000; W.cut_from = 3000; W.cut_to = 3000 + 70000;
        r = run(1u << 20, 0, 400000);
        CHECK(r.a_st == CF_SH_TX_FAIL && r.a_err == CF_SH_E_LOST && r.b_st == CF_SH_RX_FAIL && r.b_err == CF_SH_E_LOST && _LetGo == 1 && _Kept == 0,
              "a 70 s cut: both LOST, B let go (a %s b %s, let go %d)", cf_sh_why(r.a_err), cf_sh_why(r.b_err), _LetGo);
        /* a cut during the ask (A gone while B's person chooses) */
        reset_world(); W.cut_from = 1000 + 150; W.cut_to = 1000 + 40000;
        r = run(10000, 0, 100000);
        CHECK(r.b_st == CF_SH_RX_FAIL && r.b_err == CF_SH_E_LOST && r.a_st == CF_SH_TX_FAIL, "a cut while B asks: b %s a %s", cf_sh_why(r.b_err), cf_sh_why(r.a_err));
        /* a cut at the end (RESULT lost for a while) */
        reset_world(); W.finish_ticks = 3;
        { r = run(20000, 0, 200000); }
        CHECK(r.a_st == CF_SH_TX_DONE && _Kept == 1 && _FinishCalls == 4, "finish working 3 times then kept (calls %d)", _FinishCalls);
    }
    /* ---- m110s: PLAY NOW. B starts the game before its verdict goes back (its radio paused up to 45 s for the load), so A says
       what happened there: before, A said IT PLAYS ON THAT CHROMATIC NOW while B said IT DID NOT START (m110f, unit 2) */
    {
        reset_world(); _Dest = CF_SH_DEST_PLAY; W.finish_deaf_ms = 45000; W.finish_ticks = 2;
        r = run(262144, 0, 400000);
        CHECK(r.a_st == CF_SH_TX_DONE && r.b_st == CF_SH_RX_DONE && _Kept == 1, "PLAY NOW plays after 45 s deaf: a %d/%s b %d/%s", r.a_st, cf_sh_why(r.a_err), r.b_st, cf_sh_why(r.b_err));
        reset_world(); _Dest = CF_SH_DEST_PLAY; W.finish_deaf_ms = 45000; W.finish_ticks = 2; W.finish_err = -CF_SH_E_START;
        r = run(262144, 0, 400000);
        CHECK(r.a_st == CF_SH_TX_FAIL && r.a_err == CF_SH_E_START && r.b_st == CF_SH_RX_FAIL && r.b_err == CF_SH_E_START && _Kept == 0,
              "PLAY NOW did not start: A told (a %d/%s b %d/%s)", r.a_st, cf_sh_why(r.a_err), r.b_st, cf_sh_why(r.b_err));
        reset_world(); _Dest = CF_SH_DEST_PLAY; W.finish_err = -CF_SH_E_START;
        r = run(262144, 0, 400000);
        CHECK(r.a_err == CF_SH_E_START && r.b_err == CF_SH_E_START, "PLAY NOW did not start at once: a %s b %s", cf_sh_why(r.a_err), cf_sh_why(r.b_err));
        reset_world(); _Dest = CF_SH_DEST_PLAY; W.finish_deaf_ms = 65000; W.finish_ticks = 2;
        r = run(262144, 0, 400000);
        CHECK(r.a_st == CF_SH_TX_FAIL && r.a_err == CF_SH_E_LOST, "a load past CF_SH_LOST_MS deaf: A gives up as LOST (%s): why the pause stops at 45 s", cf_sh_why(r.a_err));
        _Dest = CF_SH_DEST_LIB;
    }
    /* ---- B restarted (power) mid-transfer: A told at once (UNKNOWN), nothing kept */
    reset_world(); W.sink_us_per_kib = 4000; W.reset_b_at = 1000 + 3000;
    r = run(1u << 20, 0, 200000);
    CHECK(r.a_st == CF_SH_TX_FAIL && r.a_err == CF_SH_E_UNKNOWN && _Kept == 0, "B restarted: A stops (%s) in %u ms", cf_sh_why(r.a_err), r.a_done_ms);
    CHECK(r.a_done_ms < 3000 + 3000, "A knew within 3 s (%u ms)", r.a_done_ms);
    /* ---- a byte changed on the way (with a right frame crc): B's sha256 says no, nothing kept, A told */
    reset_world(); W.corrupt_at = 100;
    r = run(100000, 0, 200000);
    CHECK(r.a_st == CF_SH_TX_FAIL && r.a_err == CF_SH_E_CHECK && r.b_st == CF_SH_RX_FAIL && r.b_err == CF_SH_E_CHECK && _Kept == 0 && _LetGo == 1,
          "corrupted: a %s b %s kept %d let go %d", cf_sh_why(r.a_err), cf_sh_why(r.b_err), _Kept, _LetGo);
    reset_world(); W.corrupt_at = 2;     /* (the third frame: bytes 468..599, the save starts at 400) */
    r = run(400, 200, 200000);
    CHECK(r.a_err == CF_SH_E_CHECK && _Kept == 0, "corrupted save: %s", cf_sh_why(r.a_err));
    /* ---- B's place fails mid-way */
    reset_world(); W.sink_fail_at = 50000;
    r = run(100000, 0, 200000);
    CHECK(r.a_st == CF_SH_TX_FAIL && r.a_err == CF_SH_E_WRITE && r.b_err == CF_SH_E_WRITE && _Kept == 0, "B's write failed: a %s b %s", cf_sh_why(r.a_err), cf_sh_why(r.b_err));
    /* ---- the people: wrong code, NO, B stops, A stops */
    reset_world(); W.wrong_code = 1;
    r = run(1000, 0, CF_SH_FIND_MS + 5000);
    CHECK(r.a_st == CF_SH_TX_FAIL && r.a_err == CF_SH_E_NOTFOUND && _Offers == 0, "wrong code: %s, offers %d", cf_sh_why(r.a_err), _Offers);
    reset_world(); _BeginErr = CF_SH_E_ROOM;
    r = run(1000, 0, 60000);
    CHECK(r.a_st == CF_SH_TX_FAIL && r.a_err == CF_SH_E_ROOM && r.b_err == CF_SH_E_ROOM && _Kept == 0, "B's place has no room: a %s b %s", cf_sh_why(r.a_err), cf_sh_why(r.b_err));
    _BeginErr = CF_SH_E_WRITE;
    r = run(1000, 0, 60000);
    CHECK(r.a_err == CF_SH_E_WRITE && r.b_err == CF_SH_E_WRITE, "B's place would not open: a %s b %s", cf_sh_why(r.a_err), cf_sh_why(r.b_err));
    _BeginErr = 0;
    reset_world(); W.b_decline = 1;
    r = run(1000, 0, 60000);
    CHECK(r.a_err == CF_SH_E_DECLINED && r.b_err == CF_SH_E_DECLINED && _Begun == 0, "declined: a %s b %s", cf_sh_why(r.a_err), cf_sh_why(r.b_err));
    reset_world(); W.b_cancel_at = 1000 + 2000; W.sink_us_per_kib = 4000;
    r = run(1u << 20, 0, 60000);
    CHECK(r.a_err == CF_SH_E_STOPPED && r.b_err == CF_SH_E_CANCELLED && _LetGo == 1 && _Kept == 0, "B stopped: a %s b %s let go %d", cf_sh_why(r.a_err), cf_sh_why(r.b_err), _LetGo);
    reset_world(); W.a_cancel_at = 1000 + 2000; W.sink_us_per_kib = 4000;
    r = run(1u << 20, 0, 60000);
    CHECK(r.a_err == CF_SH_E_CANCELLED && r.b_err == CF_SH_E_STOPPED && _LetGo == 1 && _Kept == 0, "A stopped: a %s b %s let go %d", cf_sh_why(r.a_err), cf_sh_why(r.b_err), _LetGo);
    /* ---- a third unit with the same code while B is busy: told BUSY, B's transfer unharmed */
    {
        reset_world(); W.sink_us_per_kib = 1000;
        /* (run by hand: start the transfer, then a HELLO from another MAC) */
        cf_sh_offer_t o; memset(&o, 0, sizeof o); o.game_size = 50000; o.rom_size = 50000; snprintf(o.title, sizeof o.title, "X");
        uint8_t b[6 + CF_SH_OFFER_LEN], f[CF_SH_FRAME_MAX];
        r = run(50000, 0, 1);        /* (sets the world up; B in WAIT) */
        (void)r;
        uint32_t tag = cf_sh_tag("4729", _MacB);
        b[0] = (uint8_t)tag; b[1] = (uint8_t)(tag >> 8); b[2] = (uint8_t)(tag >> 16); b[3] = (uint8_t)(tag >> 24); b[4] = 1; b[5] = 2;
        cf_sh_offer_put(b + 6, &o);
        /* B takes A's offer first */
        for (int i = 0; i < 2000 && B.st != CF_SH_RX_ASK; i++) { _Now++; _NowUs += 1000; cf_sh_tx_tick(&A, _Now); cf_sh_rx_tick(&B, _Now);
            for (int k = 0; k < _AirN; ) { pkt_t p = _Air[k]; if (p.at > _Now) { k++; continue; } memmove(&_Air[k], &_Air[k + 1], sizeof(pkt_t) * (size_t)(_AirN - k - 1)); _AirN--; if (p.to_b) cf_sh_rx_frame(&B, p.src, p.f, p.n, _Now); else cf_sh_tx_frame(&A, p.src, p.f, p.n, _Now); } }
        const uint32_t sid0 = B.sid;
        uint8_t other[6] = { 9, 9, 9, 9, 9, 9 };
        const uint32_t n = cf_sh_build(f, CF_SH_T_HELLO, 0x777, b, sizeof b);
        const int air0 = _AirN;
        cf_sh_rx_frame(&B, other, f, n, _Now);
        CHECK(B.sid == sid0 && B.st == CF_SH_RX_ASK && _AirN == air0 + 1 && _Air[_AirN - 1].f[2] == CF_SH_T_REPLY && _Air[_AirN - 1].f[8] == CF_SH_V_BUSY,
              "a second sender is told BUSY (st %d)", B.st);
    }
    /* ---- the 3 MB container (Emerald Color packed: 2,821,862 bytes) + a 64 KiB save over a lossy, slow link */
    reset_world(); W.loss = 0.05; W.sink_us_per_kib = 15000; W.lat_ms = 2; _Seed = 4242;
    r = run(2821862, 65536, 3000000);
    CHECK(r.a_st == CF_SH_TX_DONE && same() && _Kept == 1, "Emerald-sized 2.8 MB + 64 KiB, 5%% loss, slow sink: %s", cf_sh_why(r.a_err));
    printf("2.8 MB + 64 KiB, 5%% loss, 15 ms/KiB sink: %u ms simulated (%.1f KB/s)\n", r.a_done_ms, 2887398.0 / r.a_done_ms);
    free(_Src); free(_Dst);
    printf("cf_share: %d passed, %d failed\n", _Passes, _Fails);
    return _Fails ? 1 : 0;
}
