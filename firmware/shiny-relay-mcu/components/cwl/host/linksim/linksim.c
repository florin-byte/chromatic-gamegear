// linksim.c: two Game Boys (SameBoy) linked through the CWL2 core
// (../../cwl_proto.c, the firmware's own file) in simulated time.
//
// Each side is a Chromatic: the FPGA link port (link.v: held masters, write
// barrier, stop-then-restart absorb, fast-clock stall, standing offers, and
// the send-ahead slave lock proposed for FPGA 18.22), the FPGA <-> ESP32 UART
// (6-byte packets at 115200 baud, the ESP32's RX timeout), the ESP32 link
// task (one pass per FreeRTOS tick as in 0.17.2, or event-driven as with
// linkperf1's change A) and the radio (one-way latency, jitter, loss,
// optional reordering). Nothing runs in real time: a 5 minute match takes
// seconds, and the same seed gives the same run.
//
// Checks: every exchange is logged at both ports (role, byte sent, byte got);
// the two logs must mirror each other exactly (what a cable guarantees).
// Scripted presses are checked against the game's own joypad reads.
//
//   linksim ROM_A STATE_A ROM_B STATE_B [options]   (STATE "-" = boot)
//   --ms N | --frames N         length (wall time)
//   --rtt MS --jitter MS --loss PCT --reorder --seed N
//   --esp base|a                ESP32 timing model (default base)
//   --sa | --sa-a | --sa-b      send-ahead on both / one side
//   --nolock-a | --nolock-b     that side's FPGA cannot lock (speaks it, never lends)
//   --cable                     ideal link cable instead (reference)
//   --keys-a F:KEY/N,...        presses at wall frame F held N frames
//   --player-a SEED:F0:F1:EVERY:HOLD:k1|k2|..  a press every EVERY frames
//   --shot PREFIX [--shot-every F]   PPMs (PREFIX_a.ppm at the end)
//   --save-a S --save-b S       save states at the end
//   --watch-a ADDR --watch-b ADDR   print a RAM byte when it changes
//   --peek-end ADDR:LEN         dump RAM of both at the end
//   --trace                     port + core events to stderr
#include "cwl_proto.h"

#include <Core/gb.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef BOOTROM_DIR
#define BOOTROM_DIR "../../third_party/SameBoy/build/bin/BootROMs"
#endif

#define TPMS 8388.608                       // GB_run ticks (8 MHz units) per ms
#define FRAME_MS (70224.0 / 4194.304)       // 16.7427 ms

// ---------------------------------------------------------------- config

static struct {
    double rtt, jitter, loss;
    int reorder;
    int esp_a;                 // event-driven ESP32 link task + short UART RX timeout
    int sa[2], nolock[2];
    int cable;
    int trace;
    int standing, absorb, fast_stall;
    uint32_t timeout_ms;
    double uart_byte_ms;       // 10 bits at 115200
    double rx_tout_ms;         // ESP32 UART RX timeout after the last byte
    double pass_ms;            // base: one link-task pass per tick
    double wake_ms;            // a: event to processing
} C = {
    .rtt = 2, .jitter = 0, .loss = 0, .standing = 1, .absorb = 1, .fast_stall = 1,
    .timeout_ms = 1500, .uart_byte_ms = 10.0 / 115.2, .pass_ms = 1.0, .wake_ms = 0.05,
};

static uint64_t rng = 88172645463325252ull;
static double urand(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (double)(rng >> 11) / 9007199254740992.0;
}

// ---------------------------------------------------------------- events

enum { E_UP = 1, E_DOWN, E_NET, E_PASS, E_POLL, E_KEY, E_UPX, E_NETX, E_SCRIPT };
typedef struct { double t; uint64_t n; int kind, side; uint8_t a, b, c; uint8_t pkt[kCwlPacketLen]; } Ev;
static Ev *H;
static int Hn, Hcap;
static uint64_t Hseq;

static int ev_less(const Ev *x, const Ev *y) { return x->t < y->t || (x->t == y->t && x->n < y->n); }
static void hpush(Ev e)
{
    if (Hn == Hcap) { Hcap = Hcap ? Hcap * 2 : 4096; H = realloc(H, sizeof(Ev) * (size_t)Hcap); }
    e.n = Hseq++;
    int i = Hn++;
    while (i > 0) {
        const int p = (i - 1) / 2;
        if (!ev_less(&e, &H[p])) break;
        H[i] = H[p];
        i = p;
    }
    H[i] = e;
}
static Ev hpop(void)
{
    Ev top = H[0], last = H[--Hn];
    int i = 0;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= Hn) break;
        if (c + 1 < Hn && ev_less(&H[c + 1], &H[c])) c++;
        if (!ev_less(&H[c], &last)) break;
        H[i] = H[c];
        i = c;
    }
    if (Hn > 0) H[i] = last;
    return top;
}

static double now;   // simulated ms

// ---------------------------------------------------------------- sides

enum { ST_NONE, ST_BARRIER, ST_FAST, ST_LOCK };
enum { FE_ARMED = 1, FE_DISARM, FE_MASTER, FE_SLAVE_DONE, FE_SLAVE_NAK, FE_ABANDON, FE_NAK_STALLED, FE_LOCKED, FE_LOCK_NAK, FE_SLAVE_ABANDON };
enum { FC_REPLY = 1, FC_XFER = 2, FC_DROP = 4, FC_WITHDRAW = 6, FC_LOCK = 7, FC_UNLOCK = 8 };

typedef struct { double down, up; int key; int seen; double seen_t; } Press;

typedef struct Side {
    int id;
    GB_gameboy_t *gb;
    uint32_t px[256 * 224];
    double t;                 // this Game Boy's emulated time
    uint64_t frames;
    // FPGA port
    int held, fast, in_write, offer_pend, locked;
    uint8_t offer_byte, held_sb;
    double held_t0;
    int stalled;
    double stall_t0;
    int park_valid;
    uint16_t park_addr;
    uint8_t park_val;
    // UART + ESP32
    double up_busy, down_busy;
    CwlState_t cwl;
    uint32_t last_timeouts;
    Ev upq[256]; int upq_n;    // base: FPGA events waiting for the next pass
    Ev rxq[256]; int rxq_n;    // base: packets waiting for the next pass
    uint8_t txb[256][kCwlPacketLen]; int txb_n;
    int flushing;              // base: packets sent now (the pass's flush), else at the next pass
    double net_last;           // in-order radio: last arrival time
    // cable
    bool cable_bit;
    // logs
    uint32_t *log; int log_n, log_cap;         // role << 16 | sent << 8 | got
    // stats
    uint32_t holds, hold_b[5], noexch, safety, absorbed, restarts;
    double hold_sum, *hold_v; int hold_vn, hold_vcap;
    uint32_t stalls[4]; double stall_ms[4], stall_max;
    float *frame_stall; int frame_n;
    uint32_t presses_n; Press *presses; int presses_cap;
    uint32_t joyp_reads;
    int watch; uint8_t watch_v; int watch_init;
    double quiet_since; int quiet_n;
    uint32_t serial_master_starts;
} Side;
static Side S[2];
static Side *side_of(GB_gameboy_t *gb) { return (Side *)GB_get_user_data(gb); }
static uint8_t *io(Side *s) { return GB_get_direct_access(s->gb, GB_DIRECT_ACCESS_IO, NULL, NULL); }

static void tr(Side *s, const char *fmt, ...)
{
    if (!C.trace) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%10.3f %c ", now, 'A' + s->id);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void log_x(Side *s, int role, uint8_t sent, uint8_t got)
{
    if (s->log_n == s->log_cap) { s->log_cap = s->log_cap ? s->log_cap * 2 : 4096; s->log = realloc(s->log, sizeof(uint32_t) * (size_t)s->log_cap); }
    s->log[s->log_n++] = (uint32_t)role << 16 | (uint32_t)sent << 8 | got;
}

static int bucket(double ms) { return ms < 1 ? 0 : ms < 8 ? 1 : ms < 17 ? 2 : ms < 33 ? 3 : 4; }

static void hold_done(Side *s)
{
    const double ms = now - s->held_t0;
    s->holds++;
    s->hold_b[bucket(ms)]++;
    s->hold_sum += ms;
    if (s->hold_vn == s->hold_vcap) { s->hold_vcap = s->hold_vcap ? s->hold_vcap * 2 : 4096; s->hold_v = realloc(s->hold_v, sizeof(double) * (size_t)s->hold_vcap); }
    s->hold_v[s->hold_vn++] = ms;
}

static void frame_stall_add(Side *s, double a, double b)
{
    while (a < b) {
        const int idx = (int)(a / FRAME_MS);
        const double wend = (idx + 1) * FRAME_MS;
        const double e = b < wend ? b : wend;
        if (idx >= 0 && idx < s->frame_n) s->frame_stall[idx] += (float)(e - a);
        a = e;
    }
}

// ---- UART

static void fpga_event(Side *s, int ev, uint8_t byte)
{
    tr(s, "FPGA ev %d %02x\n", ev, byte);
    const double done = (s->up_busy > now ? s->up_busy : now) + 6 * C.uart_byte_ms;
    s->up_busy = done;
    Ev e = {.t = done + C.rx_tout_ms, .kind = E_UP, .side = s->id, .a = (uint8_t)ev, .b = byte};
    hpush(e);
}

static void esp_cmd(Side *s, int cmd, uint8_t byte, uint8_t flag)
{
    const double done = (s->down_busy > now ? s->down_busy : now) + 6 * C.uart_byte_ms;
    s->down_busy = done;
    Ev e = {.t = done, .kind = E_DOWN, .side = s->id, .a = (uint8_t)cmd, .b = byte, .c = flag};
    hpush(e);
}

// ---- port model (link.v)

static int port_write(Side *s, uint16_t addr, uint8_t v);

static void stall(Side *s, int kind)
{
    s->stalled = kind;
    s->stall_t0 = now;
}

static void unstall(Side *s)
{
    if (!s->stalled) return;
    const double ms = now - s->stall_t0;
    s->stalls[s->stalled]++;
    s->stall_ms[s->stalled] += ms;
    if (ms > s->stall_max) s->stall_max = ms;
    frame_stall_add(s, s->stall_t0, now);
    s->stalled = ST_NONE;
    if (s->t < now) s->t = now;
}

static void raw_write(Side *s, uint16_t addr, uint8_t v)
{
    s->in_write = 1;
    GB_write_memory(s->gb, addr, v);
    s->in_write = 0;
}

static void apply_parked(Side *s)
{
    if (!s->park_valid) return;
    s->park_valid = 0;
    tr(s, "PARKED %s=%02x lands\n", s->park_addr == 0xFF01 ? "SB" : "SC", s->park_val);
    if (port_write(s, s->park_addr, s->park_val)) raw_write(s, s->park_addr, s->park_val);
}

static void park(Side *s, uint16_t addr, uint8_t v, int kind)
{
    s->park_valid = 1;
    s->park_addr = addr;
    s->park_val = v;
    stall(s, kind);
}

static void complete(Side *s, uint8_t b)
{
    uint8_t *r = io(s);
    r[0x01] = b;
    r[0x02] &= (uint8_t)~0x80;
    r[0x0F] |= 8;
}

// The game writes SB / SC: returns 1 if SameBoy should perform the write.
static int port_write(Side *s, uint16_t addr, uint8_t v)
{
    uint8_t *r = io(s);
    if (s->locked) {
        // send-ahead lock (FPGA 18.22 proposal): the byte is lent to the
        // master; any SB/SC write waits for the master's verdict.
        tr(s, "W %s=%02x under the lock: parked\n", addr == 0xFF01 ? "SB" : "SC", v);
        park(s, addr, v, ST_LOCK);
        fpga_event(s, FE_SLAVE_ABANDON, r[0x01]);
        return 0;
    }
    if (addr == 0xFF01 && s->held) {
        tr(s, "W SB=%02x under held master: parked\n", v);
        park(s, addr, v, ST_BARRIER);
        fpga_event(s, FE_ABANDON, r[0x01]);
        return 0;
    }
    const int armed_slave = !s->held && (r[0x02] & 0x81) == 0x80;
    if (addr == 0xFF01) {
        if (armed_slave) fpga_event(s, FE_ARMED, v);
        return 1;
    }
    if (s->held && C.absorb && (v & 0x81) == 0x01) {
        s->absorbed++;
        return 0;
    }
    if (s->held && (v & 0x81) != 0x81) {
        tr(s, "W SC=%02x under held master: parked\n", v);
        park(s, addr, v, ST_BARRIER);
        fpga_event(s, FE_ABANDON, r[0x01]);
        return 0;
    }
    const uint8_t sb = r[0x01];
    if (s->held) {   // SC = 0x81 again: continues the held transfer
        s->restarts++;
        raw_write(s, 0xFF02, (uint8_t)(v & ~1));
        return 0;
    }
    if ((v & 0x81) == 0x81) {
        raw_write(s, 0xFF02, (uint8_t)(v & ~1));   // external clock: SameBoy never clocks it
        s->held = 1;
        s->held_t0 = now;
        s->held_sb = sb;
        s->serial_master_starts++;
        fpga_event(s, FE_MASTER, sb);
        if ((v & 0x02) && GB_is_cgb(s->gb) && C.fast_stall) {
            s->fast = 1;
            stall(s, ST_FAST);
        }
        return 0;
    }
    if ((v & 0x81) == 0x80 && s->offer_pend) {
        raw_write(s, 0xFF02, v);
        s->offer_pend = 0;
        log_x(s, 'S', sb, s->offer_byte);
        complete(s, s->offer_byte);
        tr(s, "standing exchange sent %02x got %02x\n", sb, s->offer_byte);
        fpga_event(s, FE_SLAVE_DONE, sb);
        return 0;
    }
    if (v & 0x80) fpga_event(s, FE_ARMED, sb);
    else if (armed_slave) fpga_event(s, FE_DISARM, sb);
    return 1;
}

static void master_released(Side *s)
{
    s->held = 0;
    s->fast = 0;
    hold_done(s);
    if (s->stalled == ST_FAST || s->stalled == ST_BARRIER) {
        unstall(s);
        apply_parked(s);
    }
}

static void fpga_cmd(Side *s, int cmd, uint8_t b, uint8_t flag)
{
    uint8_t *r = io(s);
    switch (cmd) {
    case FC_REPLY:
        if (!s->held) { tr(s, "REPLY %02x dropped (not held)\n", b); return; }
        tr(s, "REPLY %02x%s (%.2f ms)\n", b, flag ? " (no exchange)" : "", now - s->held_t0);
        if (flag) s->noexch++;
        else log_x(s, 'M', s->held_sb, b);
        complete(s, b);
        master_released(s);
        break;
    case FC_DROP:
        if (!s->held) return;
        tr(s, "DROP (%.2f ms)\n", now - s->held_t0);
        s->noexch++;
        r[0x02] &= (uint8_t)~0x80;
        master_released(s);
        break;
    case FC_XFER: {
        const int armed = !s->held && (r[0x02] & 0x81) == 0x80;
        if (armed) {
            const uint8_t sent = r[0x01];
            log_x(s, 'S', sent, b);
            complete(s, b);
            tr(s, "XFER exchanged sent %02x got %02x%s\n", sent, b, s->locked ? " (locked)" : "");
            fpga_event(s, FE_SLAVE_DONE, sent);
            if (s->locked) {
                s->locked = 0;
                if (s->stalled == ST_LOCK) { unstall(s); apply_parked(s); }
            }
            return;
        }
        if (C.standing) {
            s->offer_pend = 1;
            s->offer_byte = b;
            if (s->held && s->fast) fpga_event(s, FE_NAK_STALLED, r[0x01]);
            return;
        }
        fpga_event(s, (s->held && s->fast) ? FE_NAK_STALLED : FE_SLAVE_NAK, r[0x01]);
        break;
    }
    case FC_WITHDRAW:
        if (!s->offer_pend) return;
        s->offer_pend = 0;
        fpga_event(s, FE_SLAVE_NAK, r[0x01]);
        break;
    case FC_LOCK: {
        const int armed = !s->held && (r[0x02] & 0x81) == 0x80;
        if (armed && !s->locked && !s->stalled) {
            s->locked = 1;
            fpga_event(s, FE_LOCKED, r[0x01]);
        } else {
            fpga_event(s, FE_LOCK_NAK, r[0x01]);
        }
        break;
    }
    case FC_UNLOCK:
        if (!s->locked) return;
        s->locked = 0;
        if (s->stalled == ST_LOCK) { unstall(s); apply_parked(s); }
        break;
    }
}

// ---- ESP32 host callbacks

static void net_send(Side *s, const uint8_t *pkt)
{
    if (C.loss > 0 && urand() * 100 < C.loss) return;
    double t = now + C.rtt / 2 + (C.jitter > 0 ? urand() * C.jitter : 0);
    if (!C.reorder) {
        if (t < s->net_last) t = s->net_last;
        s->net_last = t;
    }
    Ev e = {.t = t, .kind = E_NET, .side = 1 - s->id};
    memcpy(e.pkt, pkt, kCwlPacketLen);
    hpush(e);
}

static void flush(Side *s)
{
    for (int i = 0; i < s->txb_n; i++) net_send(s, s->txb[i]);
    s->txb_n = 0;
}

static void h_send(void *ctx, const uint8_t pkt[kCwlPacketLen])
{
    Side *s = ctx;
    if (pkt[3] != kCwlType_Hello && pkt[3] != kCwlType_Caps) tr(s, "TX %d seq %u b %02x\n", pkt[3], pkt[4], pkt[5]);
    if (C.esp_a || s->flushing) { net_send(s, pkt); return; }
    if (s->txb_n < 256) memcpy(s->txb[s->txb_n++], pkt, kCwlPacketLen);
}
static void h_master_reply(void *ctx, uint8_t b)
{
    Side *s = ctx;
    const uint8_t noexch = s->cwl.stats.master_timeouts != s->last_timeouts;
    s->last_timeouts = s->cwl.stats.master_timeouts;
    esp_cmd(s, FC_REPLY, b, noexch);
}
static void h_master_dropped(void *ctx) { Side *s = ctx; s->last_timeouts = s->cwl.stats.master_timeouts; esp_cmd(s, FC_DROP, 0, 0); }
static void h_slave_offer(void *ctx, uint8_t b) { esp_cmd(ctx, FC_XFER, b, 0); }
static void h_slave_withdraw(void *ctx) { esp_cmd(ctx, FC_WITHDRAW, 0, 0); }
static void h_slave_lock(void *ctx) { esp_cmd(ctx, FC_LOCK, 0, 0); }
static void h_slave_unlock(void *ctx) { esp_cmd(ctx, FC_UNLOCK, 0, 0); }
static uint32_t h_now_us(void *ctx) { (void)ctx; return (uint32_t)(int64_t)(now * 1000.0); }

static uint32_t now_ms(void) { return (uint32_t)now; }

static void esp_event(Side *s, uint8_t ev, uint8_t b)
{
    CwlState_t *c = &s->cwl;
    const uint32_t t = now_ms();
    tr(s, "ESP ev %d %02x\n", ev, b);
    switch (ev) {
    case FE_ARMED: Cwl_OnLocalArmed(c, t); break;
    case FE_DISARM: Cwl_OnLocalDisarm(c, t); break;
    case FE_MASTER: Cwl_OnLocalMaster(c, b, t); break;
    case FE_SLAVE_DONE: Cwl_OnLocalSlaveResult(c, true, b, t); break;
    case FE_SLAVE_NAK: Cwl_OnLocalSlaveResult(c, false, 0, t); break;
    case FE_ABANDON: Cwl_OnLocalMasterAbandonB(c, b, t); break;
    case FE_NAK_STALLED:
        if (!c->standing) Cwl_OnLocalSlaveResult(c, false, 0, t);
        Cwl_OnLocalMasterCollision(c, t);
        break;
    case FE_LOCKED: Cwl_OnLocalLocked(c, true, b, t); break;
    case FE_LOCK_NAK: Cwl_OnLocalLocked(c, false, 0, t); break;
    case FE_SLAVE_ABANDON: Cwl_OnLocalSlaveAbandon(c, t); break;
    }
}

static void esp_packet(Side *s, const uint8_t *pkt)
{
    if (pkt[3] != kCwlType_Hello && pkt[3] != kCwlType_Caps) tr(s, "RX %d seq %u b %02x\n", pkt[3], pkt[4], pkt[5]);
    (void)Cwl_OnPacket(&s->cwl, pkt, kCwlPacketLen, now_ms());
}

static void safety(Side *s)
{
    if (s->held && now - s->held_t0 > 4000) {
        // FPGA safety net (~4 s): completes with 0xFF + IRQ. Never expected.
        s->safety++;
        s->noexch++;
        complete(s, 0xFF);
        master_released(s);
    }
    if (s->locked && s->stalled == ST_LOCK && now - s->stall_t0 > 4000) {
        s->safety++;
        s->locked = 0;
        unstall(s);
        apply_parked(s);
    }
}

// ---------------------------------------------------------------- GB glue

static int trace_sc;
static bool on_write(GB_gameboy_t *gb, uint16_t addr, uint8_t v)
{
    Side *s = side_of(gb);
    if (trace_sc && (addr == 0xFF01 || addr == 0xFF02) && !s->in_write)
        fprintf(stderr, "%10.3f %c f%llu W %s=%02x pc=%04x sc=%02x sb=%02x\n", now, 'A' + s->id, (unsigned long long)s->frames,
                addr == 0xFF01 ? "SB" : "SC", v, GB_get_registers(gb)->pc, GB_safe_read_memory(gb, 0xFF02), GB_safe_read_memory(gb, 0xFF01));
    if (C.cable) {
        if (addr == 0xFF02 && (v & 0x81) == 0x81) s->serial_master_starts++;
        return true;
    }
    if ((addr != 0xFF01 && addr != 0xFF02) || s->in_write) return true;
    return port_write(s, addr, v) != 0;
}

static uint8_t on_read(GB_gameboy_t *gb, uint16_t addr, uint8_t data)
{
    Side *s = side_of(gb);
    if (addr == 0xFF02 && !C.cable && s->held) data |= 1;   // reads see the internal clock
    if (addr == 0xFF00) {
        s->joyp_reads++;
        // Which scripted presses does this read show?
        for (uint32_t i = 0; i < s->presses_n; i++) {
            Press *p = &s->presses[i];
            if (p->seen || now < p->down || now >= p->up) continue;
            const int dir = p->key < 4;   // right left up down = P14 group
            if (dir && (data & 0x10)) continue;
            if (!dir && (data & 0x20)) continue;
            const int bit = dir ? p->key : p->key - 4;
            if (!(data & (1 << bit))) { p->seen = 1; p->seen_t = now; }
        }
    }
    return data;
}

static void vblank(GB_gameboy_t *gb, GB_vblank_type_t type)
{
    (void)type;
    Side *s = side_of(gb);
    s->frames++;
    if (s->watch) {
        const uint8_t v = GB_safe_read_memory(gb, (uint16_t)s->watch);
        if (!s->watch_init || v != s->watch_v) {
            fprintf(stderr, "%c f%llu t%.0f watch %04x = %02x\n", 'A' + s->id, (unsigned long long)s->frames, now, s->watch, v);
            s->watch_v = v;
            s->watch_init = 1;
        }
    }
}

static uint32_t rgb(GB_gameboy_t *gb, uint8_t r, uint8_t g, uint8_t b) { (void)gb; return 0xFF000000u | r << 16 | g << 8 | b; }

static void cable_bstart(GB_gameboy_t *gb, bool b) { side_of(gb)->cable_bit = b; }
static bool cable_bend(GB_gameboy_t *gb)
{
    Side *s = side_of(gb), *o = &S[1 - s->id];
    const bool ret = GB_serial_get_data_bit(o->gb);
    GB_serial_set_data_bit(o->gb, s->cable_bit);
    return ret;
}

// ---------------------------------------------------------------- input

static const char *kKeys[8] = {"right", "left", "up", "down", "a", "b", "select", "start"};
static int key_id(const char *n) { for (int k = 0; k < 8; k++) if (!strcmp(n, kKeys[k])) return k; return -1; }

static void add_press(Side *s, double down, double up, int key)
{
    if (s->presses_n == (uint32_t)s->presses_cap) { s->presses_cap = s->presses_cap ? s->presses_cap * 2 : 256; s->presses = realloc(s->presses, sizeof(Press) * (size_t)s->presses_cap); }
    s->presses[s->presses_n++] = (Press){down, up, key, 0, 0};
    Ev d = {.t = down, .kind = E_KEY, .side = s->id, .a = (uint8_t)key, .b = 1};
    Ev u = {.t = up, .kind = E_KEY, .side = s->id, .a = (uint8_t)key, .b = 0};
    hpush(d);
    hpush(u);
}

static void parse_keys(Side *s, char *spec)
{
    for (char *t = strtok(spec, ","); t; t = strtok(NULL, ",")) {
        char *c = strchr(t, ':');
        if (!c) continue;
        *c = 0;
        char *sl = strchr(c + 1, '/');
        int hold = 6;
        if (sl) { *sl = 0; hold = atoi(sl + 1); }
        const int k = key_id(c + 1);
        if (k < 0) { fprintf(stderr, "bad key %s\n", c + 1); exit(2); }
        const double f = atof(t);
        add_press(s, f * FRAME_MS, (f + hold) * FRAME_MS, k);
    }
}

// SEED:F0:F1:EVERY:HOLD:k1|k2|...  a press every EVERY wall frames (+-25%)
static void parse_player(Side *s, char *spec)
{
    unsigned seed; double f0, f1, every, hold;
    char keys[128];
    if (sscanf(spec, "%u:%lf:%lf:%lf:%lf:%127s", &seed, &f0, &f1, &every, &hold, keys) != 6) { fprintf(stderr, "bad --player\n"); exit(2); }
    int ks[8], nk = 0;
    for (char *t = strtok(keys, "|"); t && nk < 8; t = strtok(NULL, "|")) { const int k = key_id(t); if (k >= 0) ks[nk++] = k; }
    uint64_t r = seed * 2654435761ull + 1;
    for (double f = f0; f < f1;) {
        r ^= r << 13; r ^= r >> 7; r ^= r << 17;
        const int k = ks[r % (uint64_t)nk];
        add_press(s, f * FRAME_MS, (f + hold) * FRAME_MS, k);
        r ^= r << 13; r ^= r >> 7; r ^= r << 17;
        f += every * (0.75 + 0.5 * (double)(r % 1000) / 1000.0);
    }
}

// ---------------------------------------------------------------- output

static void save_ppm(Side *s, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n160 144\n255\n");
    for (int i = 0; i < 160 * 144; i++) { fputc(s->px[i] >> 16, f); fputc(s->px[i] >> 8, f); fputc(s->px[i], f); }
    fclose(f);
}

static int cmpd(const void *a, const void *b) { const double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double pct(Side *s, double p)
{
    if (!s->hold_vn) return 0;
    qsort(s->hold_v, (size_t)s->hold_vn, sizeof(double), cmpd);
    int i = (int)(p / 100.0 * (s->hold_vn - 1) + 0.5);
    return s->hold_v[i];
}

// The two port logs must mirror each other: A's i-th exchange (role, sent,
// got) is B's i-th with the roles swapped and the bytes crossed.
static int check_logs(int *matched, int *first_bad)
{
    Side *a = &S[0], *b = &S[1];
    const int n = a->log_n < b->log_n ? a->log_n : b->log_n;
    *first_bad = -1;
    for (int i = 0; i < n; i++) {
        const uint32_t x = a->log[i], y = b->log[i];
        const int ra = x >> 16, rb = y >> 16;
        const uint8_t sa = (x >> 8) & 0xFF, ga = x & 0xFF, sb = (y >> 8) & 0xFF, gb = y & 0xFF;
        if (ra == rb || sa != gb || ga != sb) { *first_bad = i; break; }
    }
    *matched = *first_bad < 0 ? n : *first_bad;
    const int tail = abs(a->log_n - b->log_n);
    return *first_bad < 0 && tail <= 2;
}


// ---------------------------------------------------------------- scenario
// --script FILE: threads of timed steps, all starting at t = 0.
//   thread NAME              start a thread (steps below run in order)
//   key SIDES KEY [FRAMES]   press (SIDES: a, b or ab), held 6 frames by default
//   keys SIDES SECONDS K1 K2 ...   press each, SECONDS apart (compat keys.sh)
//   sleep SECONDS
//   waitx SIDE N [TIMEOUT_S]       until SIDE's port has N exchanges (else note a timeout)
//   waitq SIDE SECONDS [TIMEOUT_S] until SIDE's port had no exchange for SECONDS
//   shot NAME                PPMs of both sides (PREFIX from --shot)
//   peek SIDE ADDR LEN LABEL print RAM ([BANK:]ADDR hex)
//   mark LABEL               print the time and exchange counts
typedef struct { char cmd[12]; char a[24], b[24]; double x, y; char rest[160]; } Step;
typedef struct { Step *st; int n, cap, pc; double wake; double wait_t0; int waiting; } Thread;
static Thread TH[8];
static int nth = 0;
static const char *shot_prefix = NULL;
static uint32_t script_timeouts = 0;

static void script_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(2); }
    char line[256];
    Thread *t = NULL;
    while (fgets(line, sizeof(line), f)) {
        char *h = strchr(line, '#'); if (h) *h = 0;
        char cmd[12] = {0};
        if (sscanf(line, "%11s", cmd) != 1) continue;
        if (!strcmp(cmd, "thread")) { if (nth < 8) t = &TH[nth++]; continue; }
        if (!t) { t = &TH[nth++]; }
        if (t->n == t->cap) { t->cap = t->cap ? t->cap * 2 : 32; t->st = realloc(t->st, sizeof(Step) * (size_t)t->cap); }
        Step *s = &t->st[t->n++];
        memset(s, 0, sizeof(*s));
        snprintf(s->cmd, sizeof(s->cmd), "%s", cmd);
        char *p = line + strspn(line, " \t");
        p += strlen(cmd);
        snprintf(s->rest, sizeof(s->rest), "%s", p);
        char *nl = strchr(s->rest, '\n'); if (nl) *nl = 0;
        sscanf(s->rest, "%23s %23s", s->a, s->b);
    }
    fclose(f);
}

static void press_now(const char *sides, const char *key, int frames)
{
    const int k = key_id(key);
    if (k < 0) { fprintf(stderr, "script: bad key %s\n", key); exit(2); }
    for (int i = 0; i < 2; i++)
        if (strchr(sides, 'a' + i)) add_press(&S[i], now + 0.01, now + 0.01 + frames * FRAME_MS, k);
}

static void script_tick(void)
{
    for (int i = 0; i < nth; i++) {
        Thread *t = &TH[i];
        while (t->pc < t->n && now >= t->wake) {
            Step *s = &t->st[t->pc];
            if (!strcmp(s->cmd, "key")) {
                int fr = 6; sscanf(s->rest, "%*s %*s %d", &fr);
                press_now(s->a, s->b, fr);
            } else if (!strcmp(s->cmd, "keys")) {
                // keys SIDES SECONDS K1 K2 ...: expand into key + sleep steps in place
                char sides[8], ks[160]; double sec;
                if (sscanf(s->rest, "%7s %lf %159[^\n]", sides, &sec, ks) == 3) {
                    char *save; int at = t->pc + 1, added = 0;
                    Step tmp[40]; int nt = 0;
                    for (char *k = strtok_r(ks, " \t", &save); k && nt < 38; k = strtok_r(NULL, " \t", &save)) {
                        char *sl = strchr(k, '/'); int fr = 6;
                        if (sl) { *sl = 0; fr = atoi(sl + 1); }
                        Step a = {0}; snprintf(a.cmd, sizeof(a.cmd), "key"); snprintf(a.a, sizeof(a.a), "%s", sides);
                        snprintf(a.b, sizeof(a.b), "%s", k); snprintf(a.rest, sizeof(a.rest), "%s %s %d", sides, k, fr);
                        Step z = {0}; snprintf(z.cmd, sizeof(z.cmd), "sleep"); snprintf(z.rest, sizeof(z.rest), "%g", sec); snprintf(z.a, sizeof(z.a), "%g", sec);
                        tmp[nt++] = a; tmp[nt++] = z;
                    }
                    added = nt;
                    if (t->n + added > t->cap) { t->cap = t->n + added + 32; t->st = realloc(t->st, sizeof(Step) * (size_t)t->cap); }
                    memmove(&t->st[at + added], &t->st[at], sizeof(Step) * (size_t)(t->n - at));
                    memcpy(&t->st[at], tmp, sizeof(Step) * (size_t)added);
                    t->n += added;
                    s = &t->st[t->pc];
                }
            } else if (!strcmp(s->cmd, "sleep")) {
                t->wake = now + atof(s->a) * 1000;
            } else if (!strcmp(s->cmd, "waitx") || !strcmp(s->cmd, "waitq")) {
                Side *sd = &S[s->a[0] == 'b'];
                double n = 0, to = 600; sscanf(s->rest, "%*s %lf %lf", &n, &to);
                if (!t->waiting) { t->waiting = 1; t->wait_t0 = now; sd->quiet_since = now; sd->quiet_n = sd->log_n; }
                int done;
                if (s->cmd[4] == 'x') done = sd->log_n >= (int)n;
                else {
                    if (sd->log_n != sd->quiet_n) { sd->quiet_n = sd->log_n; sd->quiet_since = now; }
                    done = now - sd->quiet_since >= n * 1000;
                }
                if (!done && now - t->wait_t0 < to * 1000) break;
                if (!done) { script_timeouts++; printf("script: %s %s %s timed out at %.1f s\n", s->cmd, s->a, s->b, now / 1000); }
                t->waiting = 0;
            } else if (!strcmp(s->cmd, "shot")) {
                if (shot_prefix) for (int k = 0; k < 2; k++) {
                    char path[600]; snprintf(path, sizeof(path), "%s_%s_%c.ppm", shot_prefix, s->a, 'a' + k);
                    save_ppm(&S[k], path);
                }
            } else if (!strcmp(s->cmd, "peek")) {
                Side *sd = &S[s->a[0] == 'b'];
                unsigned bank = 0, addr = 0, len = 1; char label[64] = "";
                char loc[24]; sscanf(s->rest, "%*s %23s %u %63s", loc, &len, label);
                if (strchr(loc, ':')) sscanf(loc, "%x:%x", &bank, &addr); else sscanf(loc, "%x", &addr);
                printf("peek %c %s t=%.1f %04x:", 'A' + sd->id, label, now / 1000, addr);
                for (unsigned k = 0; k < len && k < 64; k++) printf(" %02x", GB_safe_read_memory(sd->gb, (uint16_t)(addr + k)));
                printf("\n");
            } else if (!strcmp(s->cmd, "mark")) {
                printf("mark %s t=%.1f s exchanges A %d B %d\n", s->rest + strspn(s->rest, " "), now / 1000, S[0].log_n, S[1].log_n);
            }
            t->pc++;
        }
    }
}

// ---------------------------------------------------------------- main

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: linksim ROM_A STATE_A ROM_B STATE_B [options] (see linksim.c)\n");
        return 2;
    }
    double end_ms = 60000;
    const char *shot = NULL, *save[2] = {0}, *peek = NULL;
    int shot_every = 0;
    char *keys[2] = {0}, *players[2][4] = {{0}};
    int np[2] = {0, 0};
    uint64_t seed = 1;
    int sa_both = 0;
    for (int i = 5; i < argc; i++) {
        const char *o = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
#define ARG(name) (!strcmp(o, name) && v && ++i)
        if (ARG("--ms")) end_ms = atof(v);
        else if (ARG("--frames")) end_ms = atof(v) * FRAME_MS;
        else if (ARG("--rtt")) C.rtt = atof(v);
        else if (ARG("--jitter")) C.jitter = atof(v);
        else if (ARG("--loss")) C.loss = atof(v);
        else if (!strcmp(o, "--reorder")) C.reorder = 1;
        else if (ARG("--seed")) seed = strtoull(v, NULL, 10);
        else if (ARG("--esp")) C.esp_a = !strcmp(v, "a");
        else if (!strcmp(o, "--sa")) sa_both = 1;
        else if (!strcmp(o, "--sa-a")) C.sa[0] = 1;
        else if (!strcmp(o, "--sa-b")) C.sa[1] = 1;
        else if (!strcmp(o, "--nolock-a")) C.nolock[0] = 1;
        else if (!strcmp(o, "--nolock-b")) C.nolock[1] = 1;
        else if (!strcmp(o, "--cable")) C.cable = 1;
        else if (!strcmp(o, "--no-standing")) C.standing = 0;
        else if (!strcmp(o, "--no-absorb")) C.absorb = 0;
        else if (ARG("--timeout")) C.timeout_ms = (uint32_t)atoi(v);
        else if (ARG("--keys-a")) keys[0] = strdup(v);
        else if (ARG("--keys-b")) keys[1] = strdup(v);
        else if (ARG("--player-a")) { if (np[0] < 4) players[0][np[0]++] = strdup(v); }
        else if (ARG("--player-b")) { if (np[1] < 4) players[1][np[1]++] = strdup(v); }
        else if (ARG("--shot")) shot = shot_prefix = v;
        else if (ARG("--script")) script_load(v);
        else if (ARG("--shot-every")) shot_every = atoi(v);
        else if (ARG("--save-a")) save[0] = v;
        else if (ARG("--save-b")) save[1] = v;
        else if (ARG("--watch-a")) S[0].watch = (int)strtol(v, NULL, 16);
        else if (ARG("--watch-b")) S[1].watch = (int)strtol(v, NULL, 16);
        else if (ARG("--peek-end")) peek = v;
        else if (!strcmp(o, "--trace")) C.trace = 1;
        else if (!strcmp(o, "--trace-sc")) trace_sc = 1;
        else { fprintf(stderr, "unknown option %s\n", o); return 2; }
#undef ARG
    }
    if (sa_both) C.sa[0] = C.sa[1] = 1;
    rng ^= seed * 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 20; i++) (void)urand();
    if (C.esp_a) C.rx_tout_ms = C.uart_byte_ms;        // RX timeout 1 symbol
    else C.rx_tout_ms = 10 * C.uart_byte_ms;           // IDF default 10 symbols

    for (int i = 0; i < 2; i++) {
        Side *s = &S[i];
        s->id = i;
        s->gb = GB_init(GB_alloc(), GB_MODEL_CGB_E);
        GB_set_user_data(s->gb, s);
        if (GB_load_boot_rom(s->gb, BOOTROM_DIR "/cgb_boot.bin")) { fprintf(stderr, "boot rom\n"); return 1; }
        if (GB_load_rom(s->gb, argv[1 + 2 * i])) { fprintf(stderr, "rom %s\n", argv[1 + 2 * i]); return 1; }
        if (strcmp(argv[2 + 2 * i], "-") && GB_load_state(s->gb, argv[2 + 2 * i])) { fprintf(stderr, "state %s\n", argv[2 + 2 * i]); return 1; }
        GB_set_rgb_encode_callback(s->gb, rgb);
        GB_set_pixels_output(s->gb, s->px);
        GB_set_vblank_callback(s->gb, vblank);
        GB_set_sample_rate(s->gb, 0);
        GB_set_turbo_mode(s->gb, true, true);
        GB_set_write_memory_callback(s->gb, on_write);
        GB_set_read_memory_callback(s->gb, on_read);
        if (C.cable) {
            GB_set_serial_transfer_bit_start_callback(s->gb, cable_bstart);
            GB_set_serial_transfer_bit_end_callback(s->gb, cable_bend);
        }
        s->frame_n = (int)(end_ms / FRAME_MS) + 4;
        s->frame_stall = calloc((size_t)s->frame_n, sizeof(float));
        const CwlHost_t h = {s, h_send, h_master_reply, h_slave_offer, h_master_dropped, h_slave_withdraw,
                             h_slave_lock, h_slave_unlock, h_now_us};
        Cwl_Init(&s->cwl, &h, 0);
        Cwl_SetStandingOffers(&s->cwl, C.standing);
        Cwl_SetSession(&s->cwl, (uint16_t)(urand() * 65535) | 1);
        Cwl_SetSendAhead(&s->cwl, C.sa[i], !C.nolock[i]);
        s->cwl.master_timeout_ms = C.timeout_ms;
        if (keys[i]) parse_keys(s, keys[i]);
        for (int k = 0; k < np[i]; k++) parse_player(s, players[i][k]);
        if (!C.cable) {
            // ESP32 link task: a pass per tick (base; random phase) or a 1 ms poll (a)
            Ev e = {.t = urand() * C.pass_ms, .kind = C.esp_a ? E_POLL : E_PASS, .side = i};
            hpush(e);
        }
    }

    // The link was up long before a save state's moment (menus): each core has
    // heard the other's HELLO, as on a device (a first transfer before any HELLO
    // would read 0xFF, "unplugged", at any RTT above a few ms).
    if (!C.cable)
        for (int i = 0; i < 2; i++) {
            const uint16_t ps = S[1 - i].cwl.session;
            const uint8_t hello[kCwlPacketLen] = {'C', 'W', kCwlVersion, kCwlType_Hello, (uint8_t)(ps >> 8), (uint8_t)ps};
            (void)Cwl_OnPacket(&S[i].cwl, hello, kCwlPacketLen, 0);
        }
    if (nth) { Ev e = {.t = 0, .kind = E_SCRIPT}; hpush(e); }
    // run
    uint64_t steps = 0;
    for (;;) {
        Side *run = NULL;
        double tg = 1e300;
        for (int i = 0; i < 2; i++)
            if (!S[i].stalled && S[i].t < tg) { tg = S[i].t; run = &S[i]; }
        const double te = Hn ? H[0].t : 1e300;
        if (te <= tg) {
            if (te >= end_ms) break;
            Ev e = hpop();
            now = e.t;
            Side *s = &S[e.side];
            switch (e.kind) {
            case E_UP:
                if (C.esp_a) { e.kind = E_UPX; e.t = now + C.wake_ms; hpush(e); }
                else if (s->upq_n < 256) s->upq[s->upq_n++] = e;
                break;
            case E_UPX: esp_event(s, e.a, e.b); Cwl_Poll(&s->cwl, now_ms()); break;
            case E_NET:
                if (C.esp_a) { e.kind = E_NETX; e.t = now + C.wake_ms; hpush(e); }
                else if (s->rxq_n < 256) s->rxq[s->rxq_n++] = e;
                break;
            case E_NETX: esp_packet(s, e.pkt); Cwl_Poll(&s->cwl, now_ms()); break;
            case E_DOWN: fpga_cmd(s, e.a, e.b, e.c); break;
            case E_PASS: {
                // ESP32 0.17.2 link task: recvfrom returns at the tick; frames
                // that arrived are processed, the core polled, everything
                // queued goes out (LxFlush); then the next pass takes the FPGA
                // events (their packets wait for the next flush).
                s->flushing = 1;
                for (int k = 0; k < s->rxq_n; k++) esp_packet(s, s->rxq[k].pkt);
                s->rxq_n = 0;
                Cwl_Poll(&s->cwl, now_ms());
                s->flushing = 0;
                flush(s);
                for (int k = 0; k < s->upq_n; k++) esp_event(s, s->upq[k].a, s->upq[k].b);
                s->upq_n = 0;
                safety(s);
                e.t = now + C.pass_ms;
                hpush(e);
                break;
            }
            case E_POLL:
                Cwl_Poll(&s->cwl, now_ms());
                safety(s);
                e.t = now + 1.0;
                hpush(e);
                break;
            case E_KEY:
                GB_set_key_state(s->gb, (GB_key_t)e.a, e.b != 0);
                break;
            case E_SCRIPT:
                script_tick();
                e.t = now + 5;
                hpush(e);
                break;
            }
            continue;
        }
        if (!run) { fprintf(stderr, "deadlock: both stalled, no events\n"); break; }
        if (tg >= end_ms) break;
        now = run->t;
        const uint64_t f0 = run->frames;
        const unsigned ticks = GB_run(run->gb);
        run->t += ticks / TPMS;
        if (shot && shot_every && run->frames != f0 && run->frames % (uint64_t)shot_every == 0) {
            char p[600];
            snprintf(p, sizeof(p), "%s_%c_%06llu.ppm", shot, 'a' + run->id, (unsigned long long)run->frames);
            save_ppm(run, p);
        }
        steps++;
    }
    now = end_ms;
    for (int i = 0; i < 2; i++) if (S[i].stalled) { frame_stall_add(&S[i], S[i].stall_t0, now); }

    // report
    int matched, bad;
    const int ok = C.cable ? 1 : check_logs(&matched, &bad);
    if (C.cable) { matched = 0; bad = -1; }
    if (nth) printf("script: %u wait timeouts\n", script_timeouts);
    printf("linksim %s rtt %.1f jitter %.1f loss %.1f%s esp %s sa %d/%d%s, %.1f s simulated\n", C.cable ? "CABLE" : "cwl",
           C.rtt, C.jitter, C.loss, C.reorder ? " reorder" : "", C.esp_a ? "A" : "base", C.sa[0], C.sa[1],
           (C.nolock[0] || C.nolock[1]) ? " (nolock)" : "", end_ms / 1000);
    for (int i = 0; i < 2; i++) {
        Side *s = &S[i];
        const CwlStats_t *st = &s->cwl.stats;
        const int wall_frames = (int)(end_ms / FRAME_MS);
        int half = 0;
        double stall_total = 0;
        for (int k = 0; k < s->frame_n; k++) { if (s->frame_stall[k] > FRAME_MS / 2) half++; stall_total += s->frame_stall[k]; }
        uint32_t seen = 0, tot = 0;
        double lat = 0;
        for (uint32_t k = 0; k < s->presses_n; k++) {
            if (s->presses[k].down >= end_ms - 200) continue;
            tot++;
            if (s->presses[k].seen) { seen++; lat += s->presses[k].seen_t - s->presses[k].down; }
        }
        int nm = 0, ns = 0;
        for (int k = 0; k < s->log_n; k++) { if ((s->log[k] >> 16) == 'M') nm++; else ns++; }
        printf("side %c: frames %llu/%d lost %d, stalled %.0f ms (frames >half stalled %d), stalls barrier %u/%.0f ms fast %u/%.0f ms lock %u/%.0f ms max %.1f\n",
               'A' + i, (unsigned long long)s->frames, wall_frames, wall_frames - (int)s->frames, stall_total, half,
               s->stalls[ST_BARRIER], s->stall_ms[ST_BARRIER], s->stalls[ST_FAST], s->stall_ms[ST_FAST],
               s->stalls[ST_LOCK], s->stall_ms[ST_LOCK], s->stall_max);
        if (!C.cable) {
            const double hp50 = pct(s, 50), hp90 = pct(s, 90), hmax = s->hold_vn ? s->hold_v[s->hold_vn - 1] : 0;
            printf("  port: exchanges %d (master %d slave %d), master starts %u holds %u <1/<8/<17/<33/>=33 %u/%u/%u/%u/%u p50 %.1f p90 %.1f max %.1f ms, no-exchange %u, restarts %u, absorbed %u, safety %u\n",
                   s->log_n, nm, ns, s->serial_master_starts, s->holds, s->hold_b[0], s->hold_b[1], s->hold_b[2], s->hold_b[3], s->hold_b[4],
                   hp50, hp90, hmax, s->noexch, s->restarts, s->absorbed, s->safety);
            printf("  core: master_done %u timeouts %u slave_done %u abandons %u cancels %u collisions %u unknown_ab %u | sa lent %u taken %u revokes %u/%u retarget %u supp %u naks %u used %u stale %u wait_take %u mismatch %u stray %u\n",
                   st->master_done, st->master_timeouts, st->slave_done, st->master_abandons, st->cancels, st->master_collisions,
                   st->unknown_abandons, st->sa_pushes, st->sa_taken, st->sa_revoked, st->sa_revokes, st->sa_retargets,
                   st->sa_suppressed, st->sa_lock_naks, st->sa_used, st->sa_stale, st->sa_wait_take, st->sa_mismatch, st->sa_stray_take);
            char xb[1600];
            Cwl_XstatFormat(&s->cwl, xb, sizeof(xb), now_ms());
            for (char *l = strtok(xb, "\n"); l; l = strtok(NULL, "\n")) printf("  | %s\n", l);
        } else {
            printf("  cable: master starts %u\n", s->serial_master_starts);
        }
        printf("  input: presses %u seen %u unseen %u, mean latency to first read %.1f ms, joypad reads %u\n",
               tot, seen, tot - seen, seen ? lat / seen : 0.0, s->joyp_reads);
        if (shot) { char p[600]; snprintf(p, sizeof(p), "%s_%c.ppm", shot, 'a' + i); save_ppm(s, p); }
        if (save[i]) GB_save_state(s->gb, save[i]);
        if (peek) {
            unsigned a, l;
            if (sscanf(peek, "%x:%u", &a, &l) == 2) {
                printf("  peek %04x:", a);
                for (unsigned k = 0; k < l; k++) printf(" %02x", GB_safe_read_memory(s->gb, (uint16_t)(a + k)));
                printf("\n");
            }
        }
    }
    if (!C.cable) {
        printf("check: logs %s, %d exchanges mirrored, A %d B %d%s\n", ok ? "MIRROR" : "DESYNC", matched, S[0].log_n, S[1].log_n,
               bad >= 0 ? " FIRST MISMATCH" : "");
        if (bad >= 0) {
            for (int k = bad - 3 < 0 ? 0 : bad - 3; k < bad + 3; k++) {
                const uint32_t x = k < S[0].log_n ? S[0].log[k] : 0, y = k < S[1].log_n ? S[1].log[k] : 0;
                printf("  #%d A %c %02x/%02x  B %c %02x/%02x\n", k, (int)(x >> 16), (x >> 8) & 255, x & 255, (int)(y >> 16), (y >> 8) & 255, y & 255);
            }
        }
    }
    return ok ? 0 : 3;
}
