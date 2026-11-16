/* cf_rc_win.inc: lane gamecomp1. The timed window reads of cf_rc_dev.c (no status round trip; tagged windows when the
 * FPGA design has them), in their own file so the Mac test (host/cf_rc_win_test.c) runs the SAME code against a model of
 * the FPGA. Included once, by cf_rc_dev.c and by that test. The includer gives: _Seq, _Buf (1 KiB), buf_ok, xs_send,
 * OSD_QspiWinRead, win_read_sure (a window read twice, kept when both agree), esp_timer_get_time, esp_rom_delay_us, uart_wait_tx_done (UART_NUM_1), vTaskDelay (pdMS_TO_TICKS),
 * CF_XS_P_OP, CF_RC_O_FREAD, printf. */

/* lane gamecomp1: a window read WITHOUT its status round trip (a library game's load). An op on the 115,200-baud UART is
   14 bytes (1.2 ms); the status packets that say it ended are ~3 ms each on the way back, and waiting for them made every
   KiB cost 13 ms (RREAD) / 26 ms (FREAD). Here the op is sent, the UART drained, a fixed margin waited (the FPGA fills the
   window in microseconds from PSRAM, in ~1 ms from its x1 boot flash), and the window read. No check of its own: the
   caller's data is SGZ1, whose every block has a crc32 and the whole game a sha256; a load that does not check is done
   again the checked way. The FPGA must be idle (nothing queued) and the margins long enough: lib fast shows / sets them. */
static uint32_t _FastUsFread = 1500, _FastUsRread = 300;
static int _FastOn = 1, _FastAhead = 1;
/* send-ahead: after a window is read, the op for the next KiB goes out at once (its 1.2 ms on the line and the FPGA's fill
   happen while the caller inflates); a request for that KiB then only waits for what is left of that time */
static struct { int on; uint8_t op, seq, tag; uint32_t a, ke, lim; int64_t ready; uint32_t hits, misses; } _Ahead;
/* the window tag (fpgacomp1 r1831e, gamecomp1 R7a): an op with arg bit 0 leaves window word 0 = {0x00, its seq} once its
   bytes are in (0x0000 from its start), its data from word 1 (n <= 1022). The ESP32 then polls the window itself (QSPI,
   microseconds) for its op's tag instead of a fixed margin: no stale window, no status round trip. Older designs (same
   version 18.31 up to r1831d) ignore the bit: found out by tag_probe. _Tag: -1 not known yet, 0 no, 1 yes */
static int _Tag = -1, _TagOn = 0;   /* off by default: unit 1 r1831e 10-22, a library play with tags on came out DAMAGED (checked re-reads spoiled too); lib fast tag turns them on */
static uint32_t _TagWaits, _TagPolls, _TagLate;
#define TAG_DATA_MAX 1020u               /* data bytes per tagged window (even, <= 1022) */
#define TAG_WAIT_US  30000               /* an op whose tag is not there by then: an error (the caller reads again, checked) */
static uint8_t op_send(uint8_t op, uint32_t a, uint32_t ke, uint8_t arg)
{
    const uint8_t seq = _Seq;
    _Seq = (uint8_t)(_Seq == 0xFF ? 0xC0 : _Seq + 1);
    const uint8_t p[10] = { CF_XS_P_OP, seq, op, (uint8_t)a, (uint8_t)(a >> 8), (uint8_t)(a >> 16), (uint8_t)ke, (uint8_t)(ke >> 8), 0, arg };
    xs_send(p, sizeof p);
    return seq;
}
#define OP_LINE_US 1300                  /* 14 bytes at 115,200 baud, and a little */
/* the tagged op seq's window (ke data bytes) into _Buf (tag at 0..1, data from 2), once its tag is there; ready = when
   its packet is off the line (no tag can be there before) */
static int tag_wait(uint8_t seq, uint32_t ke, int64_t ready)
{
    while (esp_timer_get_time() < ready) { }
    const int64_t t0 = esp_timer_get_time();
    _TagWaits++;
    for (int i = 0;; i++) {
        if (OSD_QspiWinRead(0, _Buf, 2)) return -2;
        _TagPolls++;
        if (_Buf[0] == 0 && _Buf[1] == seq) break;
        if (esp_timer_get_time() - t0 > TAG_WAIT_US) { _TagLate++; return -3; }
        if (i > 4) esp_rom_delay_us(50);
    }
    if (OSD_QspiWinRead(0, _Buf, (uint16_t)(ke + 2u))) return -2;
    return (_Buf[0] == 0 && _Buf[1] == seq) ? 0 : -3;   /* (word 0 is read first: its tag says the data after it was in) */
}
/* does this design tag its windows? Two tagged FREADs of 2 bytes at the flash's start (data that never looks like a tag
   twice: the seqs differ) */
static int tag_probe(void)
{
    if (_Tag >= 0 || !buf_ok()) return _Tag;
    int ok = 1;
    for (int i = 0; i < 2 && ok; i++) {
        const uint8_t seq = op_send(CF_RC_O_FREAD, 0, 2, 1);
        ok = tag_wait(seq, 2, esp_timer_get_time() + OP_LINE_US) == 0;
    }
    if (!ok) vTaskDelay(pdMS_TO_TICKS(20));             /* (an untagged design's op lands, then nothing is in flight) */
    _Tag = ok;
    printf("rc: window tag: %s\n", ok ? "yes (tagged window reads)" : "no (timed margins)");
    return _Tag;
}
/* lim: the first byte not to read ahead into (the end of this extent / of the scratch copy). Any address: RREAD takes even
   ones only (cart_xs qrrok), so a read at an odd address reads from the even one below and skips its first byte */
static int win_timed_lim(uint8_t op, uint32_t a, uint8_t *dst, uint32_t n, uint32_t us, uint32_t lim)
{
    if (!buf_ok()) return -1;
    const int tg = _Tag == 1 && _TagOn;
    const uint32_t win = tg ? TAG_DATA_MAX : 1024u, d0 = tg ? 2u : 0u;
    while (n) {
        const uint32_t a0 = a & ~1u, sk = a - a0;
        /* an RREAD stays inside one 1 KiB PSRAM row (ramcart.v's window read; across one, late in a library play on unit 1, its
           bytes came back a word late, both fills alike) */
        const uint32_t rl = op == CF_RC_O_RREAD ? 1024u - (a0 & 1023u) : 0xFFFFFFFFu, cap = (win < rl ? win : rl) - sk;
        const uint32_t k = n < cap ? n : cap, ke = (sk + k + 1u) & ~1u;
        uint8_t seq = 0;
        int64_t ready = 0;
        if (_Ahead.on && _Ahead.op == op && _Ahead.a == a0 && _Ahead.ke >= ke && _Ahead.tag == tg) {
            _Ahead.hits++;
            seq = _Ahead.seq; ready = _Ahead.ready;
            if (!tg) while (esp_timer_get_time() < _Ahead.ready) { }
        } else {
            if (_Ahead.on) {                               /* (a stray op ahead of this one lands first) */
                _Ahead.misses++;
                if (_Ahead.tag) (void)tag_wait(_Ahead.seq, _Ahead.ke, _Ahead.ready);
                else { while (esp_timer_get_time() < _Ahead.ready) { } esp_rom_delay_us(us); }
            }
            seq = op_send(op, a0, ke, (uint8_t)tg);
            ready = esp_timer_get_time() + OP_LINE_US;
            if (!tg) { (void)uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(20)); esp_rom_delay_us(us); }
        }
        _Ahead.on = 0;
        if (tg) { const int e = tag_wait(seq, ke, ready); if (e) return e; }
        else { const int we = win_read_sure((uint16_t)ke); if (we) return we; }
        memcpy(dst, _Buf + d0 + sk, k);
        dst += k; a += k; n -= k;
        const uint32_t na = a & ~1u;
        if (_FastAhead && na + 2u <= lim) {             /* the next window, sent now */
            const uint32_t nrl = op == CF_RC_O_RREAD ? 1024u - (na & 1023u) : 0xFFFFFFFFu, nw = win < nrl ? win : nrl;
            const uint32_t nk = lim - na < nw ? ((lim - na + 1u) & ~1u) : nw;
            _Ahead.seq = op_send(op, na, nk, (uint8_t)tg);
            _Ahead.on = 1; _Ahead.op = op; _Ahead.a = na; _Ahead.ke = nk; _Ahead.lim = lim; _Ahead.tag = (uint8_t)tg;
            _Ahead.ready = esp_timer_get_time() + OP_LINE_US + (tg ? 0 : us);
        }
    }
    return 0;
}

/* the op sent ahead lands before any other window op (a read-back would read its bytes, or the next read stale ones) */
static void ahead_done(void)
{
    if (!_Ahead.on) return;
    if (_Ahead.tag) (void)tag_wait(_Ahead.seq, _Ahead.ke, _Ahead.ready);
    else { while (esp_timer_get_time() < _Ahead.ready) { } esp_rom_delay_us(_FastUsFread); }
    _Ahead.on = 0;
}
