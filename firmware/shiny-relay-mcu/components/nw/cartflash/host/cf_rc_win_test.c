/* cf_rc_win_test.c: lane gamecomp1. Mac test of the timed window reads (../cf_rc_win.inc, the SAME code cf_rc_dev.c
 * builds) against a model of the FPGA's read window:
 *   - ops go out on a 115,200-baud line (14 bytes, ~1.2 ms each, one after another) and the FPGA runs them in order;
 *   - an op fills the 1 KiB window over its fill time (FREAD ~0.9 ms from the x1 boot flash, RREAD ~50 us from PSRAM),
 *     byte by byte, so a read in the middle of a fill sees part new, part old (a stale window);
 *   - a tagged design (r1831e) on an op with arg bit 0: word 0 = 0x0000 at the op's start, the data from word 1, then
 *     word 0 = {0x00, seq} once the data is in; an untagged design (<= r1831d) ignores the bit (data from word 0);
 *   - faults: an op the FPGA never runs (lost on the line), a slow FPGA (fill longer than the timed margins).
 * Virtual time: every esp_timer_get_time() call moves the clock 1 us (so busy-waits end), esp_rom_delay_us adds its us.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cf_proto.h"
#include "cf_rc_dev.h"

/* ---- the includer's part (see the head of cf_rc_win.inc) ---- */
static int64_t Now;
static int64_t esp_timer_get_time(void) { return Now++; }
static void esp_rom_delay_us(uint32_t us) { Now += us; }
#define UART_NUM_1 1
#define pdMS_TO_TICKS(ms) (ms)
static void vTaskDelay(int ticks) { Now += (int64_t)ticks * 1000; }
static int64_t LineFree;               /* when the line is free for the next packet */
static int uart_wait_tx_done(int u, int ticks) { (void)u; (void)ticks; if (Now < LineFree) Now = LineFree; return 0; }
static uint8_t _Seq = 0xC0;
static uint8_t BufMem[1024];
static uint8_t *_Buf;
static int buf_ok(void) { _Buf = BufMem; return 1; }

/* ---- the FPGA model ---- */
#define FLASH_SZ (4u << 20)
#define PSRAM_SZ (8u << 20)
static uint8_t *Flash, *Psram;
static uint8_t Win[1024];
static struct { int tagged, lose_nth, n_sent; int64_t fill_fread, fill_rread; } M;
typedef struct { uint8_t seq, op, arg; uint32_t a, ke; int64_t arrive; } op_t;
static op_t Q[64];
static int Qh, Qt;
static struct { int busy; op_t o; int64_t start, done; uint32_t wrote; } Run;
static int64_t FpgaFree;
static int Errors;
#define FAIL(...) do { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); Errors++; } while (0)

static void xs_send(const uint8_t *p, uint8_t n)
{
    if (n != 10 || p[0] != CF_XS_P_OP) { FAIL("not an op packet"); return; }
    op_t o = { p[1], p[2], p[9], p[3] | (uint32_t)p[4] << 8 | (uint32_t)p[5] << 16, p[6] | (uint32_t)p[7] << 8 | (uint32_t)p[8] << 16, 0 };
    if (o.op == CF_RC_O_RREAD && (o.a & 1u)) FAIL("RREAD at an odd address %06x", o.a);
    if (o.op == CF_RC_O_RREAD && (o.a & 1023u) + o.ke > 1024u) FAIL("RREAD across a 1 KiB row: %06x + %u", o.a, o.ke);
    if (o.ke & 1u) FAIL("odd length %u", o.ke);
    if ((o.arg & 1u) && o.ke > 1022u) FAIL("tagged op of %u data bytes (> 1022)", o.ke);
    if (!(o.arg & 1u) && o.ke > 1024u) FAIL("op of %u bytes (> 1024)", o.ke);
    const int64_t t = (Now > LineFree ? Now : LineFree) + 1215;   /* 14 bytes at 115,200 baud */
    LineFree = t;
    o.arrive = t;
    if (++M.n_sent == M.lose_nth) return;                          /* lost on the line */
    if ((Qt + 1) % 64 == Qh) { FAIL("op queue overflow"); return; }
    Q[Qt] = o; Qt = (Qt + 1) % 64;
}
/* the window as it is at time Now */
static void fpga_run(void)
{
    for (;;) {
        if (!Run.busy) {
            if (Qh == Qt || Q[Qh].arrive > Now) return;
            Run.o = Q[Qh]; Qh = (Qh + 1) % 64;
            Run.start = Run.o.arrive > FpgaFree ? Run.o.arrive : FpgaFree;
            Run.done = Run.start + (Run.o.op == CF_RC_O_FREAD ? M.fill_fread : M.fill_rread);
            Run.busy = 1; Run.wrote = 0;
            if (Run.start > Now) { Q[Qh = (Qh + 63) % 64] = Run.o; Run.busy = 0; return; }   /* (not started yet) */
            if (M.tagged && (Run.o.arg & 1u)) { Win[0] = 0; Win[1] = 0; }
        }
        const int tg = M.tagged && (Run.o.arg & 1u);
        const uint8_t *src = Run.o.op == CF_RC_O_FREAD ? Flash : Psram;
        const int64_t t = Now < Run.done ? Now : Run.done;
        const uint32_t upto = Run.done == Run.start ? Run.o.ke : (uint32_t)((uint64_t)Run.o.ke * (uint64_t)(t - Run.start) / (uint64_t)(Run.done - Run.start));
        for (; Run.wrote < upto && Run.wrote < Run.o.ke; Run.wrote++) Win[(tg ? 2u : 0u) + Run.wrote] = src[Run.o.a + Run.wrote];
        if (Now < Run.done) return;
        if (tg) { Win[0] = 0; Win[1] = Run.o.seq; }
        FpgaFree = Run.done; Run.busy = 0;
    }
}
static int OSD_QspiWinRead(uint16_t off, uint8_t *p, uint16_t len)
{
    if ((uint32_t)off + len > 1024u) { FAIL("window read past 1 KiB (%u + %u)", off, len); return -1; }
    fpga_run();
    memcpy(p, Win + off, len);
    Now += 2 + len / 8;                 /* QSPI: ~8 bytes a us, and a little */
    return 0;
}

static int win_read_sure(uint16_t len) { return OSD_QspiWinRead(0, _Buf, len) ? -2 : 0; }   /* (in the model a transfer never differs) */
#include "../cf_rc_win.inc"

/* ---- the tests ---- */
static uint32_t Rng = 12345;
static uint32_t rnd(void) { Rng ^= Rng << 13; Rng ^= Rng >> 17; Rng ^= Rng << 5; return Rng; }
static void reset(int tagged, int64_t fill_fread, int64_t fill_rread)
{
    memset(&M, 0, sizeof M); M.tagged = tagged; M.fill_fread = fill_fread; M.fill_rread = fill_rread;
    Qh = Qt = 0; memset(&Run, 0, sizeof Run); FpgaFree = 0; Now = 1000000; LineFree = 0;
    memset(Win, 0xEE, sizeof Win); memset(&_Ahead, 0, sizeof _Ahead);
    _Tag = -1; _TagOn = 1; _FastAhead = 1; _Seq = 0xC0;
}
static void drain(void) { ahead_done(); Now += 100000; fpga_run(); }
/* a library load's pattern: an extent read front to back in pieces of random sizes (the inflater's calls) */
static int stream(uint8_t op, uint32_t a, uint32_t len, uint32_t us, uint32_t *wrong)
{
    const uint8_t *src = op == CF_RC_O_FREAD ? Flash : Psram;
    static uint8_t got[1 << 16];
    uint32_t off = 0;
    *wrong = 0;
    while (off < len) {
        uint32_t k = 1 + rnd() % (rnd() % 4 ? 2048u : 64u);
        if (k > len - off) k = len - off;
        const int e = win_timed_lim(op, a + off, got, k, us, a + len);
        if (e) return e;
        for (uint32_t i = 0; i < k; i++) if (got[i] != src[a + off + i]) (*wrong)++;
        off += k;
    }
    return 0;
}

int main(void)
{
    Flash = malloc(FLASH_SZ); Psram = malloc(PSRAM_SZ);
    for (uint32_t i = 0; i < FLASH_SZ; i++) Flash[i] = (uint8_t)(i * 131u ^ i >> 9);
    for (uint32_t i = 0; i < PSRAM_SZ; i++) Psram[i] = (uint8_t)(i * 7u ^ i >> 11 ^ 0x5A);
    Flash[0] = 0x00; Flash[1] = 0xC0;  /* the flash's first word looks like the first probe's tag (an untagged design shows it) */
    int pass = 0;
    uint32_t wrong;
    int e;

    /* 1. the probe */
    reset(1, 900, 50);
    if (tag_probe() != 1) FAIL("tagged design: probe says no"); else pass++;
    reset(0, 900, 50);
    if (tag_probe() != 0) FAIL("untagged design: probe says yes (the flash's first word looks like a tag)"); else pass++;
    if (Now - 1000000 > 100000) FAIL("untagged probe took %lld us", (long long)(Now - 1000000)); else pass++;
    reset(1, 900, 50); M.lose_nth = 1;
    if (tag_probe() != 0) FAIL("probe with its first op lost: should say no (and the loads go untagged)"); else pass++;

    /* 2. tagged reads, both ops, ahead on/off, odd addresses, every piece size: the bytes exactly */
    for (int ah = 0; ah < 2; ah++)
        for (int opi = 0; opi < 2; opi++) {
            const uint8_t op = opi ? CF_RC_O_RREAD : CF_RC_O_FREAD;
            for (int r = 0; r < 40; r++) {
                reset(1, 900, 50); _FastAhead = ah; (void)tag_probe();
                const uint32_t a = rnd() % ((opi ? PSRAM_SZ : FLASH_SZ) - 70000u), len = 1 + rnd() % 60000u;
                e = stream(op, a, len, opi ? _FastUsRread : _FastUsFread, &wrong);
                drain();
                if (e || wrong) { FAIL("tagged %s ahead %d a %06x len %u: e %d, %u wrong", opi ? "RREAD" : "FREAD", ah, a, len, e, wrong); break; }
                pass++;
            }
        }

    /* 3. a slow FPGA (fill 4 ms, past the 1.5 ms margin): the timed reads take stale windows (the model sees it), the
          tagged ones wait for their own tag and are right */
    reset(0, 4000, 4000); _Tag = 0;
    e = stream(CF_RC_O_FREAD, 4096, 40000, _FastUsFread, &wrong); drain();
    if (e || !wrong) FAIL("slow FPGA, untagged: expected stale windows (e %d, %u wrong): the model does not show staleness", e, wrong); else pass++;
    printf("slow FPGA, untagged margins: %u of 40000 bytes wrong (the hazard the tag removes)\n", wrong);
    reset(1, 4000, 4000); (void)tag_probe();
    e = stream(CF_RC_O_FREAD, 4096, 40000, _FastUsFread, &wrong); drain();
    if (e || wrong) FAIL("slow FPGA, tagged: e %d, %u wrong", e, wrong); else pass++;

    /* 4. an op lost mid-stream: a late tag (-3) within the limit, never wrong bytes */
    for (int ah = 0; ah < 2; ah++) {
        reset(1, 900, 50); _FastAhead = ah; (void)tag_probe();
        M.lose_nth = M.n_sent + 7;
        const int64_t t0 = Now;
        e = stream(CF_RC_O_FREAD, 8192, 60000, _FastUsFread, &wrong); drain();
        if (e != -3) FAIL("lost op (ahead %d): e %d, expected -3", ah, e); else pass++;
        if (wrong) FAIL("lost op (ahead %d): %u wrong bytes before the error", ah, wrong); else pass++;
        if (_TagLate < 1) FAIL("lost op: not counted late"); else pass++;
        if (Now - t0 > 200000) FAIL("lost op: %lld us to give up", (long long)(Now - t0)); else pass++;
    }

    /* 5. seq wrap (0xFF -> 0xC0) inside one stream: > 64 ops */
    reset(1, 900, 50); (void)tag_probe(); _Seq = 0xF8;
    e = stream(CF_RC_O_RREAD, 2, 200000, _FastUsRread, &wrong); drain();
    if (e || wrong) FAIL("seq wrap: e %d, %u wrong", e, wrong); else pass++;

    /* 6. mixed: part of an extent (an op is out ahead), then a jump elsewhere (an ahead miss: that op lands first), then
          the other op the same way; a stream after an extent read to its end (nothing ahead: no miss); and tags off */
    for (int tg = 0; tg < 2; tg++) {
        reset(tg, 900, 50); (void)tag_probe();
        static uint8_t got[4096];
        uint32_t bad = 0;
        e = win_timed_lim(CF_RC_O_FREAD, 100000, got, 3000, _FastUsFread, 110000);
        for (uint32_t i = 0; i < 3000; i++) bad += got[i] != Flash[100000 + i];
        if (!e) e = win_timed_lim(CF_RC_O_FREAD, 900001, got, 2500, _FastUsFread, 905000);
        for (uint32_t i = 0; !e && i < 2500; i++) bad += got[i] != Flash[900001 + i];
        if (!e) e = win_timed_lim(CF_RC_O_RREAD, 333333, got, 4000, _FastUsRread, 400000);
        for (uint32_t i = 0; !e && i < 4000; i++) bad += got[i] != Psram[333333 + i];
        const uint32_t misses = _Ahead.misses;
        if (!e) e = stream(CF_RC_O_FREAD, 2000000, 5000, _FastUsFread, &wrong); else wrong = 0;
        drain();
        if (e || bad || wrong) FAIL("mixed (tag %d): e %d, %u + %u wrong", tg, e, bad, wrong); else pass++;
        if (misses != 2) FAIL("mixed (tag %d): %u ahead misses, expected 2", tg, misses); else pass++;
    }
    reset(1, 900, 50); (void)tag_probe(); _TagOn = 0;
    e = stream(CF_RC_O_FREAD, 50000, 20000, _FastUsFread, &wrong); drain();
    if (e || wrong) FAIL("tagged design, tags off (lib fast notag): e %d, %u wrong", e, wrong); else pass++;

    /* 7. speed on the model (line-bound): tagged+ahead should beat timed+ahead for FREAD */
    reset(0, 900, 50); _Tag = 0;
    int64_t t0 = Now; (void)stream(CF_RC_O_FREAD, 0x10000, 256 * 1024, _FastUsFread, &wrong); drain();
    const int64_t timed = Now - t0 - 100000;
    reset(1, 900, 50); (void)tag_probe();
    t0 = Now; (void)stream(CF_RC_O_FREAD, 0x10000, 256 * 1024, _FastUsFread, &wrong); drain();
    const int64_t tagged = Now - t0 - 100000;
    printf("model, 256 KiB FREAD: timed+ahead %lld ms, tagged+ahead %lld ms (%u polls, %u waits)\n", (long long)(timed / 1000), (long long)(tagged / 1000), _TagPolls, _TagWaits);
    if (tagged >= timed) FAIL("tagged not faster than timed on the model"); else pass++;

    printf("cf_rc_win_test: %d passed, %d failed\n", pass, Errors);
    return Errors ? 1 : 0;
}
