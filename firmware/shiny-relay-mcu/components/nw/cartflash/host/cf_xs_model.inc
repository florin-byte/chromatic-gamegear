/* cf_xs_model.inc (included by cf_sim.c): the FPGA's express cart stream (fpga cart_xs.v, 18.26),
 * cartspeed1. Same protocol and the same order rules as the RTL: ops on UART address 0x12 (queue
 * 2 deep + the one running), posts checked by crc32 against the ring, status (21 bytes, address
 * 0x0C) after every change, sharing the simulated 115,200-baud line both ways with the window.
 * Bus cycles cost the engine's time (hclk 16.777 MHz: write = setup+pulse+hold+gap+1 + 2 clocks of
 * hand-over, read = access+gap+1 + 2), the flash's busy time is the cart model's. */
#define XS_HCLK_US (1.0 / 16.777216)
static int opt_xs;                 /* --xs: the FPGA has the express stream */
static int opt_xs_lose;            /* --xs-lose N: every Nth status packet is lost on the line */
static long opt_xs_flip = -1;      /* --xs-flip POS: the QSPI write that holds stream byte POS carries one bad bit, once */
static int opt_xs_timing;          /* --xs-timing N: the job asks for that bus timing */
static int opt_xs_postloss;        /* --xs-postloss N: every Nth packet ESP32 -> FPGA on address 0x12 is lost on the line */
static struct {
    int session;
    uint8_t t[5];
    uint8_t ring[CF_XS_RING];
    uint32_t cons, valid;
    uint8_t naks;
    struct { uint8_t seq, op, arg; uint32_t f, n; } q[2];
    int qn;
    int run; uint8_t op, arg, seq, done_seq, run_seq, err;
    uint32_t f, n;
    uint32_t crc, crc_res; int all_ff, all_ff_res;
    double m_pbusy_us, m_ebusy_us;
    int starve_said;
    int bank, sub, abort_pend, reset_pend, chg;
    uint8_t val;
    double t_cur, starve_t0, poll_t0;
    unsigned n_status, n_lost, n_ops, n_posts, n_naks, n_prog_bytes;
    /* the line ESP32 -> FPGA */
    struct { uint8_t p[16]; uint8_t n; } in[64];
    unsigned in_h, in_t;
} XS;
static const uint8_t k_xs_safe[5] = { 4, 7, 4, 7, 4 };

static double xs_w(void) { return (XS.t[0] + XS.t[1] + XS.t[2] + XS.t[3] + 1 + 2) * XS_HCLK_US; }
static double xs_r(void) { return (XS.t[3] + XS.t[4] + 1 + 2) * XS_HCLK_US; }
static void xs_reset_all(void)
{
    memset(&XS.session, 0, (size_t)((char *)&XS.in - (char *)&XS.session));
    memcpy(XS.t, k_xs_safe, 5);
    XS.bank = -1;
}
static uint32_t xs_crc32(uint32_t c, uint8_t d) { c ^= d; for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1; return c; }
static void xs_fail(uint8_t e) { XS.err = e; XS.run = 0; XS.qn = 0; XS.chg = 1; }
/* the session ends (allow off, GB reset): an op that is running stops with NOSESS */
static void xs_session_end(void)
{
    XS.session = 0;
    memcpy(XS.t, k_xs_safe, 5);
    if (XS.run) xs_fail(CF_XS_E_NOSESS);
}
/* a packet arrives at the FPGA */
static void xs_rx(const uint8_t *p, uint8_t n)
{
    if (!opt_xs || !n) return;
    switch (p[0]) {
    case CF_XS_P_OP:
        if (n < 10) return;
        if (XS.err && !XS.reset_pend) return;
        if (XS.qn == 2) { XS.err = CF_XS_E_QFULL; XS.qn = 0; XS.chg = 1; return; }
        XS.q[XS.qn].seq = p[1]; XS.q[XS.qn].op = p[2];
        XS.q[XS.qn].f = (uint32_t)p[3] | (uint32_t)p[4] << 8 | (uint32_t)p[5] << 16;
        XS.q[XS.qn].n = (uint32_t)p[6] | (uint32_t)p[7] << 8 | (uint32_t)p[8] << 16;
        XS.q[XS.qn].arg = p[9];
        XS.qn++;
        break;
    case CF_XS_P_POST: {
        if (n < 11) return;
        const uint32_t e = ((uint32_t)p[1] | (uint32_t)p[2] << 8 | (uint32_t)p[3] << 16) & 0xFFFFFFu;
        const uint32_t len = (uint32_t)p[5] | (uint32_t)p[6] << 8;
        const uint32_t want = (uint32_t)p[7] | (uint32_t)p[8] << 8 | (uint32_t)p[9] << 16 | (uint32_t)p[10] << 24;
        int ok = len && len <= CF_XS_RING && ((e - len) & 0xFFFFFFu) == XS.valid && ((e - XS.cons) & 0xFFFFFFu) <= CF_XS_RING;
        if (ok) {
            uint32_t c = 0xFFFFFFFFu;
            for (uint32_t i = XS.valid; i != e; i = (i + 1) & 0xFFFFFFu) c = xs_crc32(c, XS.ring[i % CF_XS_RING]);
            ok = ~c == want;
        }
        if (ok) XS.valid = e; else { XS.naks++; XS.n_naks++; XS.chg = 1; }   /* only a refusal is reported */
        XS.n_posts++;
        break;
    }
    case CF_XS_P_ABORT: XS.abort_pend = 1; XS.qn = 0; break;
    case CF_XS_P_STATUS: XS.chg = 1; break;
    case CF_XS_P_RESET: XS.reset_pend = 1; XS.qn = 0; break;
    default: break;
    }
}
static void xs_status(uint8_t *s)
{
    memset(s, 0, 32);
    s[0] = 1;
    const int starved = XS.run && (XS.op == CF_XS_O_PROG || XS.op == 5) && XS.cons == XS.valid && t_us - XS.starve_t0 > 62000.0;
    s[1] = (uint8_t)((XS.session && EG.allow) | XS.run << 1 | (XS.err != 0) << 2 | EG.allow << 3 | starved << 4 | XS.session << 7);
    s[2] = XS.done_seq; s[3] = XS.err;
    s[4] = (uint8_t)XS.cons; s[5] = (uint8_t)(XS.cons >> 8); s[6] = (uint8_t)(XS.cons >> 16);
    s[8] = (uint8_t)XS.valid; s[9] = (uint8_t)(XS.valid >> 8); s[10] = (uint8_t)(XS.valid >> 16);
    s[12] = (uint8_t)XS.crc_res; s[13] = (uint8_t)(XS.crc_res >> 8); s[14] = (uint8_t)(XS.crc_res >> 16); s[15] = (uint8_t)(XS.crc_res >> 24);
    s[16] = (uint8_t)XS.f; s[17] = (uint8_t)(XS.f >> 8); s[18] = (uint8_t)(XS.f >> 16);
    s[19] = (uint8_t)(XS.all_ff_res << 7 | (XS.naks & 7) << 4 | XS.qn);
    s[20] = XS.run_seq;
    const uint32_t pb = (uint32_t)(XS.m_pbusy_us * 16.777216), eb = (uint32_t)(XS.m_ebusy_us * 16.777216);
    for (int i = 0; i < 4; i++) { s[21 + i] = (uint8_t)(pb >> (8 * i)); s[25 + i] = (uint8_t)(eb >> (8 * i)); }
    s[29] = (uint8_t)XS.n_prog_bytes; s[30] = (uint8_t)(XS.n_prog_bytes >> 8); s[31] = (uint8_t)(XS.n_prog_bytes >> 16);
}
/* one bus write / read at the model's time */
static void xs_wr(uint16_t a, uint8_t v) { cart_time(&cart, XS.t_cur); cart_write_engine(&cart, a, v); XS.t_cur += xs_w(); }
static uint8_t xs_rd(uint16_t a) { cart_time(&cart, XS.t_cur); const uint8_t v = cart_read(&cart, a); XS.t_cur += xs_r(); return v; }
static uint16_t xs_win(void) { return (uint16_t)(0x4000u | (XS.f & 0x3FFFu)); }
static void xs_bank(void)
{
    const int b = (int)((XS.f >> 14) & 0x1FF);
    if (b == XS.bank) return;
    xs_wr(0x2000, (uint8_t)b); xs_wr(0x3000, (uint8_t)(b >> 8));
    XS.bank = b;
}
/* DQ6 polling: the chip is ready -> 1 */
static int xs_poll(double limit_us)
{
    if (cart_busy(&cart)) {
        cart_time(&cart, XS.t_cur);
        if (cart_busy(&cart)) {
            if (XS.t_cur - XS.poll_t0 > limit_us) return -1;
            const double r = xs_r(), end = cart.busy_end;
            XS.t_cur = end > XS.t_cur ? XS.t_cur + r * (double)(long)((end - XS.t_cur) / r + 1.0) : XS.t_cur + r;
            return 0;
        }
    }
    (void)xs_rd(xs_win()); (void)xs_rd(xs_win());     /* two reads that do not toggle */
    return 1;
}
/* the FPGA runs until now_us */
static void xs_run(double now_us)
{
    if (!opt_xs) return;
    if (XS.t_cur < now_us - 1000.0) XS.t_cur = now_us - 1000.0;
    for (int guard = 0; guard < 200000 && XS.t_cur < now_us; guard++) {
        if (!XS.run) {
            if (XS.reset_pend) { XS.reset_pend = 0; XS.abort_pend = 0; XS.cons = XS.valid = 0; XS.err = 0; XS.chg = 1; XS.m_pbusy_us = 0; XS.n_prog_bytes = 0; continue; }
            if (XS.abort_pend) { XS.abort_pend = 0; XS.chg = 1; continue; }
            if (!XS.qn || XS.err || EG.op != EOP_NONE) { XS.t_cur = now_us; return; }
            XS.op = XS.q[0].op; XS.seq = XS.q[0].seq; XS.f = XS.q[0].f; XS.n = XS.q[0].n; XS.arg = XS.q[0].arg;
            const uint32_t qf = XS.q[0].f, qn = XS.q[0].n;
            XS.q[0] = XS.q[1]; XS.qn--;
            XS.run_seq = XS.seq; XS.bank = -1; XS.sub = 0; XS.chg = 1; XS.n_ops++;
            XS.t_cur += 2 * XS_HCLK_US;
            if (XS.op == CF_XS_O_CFG) {
                XS.t[0] = qf & 31; XS.t[1] = (qf >> 8) & 31; XS.t[2] = (qf >> 16) & 31; XS.t[3] = qn & 31; XS.t[4] = (qn >> 8) & 31;
                for (int i = 0; i < 5; i++) if (!XS.t[i]) XS.t[i] = 1;
                XS.session = ((qn >> 16) & 1) && EG.allow;
                XS.done_seq = XS.seq;
                continue;
            }
            if (XS.op < 1 || XS.op > 6 || XS.op == 4) { xs_fail(CF_XS_E_BADOP); continue; }
            if (!(XS.session && EG.allow)) { xs_fail(CF_XS_E_NOSESS); continue; }
            XS.run = 1; XS.crc = 0xFFFFFFFFu; XS.all_ff = 1; XS.starve_t0 = XS.t_cur;
            continue;
        }
        if (XS.op == CF_XS_O_ERASE) {
            if (XS.sub == 0) {
                xs_bank();
                xs_wr(0x0AAA, 0xAA); xs_wr(0x0555, 0x55); xs_wr(0x0AAA, 0x80); xs_wr(0x0AAA, 0xAA); xs_wr(0x0555, 0x55);
                xs_wr(XS.arg == 0x10 ? 0x0AAA : xs_win(), XS.arg);
                XS.sub = 1; XS.poll_t0 = XS.t_cur;
                continue;
            }
            const int r = xs_poll(4000000.0);
            if (r < 0) { xs_fail(CF_XS_E_ETMO); continue; }
            if (r == 0) continue;
            XS.m_ebusy_us = XS.t_cur - XS.poll_t0;
            XS.done_seq = XS.seq; XS.run = 0; XS.chg = 1;
            if (XS.abort_pend) { XS.abort_pend = 0; XS.err = CF_XS_E_ABORT; }
            continue;
        }
        if (XS.op == CF_XS_O_PROG) {
            if (XS.sub == 1) {
                const int r = xs_poll(500.0);
                if (r < 0) { xs_fail(CF_XS_E_PTMO); continue; }
                if (r == 0) continue;
                XS.m_pbusy_us += XS.t_cur - XS.poll_t0;
                if (xs_rd(xs_win()) != XS.val) { xs_fail(CF_XS_E_VERIFY); continue; }
                XS.f++; XS.n--; XS.sub = 0; XS.starve_t0 = XS.t_cur;
                continue;
            }
            if (!XS.n) { XS.done_seq = XS.seq; XS.run = 0; XS.chg = 1; continue; }
            if (XS.abort_pend) { XS.abort_pend = 0; XS.err = CF_XS_E_ABORT; XS.run = 0; XS.chg = 1; continue; }
            xs_bank();
            if (XS.cons == XS.valid) {                         /* starved: the next post */
                if (now_us - XS.starve_t0 > 4000000.0) { xs_fail(CF_XS_E_STARVE); continue; }
                if (now_us - XS.starve_t0 > 62000.0 && !XS.starve_said) { XS.starve_said = 1; XS.chg = 1; }
                XS.t_cur = now_us; return;
            }
            const uint8_t b = XS.ring[XS.cons % CF_XS_RING];
            XS.cons = (XS.cons + 1) & 0xFFFFFFu;
            if (!(XS.cons & 0x3FFu)) XS.chg = 1;
            XS.t_cur += 3 * XS_HCLK_US;
            XS.starve_said = 0;
            if (b == 0xFF) { XS.f++; XS.n--; continue; }
            XS.val = b; XS.n_prog_bytes++;
            xs_wr(0x0AAA, 0xAA); xs_wr(0x0555, 0x55); xs_wr(0x0AAA, 0xA0); xs_wr(xs_win(), b);
            XS.sub = 1; XS.poll_t0 = XS.t_cur;
            continue;
        }
        if (XS.op == 5 || XS.op == 6) {                   /* RAMW / RAMCRC */
            if (!XS.n) { if (XS.op == 6) { XS.crc_res = ~XS.crc; XS.all_ff_res = XS.all_ff; } XS.done_seq = XS.seq; XS.run = 0; XS.chg = 1; continue; }
            if (XS.abort_pend) { XS.abort_pend = 0; XS.err = CF_XS_E_ABORT; XS.run = 0; XS.chg = 1; continue; }
            if (XS.sub == 0) { xs_wr(0x0000, 0x0A); XS.sub = 1; XS.bank = -1; }
            const int rb = (int)((XS.f >> 13) & 3);
            if (XS.bank != rb) { xs_wr(0x4000, (uint8_t)rb); XS.bank = rb; }
            const uint16_t a = (uint16_t)(0xA000u | (XS.f & 0x1FFFu));
            if (XS.op == 6) {
                const uint8_t v = xs_rd(a);
                XS.crc = xs_crc32(XS.crc, v); if (v != 0xFF) XS.all_ff = 0;
            } else {
                if (XS.cons == XS.valid) {
                    if (now_us - XS.starve_t0 > 4000000.0) { xs_fail(CF_XS_E_STARVE); continue; }
                    if (now_us - XS.starve_t0 > 62000.0 && !XS.starve_said) { XS.starve_said = 1; XS.chg = 1; }
                    XS.t_cur = now_us; return;
                }
                const uint8_t b = XS.ring[XS.cons % CF_XS_RING];
                XS.cons = (XS.cons + 1) & 0xFFFFFFu;
                if (!(XS.cons & 0x3FFu)) XS.chg = 1;
                xs_wr(a, b); XS.starve_t0 = XS.t_cur;
            }
            XS.f++; XS.n--;
            continue;
        }
        /* CRC */
        if (!XS.n) { XS.crc_res = ~XS.crc; XS.all_ff_res = XS.all_ff; XS.done_seq = XS.seq; XS.run = 0; XS.chg = 1; continue; }
        if (XS.abort_pend) { XS.abort_pend = 0; XS.err = CF_XS_E_ABORT; XS.run = 0; XS.chg = 1; continue; }
        xs_bank();
        const uint8_t v = xs_rd(xs_win());
        XS.crc = xs_crc32(XS.crc, v);
        if (v != 0xFF) XS.all_ff = 0;
        XS.f++; XS.n--;
    }
}
/* the line: ESP32 -> FPGA packets queued by the firmware (h_xs_send), FPGA -> ESP32 status */
static void h_xs_send(void *u, const uint8_t *p, uint8_t n)
{
    (void)u;
    if (XS.in_t - XS.in_h >= 64) return;               /* the UART driver's buffer is full: lost */
    memcpy(XS.in[XS.in_t % 64].p, p, n); XS.in[XS.in_t % 64].n = n; XS.in_t++;
}
static int h_xs_ring(void *u, uint16_t off, const uint8_t *b, uint16_t n)
{
    (void)u;
    static uint32_t pos_guess;                         /* (for --xs-flip: the stream position is valid + what is in flight) */
    for (uint16_t i = 0; i < n; i++) XS.ring[(off + i) % CF_XS_RING] = b[i];
    if (opt_xs_flip >= 0) {
        const uint32_t p0 = C.xs ? C.xs->feed : 0;
        if ((uint32_t)opt_xs_flip >= p0 && (uint32_t)opt_xs_flip < p0 + n) { XS.ring[(off + (opt_xs_flip - p0)) % CF_XS_RING] ^= 0x04; opt_xs_flip = -1; }
    }
    (void)pos_guess;
    t_us += 60.0;                                      /* ~52 us on the 40 MHz quad bus + the driver */
    return 0;
}
static int h_xs_avail(void *u) { (void)u; return opt_xs; }
/* once a simulated ms: the line's budget (shared with the window's packets) */
static void xs_line_ms(double *e2f, double *f2e)
{
    if (!opt_xs) return;
    while (XS.in_h != XS.in_t && *e2f >= XS.in[XS.in_h % 64].n + 4) {
        *e2f -= XS.in[XS.in_h % 64].n + 4;
        static unsigned n_in;
        if (!(opt_xs_postloss && ++n_in % (unsigned)opt_xs_postloss == 0)) xs_rx(XS.in[XS.in_h % 64].p, XS.in[XS.in_h % 64].n);
        XS.in_h++;
    }
    if (XS.chg && *f2e >= 36) {
        uint8_t s[32];
        xs_status(s);
        XS.chg = 0;
        *f2e -= 36;
        XS.n_status++;
        if (opt_xs_lose && XS.n_status % (unsigned)opt_xs_lose == 0) { XS.n_lost++; return; }
        cf_esp_xs_status(&C, s, 32);
    }
}
