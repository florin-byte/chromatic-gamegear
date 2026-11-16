/* cf_share.c: Chromatic to Chromatic game sharing, the protocol (lane share1). See cf_share.h. Portable C: the unit
 * (cf_share_dev.c) and the Mac host test (host/cf_share_test.c) run this same file. Nothing here blocks or allocates. */
#include <stdio.h>
#include <string.h>

#include "cf_share.h"

#ifdef ESP_PLATFORM
#include "esp_rom_crc.h"
#include "mbedtls/sha256.h"
typedef mbedtls_sha256_context sh_sha_t;
static void sha_init(sh_sha_t *s) { mbedtls_sha256_init(s); (void)mbedtls_sha256_starts(s, 0); }
static void sha_upd(sh_sha_t *s, const uint8_t *b, uint32_t n) { (void)mbedtls_sha256_update(s, b, n); }
static void sha_fin(sh_sha_t *s, uint8_t d[32]) { (void)mbedtls_sha256_finish(s, d); mbedtls_sha256_free(s); }
static void sha_drop(sh_sha_t *s) { mbedtls_sha256_free(s); }
static uint32_t crc_z(uint32_t crc, const uint8_t *b, uint32_t n) { return esp_rom_crc32_le(crc, b, n); }
#else
#include "cf_sha256.h"
typedef cf_sha256_t sh_sha_t;
static void sha_init(sh_sha_t *s) { cf_sha256_init(s); }
static void sha_upd(sh_sha_t *s, const uint8_t *b, uint32_t n) { cf_sha256_update(s, b, n); }
static void sha_fin(sh_sha_t *s, uint8_t d[32]) { cf_sha256_final(s, d); }
static void sha_drop(sh_sha_t *s) { (void)s; }
uint32_t cf_crc32(uint32_t crc, const uint8_t *b, uint32_t n);   /* cf_esp.c (zlib's crc32) */
static uint32_t crc_z(uint32_t crc, const uint8_t *b, uint32_t n) { return cf_crc32(crc, b, n); }
#endif

uint32_t cf_sh_sha_size(void) { return (uint32_t)sizeof(sh_sha_t); }

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t since(uint32_t now, uint32_t t) { return now - t; }

const char *cf_sh_why(int e)
{
    switch (e) {
    case CF_SH_OK: return "ok";
    case CF_SH_E_NOTFOUND: return "not found";
    case CF_SH_E_DECLINED: return "declined";
    case CF_SH_E_BUSY: return "busy";
    case CF_SH_E_LOST: return "lost";
    case CF_SH_E_CANCELLED: return "cancelled";
    case CF_SH_E_STOPPED: return "the other side stopped";
    case CF_SH_E_READ: return "read";
    case CF_SH_E_WRITE: return "write";
    case CF_SH_E_CHECK: return "check";
    case CF_SH_E_UNKNOWN: return "unknown session";
    case CF_SH_E_NOANSWER: return "no answer";
    case CF_SH_E_ROOM: return "no room";
    case CF_SH_E_RADIO: return "radio";
    case CF_SH_E_START: return "did not start";
    default: return "?";
    }
}

int cf_sh_code_ok(const char *code)
{
    if (!code) return 0;
    for (int i = 0; i < CF_SH_CODE_LEN; i++) if (code[i] < '0' || code[i] > '9') return 0;
    return code[CF_SH_CODE_LEN] == 0;
}

uint32_t cf_sh_tag(const char code[CF_SH_CODE_LEN], const uint8_t mac[6])
{
    uint8_t b[4 + CF_SH_CODE_LEN + 6];
    memcpy(b, "SHR1", 4);
    memcpy(b + 4, code, CF_SH_CODE_LEN);
    memcpy(b + 4 + CF_SH_CODE_LEN, mac, 6);
    return crc_z(0, b, sizeof b);
}

uint32_t cf_sh_build(uint8_t *f, uint8_t type, uint32_t sid, const uint8_t *body, uint32_t n)
{
    if (n > CF_SH_FRAME_MAX - 12u) n = CF_SH_FRAME_MAX - 12u;
    f[0] = 'S'; f[1] = 'h'; f[2] = type; f[3] = CF_SH_VER;
    put32(f + 4, sid);
    if (n && body != f + 8) memmove(f + 8, body, n);
    put32(f + 8 + n, crc_z(0, f, 8 + n));
    return 12u + n;
}

int cf_sh_parse(const uint8_t *f, uint32_t n, uint8_t *type, uint32_t *sid, const uint8_t **body, uint32_t *bn)
{
    if (n < 12u || n > CF_SH_FRAME_MAX || f[0] != 'S' || f[1] != 'h' || f[3] != CF_SH_VER) return 0;
    if (get32(f + n - 4) != crc_z(0, f, n - 4)) return 0;
    *type = f[2];
    *sid = get32(f + 4);
    *body = f + 8;
    *bn = n - 12u;
    return 1;
}

void cf_sh_offer_put(uint8_t *b, const cf_sh_offer_t *o)
{
    b[0] = o->flags; b[1] = o->src; b[2] = o->cart_type; b[3] = o->cart_ram;
    put32(b + 4, o->game_size); put32(b + 8, o->rom_size); put32(b + 12, o->rom_crc32); put32(b + 16, o->save_size);
    memset(b + 20, 0, 17 + 33);
    memcpy(b + 20, o->title, strnlen(o->title, 16));
    memcpy(b + 37, o->file, strnlen(o->file, 32));
}

static int clean_text(char *s, unsigned n)
{
    s[n - 1] = 0;
    for (unsigned i = 0; s[i]; i++) if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7E) s[i] = '?';
    return 0;
}

int cf_sh_offer_get(const uint8_t *b, uint32_t n, cf_sh_offer_t *o)
{
    if (n < CF_SH_OFFER_LEN) return -1;
    memset(o, 0, sizeof *o);
    o->flags = b[0]; o->src = b[1]; o->cart_type = b[2]; o->cart_ram = b[3];
    o->game_size = get32(b + 4); o->rom_size = get32(b + 8); o->rom_crc32 = get32(b + 12); o->save_size = get32(b + 16);
    memcpy(o->title, b + 20, 16); clean_text(o->title, sizeof o->title);
    memcpy(o->file, b + 37, 32); clean_text(o->file, sizeof o->file);
    if (o->src > CF_SH_SRC_CARD || !o->game_size || o->game_size > 0x400000u || o->save_size > 0x20030u) return -1;
    if (!(o->flags & CF_SH_F_SAVE) && o->save_size) return -1;
    return 0;
}

static void logm(const cf_sh_radio_t *r, const char *m) { if (r->log) r->log(r->u, m); }
static const uint8_t kAll[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* ================================================================ A: the sender */
static void tx_send(cf_sh_tx_t *t, uint8_t type, const uint8_t *body, uint32_t n)
{
    uint8_t f[CF_SH_FRAME_MAX];
    const uint32_t len = cf_sh_build(f, type, t->sid, body, n);
    (void)t->r.send(t->r.u, t->peer, f, len);
}
static void tx_set(cf_sh_tx_t *t, int st) { t->st = st; t->t_state = t->now; }
static void tx_fail(cf_sh_tx_t *t, int e, int tell)
{
    if (tell) { const uint8_t b = (uint8_t)e; tx_send(t, CF_SH_T_CANCEL, &b, 1); }
    if (!t->sum_done && t->sha) { sha_drop((sh_sha_t *)t->sha); }
    t->sum_done = 1;                                   /* (the context is not touched again) */
    t->err = e;
    tx_set(t, CF_SH_TX_FAIL);
    char m[64];
    snprintf(m, sizeof m, "share: send failed: %s", cf_sh_why(e));
    logm(&t->r, m);
}
static void tx_hello(cf_sh_tx_t *t)
{
    uint8_t b[6 + CF_SH_OFFER_LEN];
    put32(b, t->tag_seen);
    b[4] = t->unit_lo; b[5] = t->unit_hi;
    cf_sh_offer_put(b + 6, &t->offer);
    tx_send(t, CF_SH_T_HELLO, b, sizeof b);
    t->t_hello = t->now;
}
static void tx_endf(cf_sh_tx_t *t)
{
    uint8_t b[44];
    put32(b, t->total); put32(b + 4, t->sum.crc); memcpy(b + 8, t->sum.sha, 32); put32(b + 40, t->sum.scrc);
    tx_send(t, CF_SH_T_END, b, sizeof b);
    t->t_end = t->now;
}

void cf_sh_tx_init(cf_sh_tx_t *t, const cf_sh_radio_t *r, int (*read)(void *u, uint32_t off, uint8_t *b, uint32_t n),
                   uint8_t *const seg[CF_SH_NSEG], void *sha, const uint8_t own_mac[6], uint8_t chan0, uint32_t sid, uint32_t now)
{
    memset(t, 0, sizeof *t);
    t->r = *r;
    t->read = read;
    for (unsigned i = 0; i < CF_SH_NSEG; i++) t->seg[i] = seg[i];
    t->sha = sha;
    t->sum_done = 1;
    t->unit_lo = own_mac[4]; t->unit_hi = own_mac[5];
    t->chan0 = (chan0 >= 1 && chan0 <= 13) ? chan0 : 1;
    t->sid = sid ? sid : 0x5A5A0001u;
    t->now = t->t0 = now;
}

void cf_sh_tx_start(cf_sh_tx_t *t, const char code[CF_SH_CODE_LEN], const cf_sh_offer_t *o, uint32_t now)
{
    t->now = t->t0 = now;
    memcpy(t->code, code, CF_SH_CODE_LEN); t->code[CF_SH_CODE_LEN] = 0;
    t->offer = *o;
    if (!(t->offer.flags & CF_SH_F_SAVE)) t->offer.save_size = 0;
    t->total = t->offer.game_size + t->offer.save_size;
    t->have = t->next = t->ring_end = 0;
    t->win = 8;
    memset(&t->sum, 0, sizeof t->sum);
    sha_init((sh_sha_t *)t->sha);
    t->sum_done = 0;
    t->chan = t->chan0;
    t->sweep_i = 0;
    if (t->r.channel) t->r.channel(t->r.u, t->chan);
    t->t_sweep = now;
    t->err = 0;
    tx_set(t, CF_SH_TX_FIND);
}

int cf_sh_tx_busy(const cf_sh_tx_t *t) { return t->st != CF_SH_TX_IDLE && t->st != CF_SH_TX_DONE && t->st != CF_SH_TX_FAIL; }

void cf_sh_tx_cancel(cf_sh_tx_t *t, uint32_t now)
{
    t->now = now;
    if (!cf_sh_tx_busy(t)) return;
    tx_fail(t, CF_SH_E_CANCELLED, t->st != CF_SH_TX_FIND);
}

void cf_sh_tx_frame(cf_sh_tx_t *t, const uint8_t src[6], const uint8_t *f, uint32_t n, uint32_t now)
{
    uint8_t type; uint32_t sid, bn; const uint8_t *b;
    t->now = now;
    if (!cf_sh_parse(f, n, &type, &sid, &b, &bn)) { t->n_bad++; return; }
    t->n_frames_in++;
    if (t->st == CF_SH_TX_FIND) {
        if (type == CF_SH_T_BEACON && bn >= 6 && get32(b) == cf_sh_tag(t->code, src)) {
            memcpy(t->peer, src, 6);
            t->peer_unit = (uint16_t)get16(b + 4);
            t->tag_seen = get32(b);
            t->t_heard = now;
            tx_set(t, CF_SH_TX_HELLO);
            tx_hello(t);
            logm(&t->r, "share: found the other Chromatic");
        }
        return;
    }
    if (!cf_sh_tx_busy(t) || memcmp(src, t->peer, 6) || sid != t->sid) return;
    t->t_heard = now;
    switch (type) {
    case CF_SH_T_REPLY:
        if (bn < 6 || (t->st != CF_SH_TX_HELLO && t->st != CF_SH_TX_ASKED)) break;
        if (b[0] == CF_SH_V_WAIT) { if (t->st != CF_SH_TX_ASKED) tx_set(t, CF_SH_TX_ASKED); }
        else if (b[0] == CF_SH_V_TAKE) {
            t->dest = b[1];
            t->have = t->next = 0;                     /* (a new session always starts at 0: B's `have` is for later versions) */
            t->t_ack = t->t_probe = now;
            t->t_first = now;
            tx_set(t, CF_SH_TX_SEND);
        } else tx_fail(t, b[0] == CF_SH_V_NO ? CF_SH_E_DECLINED : CF_SH_E_BUSY, 0);
        break;
    case CF_SH_T_ACK:
        if (bn < 7 || (t->st != CF_SH_TX_SEND && t->st != CF_SH_TX_END)) break;
        {
            const uint32_t h = get32(b), w = get16(b + 4);
            if (h > t->total || h > t->next + (t->st == CF_SH_TX_END ? 0u : CF_SH_RING)) break;   /* (not ours to believe) */
            if (h > t->have) { t->have = h; t->t_ack = now; }
            if (h > t->next) t->next = h;
            if (w) t->win = w > 32u ? 32u : w;
            if ((b[6] & CF_SH_ACK_NAK) && t->next > t->have) { t->next = t->have; t->n_nak++; t->n_rewind++; }
            if (t->st == CF_SH_TX_SEND && t->have >= t->total && t->sum_done) { tx_set(t, CF_SH_TX_END); tx_endf(t); }
        }
        break;
    case CF_SH_T_RESULT:
        if (bn < 2 || t->st != CF_SH_TX_END) break;
        t->b_stage = b[1];
        if (b[1] == 1) {
            t->t_last = now;
            if (b[0] == CF_SH_OK) { tx_set(t, CF_SH_TX_DONE); logm(&t->r, "share: sent, the other Chromatic checked it"); }
            else tx_fail(t, b[0] < CF_SH_NE ? b[0] : CF_SH_E_WRITE, 0);
        }
        break;
    case CF_SH_T_CANCEL:
        tx_fail(t, (bn >= 1 && b[0] == CF_SH_E_UNKNOWN) ? CF_SH_E_UNKNOWN : (bn >= 1 && (b[0] == CF_SH_E_WRITE || b[0] == CF_SH_E_ROOM)) ? b[0] : CF_SH_E_STOPPED, 0);
        break;
    default: break;
    }
}

/* the ring: stream byte `off` lives at seg[(off % RING) / SEG] + off % SEG; a piece is contiguous to its segment's end */
static uint8_t *ring_at(const cf_sh_tx_t *t, uint32_t off) { return t->seg[(off & (CF_SH_RING - 1u)) / CF_SH_SEG] + (off & (CF_SH_SEG - 1u)); }
static uint32_t ring_run(uint32_t off) { return CF_SH_SEG - (off & (CF_SH_SEG - 1u)); }
/* the ring holds the stream bytes [ring_end - CF_SH_RING, ring_end): read more so [off, off + n) is in it */
static int tx_fill(cf_sh_tx_t *t, uint32_t off, uint32_t n)
{
    while (t->ring_end < off + n) {
        uint32_t k = t->total - t->ring_end;
        if (k > CF_SH_SEG) k = CF_SH_SEG;
        if (t->ring_end + k - t->have > CF_SH_RING) return 1;          /* (never past what B still needs: cannot happen with CF_SH_WIN) */
        for (uint32_t q = 0; q < k; ) {
            const uint32_t o = t->ring_end + q;
            uint32_t m = ring_run(o);
            if (m > k - q) m = k - q;
            uint8_t *p = ring_at(t, o);
            if (t->read(t->r.u, o, p, m)) return -1;
            /* the hashes, as the bytes are first read (each byte once): the game's crc32 + sha256, the save's crc32 */
            for (uint32_t z = 0; z < m; ) {
                uint32_t w = m - z;
                const uint32_t oz = o + z;
                if (oz < t->offer.game_size && oz + w > t->offer.game_size) w = t->offer.game_size - oz;
                if (oz < t->offer.game_size) { sha_upd((sh_sha_t *)t->sha, p + z, w); t->sum.crc = crc_z(t->sum.crc, p + z, w); }
                else t->sum.scrc = crc_z(t->sum.scrc, p + z, w);
                z += w;
            }
            q += m;
        }
        t->ring_end += k;
        if (t->ring_end >= t->total && !t->sum_done) { sha_fin((sh_sha_t *)t->sha, t->sum.sha); t->sum_done = 1; }
    }
    return 0;
}

void cf_sh_tx_tick(cf_sh_tx_t *t, uint32_t now)
{
    t->now = now;
    switch (t->st) {
    case CF_SH_TX_FIND:
        if (since(now, t->t0) >= CF_SH_FIND_MS) { tx_fail(t, CF_SH_E_NOTFOUND, 0); break; }
        if (since(now, t->t_sweep) >= CF_SH_DWELL_MS && t->r.channel) {
            /* the sweep: the home channel, then 1, 6, 11 (where a receiver goes), then the others, round again */
            static const uint8_t kOrder[13] = { 1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13 };
            t->sweep_i = (uint8_t)((t->sweep_i + 1u) % 14u);
            t->chan = t->sweep_i == 0 ? t->chan0 : kOrder[t->sweep_i - 1u];
            t->r.channel(t->r.u, t->chan);
            t->t_sweep = now;
        }
        break;
    case CF_SH_TX_HELLO:
        if (since(now, t->t_heard) >= 15000u) { tx_fail(t, CF_SH_E_NOANSWER, 0); break; }
        if (since(now, t->t_hello) >= CF_SH_HELLO_MS) tx_hello(t);
        break;
    case CF_SH_TX_ASKED:
        if (since(now, t->t_heard) >= 15000u) { tx_fail(t, CF_SH_E_LOST, 0); break; }
        if (since(now, t->t_state) >= CF_SH_ASK_MS + 10000u) { tx_fail(t, CF_SH_E_NOANSWER, 1); break; }
        if (since(now, t->t_hello) >= 1000u) tx_hello(t);
        break;
    case CF_SH_TX_SEND: {
        if (since(now, t->t_heard) >= CF_SH_LOST_MS) { tx_fail(t, CF_SH_E_LOST, 1); break; }
        const int quiet = since(now, t->t_heard) >= 1000u;   /* nothing heard for a second: one frame now and then */
        if (t->next > t->have && since(now, t->t_ack) >= CF_SH_RTO_MS && since(now, t->t_probe) >= (quiet ? CF_SH_PROBE_MS : CF_SH_RTO_MS)) {
            t->next = t->have; t->n_resend++; t->t_probe = now;
        }
        const uint32_t win = quiet ? 1u : t->win;
        int sent = 0;
        while (t->next < t->total && t->next < t->have + win * CF_SH_DATA_MAX && sent < 32) {
            uint32_t k = t->total - t->next;
            if (k > CF_SH_DATA_MAX) k = CF_SH_DATA_MAX;
            const int fe = tx_fill(t, t->next, k);
            if (fe < 0) { tx_fail(t, CF_SH_E_READ, 1); return; }
            if (fe > 0) break;
            uint8_t f[CF_SH_FRAME_MAX];
            put32(f + 8, t->next);
            for (uint32_t q = 0; q < k; ) {
                uint32_t m = ring_run(t->next + q);
                if (m > k - q) m = k - q;
                memcpy(f + 12 + q, ring_at(t, t->next + q), m);
                q += m;
            }
            const uint32_t len = cf_sh_build(f, CF_SH_T_DATA, t->sid, f + 8, 4u + k);
            if (t->r.send(t->r.u, t->peer, f, len)) { t->n_noroom++; break; }
            t->next += k;
            t->n_data++;
            sent++;
            if (quiet) { t->t_probe = now; t->n_probe++; break; }
        }
        if (t->have >= t->total && t->sum_done) { tx_set(t, CF_SH_TX_END); tx_endf(t); }
        break;
    }
    case CF_SH_TX_END:
        if (since(now, t->t_heard) >= CF_SH_LOST_MS) { tx_fail(t, CF_SH_E_LOST, 1); break; }
        if (since(now, t->t_state) >= CF_SH_FINISH_MS + 30000u) { tx_fail(t, CF_SH_E_NOANSWER, 1); break; }
        if (since(now, t->t_end) >= CF_SH_END_MS) tx_endf(t);
        break;
    default: break;
    }
}

/* ================================================================ B: the receiver */
static void rx_sendto(cf_sh_rx_t *x, const uint8_t dst[6], uint32_t sid, uint8_t type, const uint8_t *body, uint32_t n)
{
    uint8_t f[CF_SH_FRAME_MAX];
    const uint32_t len = cf_sh_build(f, type, sid, body, n);
    (void)x->r.send(x->r.u, dst, f, len);
}
static void rx_send(cf_sh_rx_t *x, uint8_t type, const uint8_t *body, uint32_t n) { rx_sendto(x, x->peer, x->sid, type, body, n); }
static void rx_set(cf_sh_rx_t *x, int st) { x->st = st; x->t_state = x->now; }
static void rx_reply(cf_sh_rx_t *x, uint8_t verdict)
{
    uint8_t b[6] = { verdict, x->dest, 0, 0, 0, 0 };
    rx_send(x, CF_SH_T_REPLY, b, sizeof b);
}
static void rx_ack(cf_sh_rx_t *x, int nak)
{
    uint8_t b[7];
    put32(b, x->have); put16(b + 4, CF_SH_WIN); b[6] = nak ? CF_SH_ACK_NAK : 0;
    rx_send(x, CF_SH_T_ACK, b, sizeof b);
    x->acked = x->have;
    x->t_ack = x->now;
    if (nak) x->t_nak = x->now;
    x->n_acks++;
}
static void rx_result(cf_sh_rx_t *x, uint8_t code, uint8_t stage)
{
    const uint8_t b[2] = { code, stage };
    rx_send(x, CF_SH_T_RESULT, b, 2);
}
static void rx_let_go(cf_sh_rx_t *x)
{
    /* the place's half-written bytes go (finish with ok 0): once */
    if (x->st == CF_SH_RX_DATA || x->st == CF_SH_RX_CHECK || x->st == CF_SH_RX_FINISH) {
        if (x->st == CF_SH_RX_DATA || x->st == CF_SH_RX_CHECK) sha_drop((sh_sha_t *)x->sha);
        if (x->finish) (void)x->finish(x->r.u, 0);
    }
}
static void rx_fail(cf_sh_rx_t *x, int e, int tell)
{
    if (tell) { const uint8_t b = (uint8_t)e; rx_send(x, CF_SH_T_CANCEL, &b, 1); }
    rx_let_go(x);
    x->err = e;
    rx_set(x, CF_SH_RX_FAIL);
    char m[64];
    snprintf(m, sizeof m, "share: receive failed: %s", cf_sh_why(e));
    logm(&x->r, m);
}

void cf_sh_rx_init(cf_sh_rx_t *x, const cf_sh_radio_t *r, void *sha, const uint8_t own_mac[6], uint32_t now)
{
    memset(x, 0, sizeof *x);
    x->r = *r;
    x->sha = sha;
    memcpy(x->own, own_mac, 6);
    x->now = x->t0 = now;
}

void cf_sh_rx_start(cf_sh_rx_t *x, const char code[CF_SH_CODE_LEN], uint32_t now)
{
    x->now = x->t0 = now;
    memcpy(x->code, code, CF_SH_CODE_LEN); x->code[CF_SH_CODE_LEN] = 0;
    x->tag = cf_sh_tag(x->code, x->own);
    x->t_beacon = now - CF_SH_BEACON_MS;
    x->err = 0;
    x->sid = 0;
    rx_set(x, CF_SH_RX_WAIT);
}

int cf_sh_rx_busy(const cf_sh_rx_t *x) { return x->st != CF_SH_RX_IDLE && x->st != CF_SH_RX_DONE && x->st != CF_SH_RX_FAIL; }

void cf_sh_rx_cancel(cf_sh_rx_t *x, uint32_t now)
{
    x->now = now;
    if (!cf_sh_rx_busy(x)) return;
    rx_fail(x, CF_SH_E_CANCELLED, x->st != CF_SH_RX_WAIT);
}

void cf_sh_rx_answer(cf_sh_rx_t *x, int take, uint8_t dest, uint32_t now)
{
    x->now = now;
    if (x->st != CF_SH_RX_ASK) return;
    if (!take) { rx_reply(x, CF_SH_V_NO); x->err = CF_SH_E_DECLINED; rx_set(x, CF_SH_RX_FAIL); return; }
    const int e = x->begin ? x->begin(x->r.u, &x->offer, dest) : 0;
    if (e) {
        /* the place cannot take it (no room, the card would not open): the sender is told why, not just NO */
        const uint8_t c = (uint8_t)(e == CF_SH_E_ROOM ? CF_SH_E_ROOM : CF_SH_E_WRITE);
        rx_send(x, CF_SH_T_CANCEL, &c, 1);
        x->err = e; rx_set(x, CF_SH_RX_FAIL);
        return;
    }
    x->dest = dest;
    x->total = x->offer.game_size + x->offer.save_size;
    x->have = x->acked = 0;
    memset(&x->sum, 0, sizeof x->sum);
    sha_init((sh_sha_t *)x->sha);
    x->have_end = 0;
    x->t_heard = x->t_ack = x->t_take = now;
    rx_set(x, CF_SH_RX_DATA);
    rx_reply(x, CF_SH_V_TAKE);
}

static void rx_take(cf_sh_rx_t *x, const uint8_t *p, uint32_t n)
{
    if (x->write(x->r.u, x->have, p, n)) { rx_fail(x, CF_SH_E_WRITE, 1); return; }
    for (uint32_t q = 0; q < n; ) {
        const uint32_t o = x->have + q;
        uint32_t m = n - q;
        if (o < x->offer.game_size && o + m > x->offer.game_size) m = x->offer.game_size - o;
        if (o < x->offer.game_size) { sha_upd((sh_sha_t *)x->sha, p + q, m); x->sum.crc = crc_z(x->sum.crc, p + q, m); }
        else x->sum.scrc = crc_z(x->sum.scrc, p + q, m);
        q += m;
    }
    x->have += n;
    x->t_take = x->now;
    x->n_data++;
    if (!x->t_first) x->t_first = x->now;
}

static void rx_check(cf_sh_rx_t *x)
{
    sha_fin((sh_sha_t *)x->sha, x->sum.sha);
    x->checked_ok = x->sum.crc == x->want.crc && !memcmp(x->sum.sha, x->want.sha, 32) && x->sum.scrc == x->want.scrc &&
                    (!x->offer.rom_crc32 || (x->offer.flags & CF_SH_F_PACKED) || x->offer.rom_crc32 == x->sum.crc);
    x->t_end = x->now;
    rx_set(x, CF_SH_RX_FINISH);
    char m[96];
    snprintf(m, sizeof m, "share: all %lu bytes came: crc32 %08lx sha256 %s", (unsigned long)x->total, (unsigned long)x->sum.crc,
             x->checked_ok ? "right" : "WRONG");
    logm(&x->r, m);
}

void cf_sh_rx_frame(cf_sh_rx_t *x, const uint8_t src[6], const uint8_t *f, uint32_t n, uint32_t now)
{
    uint8_t type; uint32_t sid, bn; const uint8_t *b;
    x->now = now;
    if (!cf_sh_parse(f, n, &type, &sid, &b, &bn)) { x->n_bad++; return; }
    if (type == CF_SH_T_BEACON) return;
    const int mine = x->sid && sid == x->sid && !memcmp(src, x->peer, 6);
    if (type == CF_SH_T_HELLO) {
        if (bn < 6 + CF_SH_OFFER_LEN || get32(b) != x->tag) return;
        if (x->st == CF_SH_RX_WAIT && sid) {
            cf_sh_offer_t o;
            if (cf_sh_offer_get(b + 6, bn - 6, &o)) return;
            x->sid = sid;
            memcpy(x->peer, src, 6);
            x->peer_unit = (uint16_t)get16(b + 4);
            x->offer = o;
            x->t_heard = now;
            rx_set(x, CF_SH_RX_ASK);
            if (x->on_offer) x->on_offer(x->r.u, &x->offer, x->peer_unit);
            rx_reply(x, CF_SH_V_WAIT);
            return;
        }
        if (mine) {
            x->t_heard = now;
            if (x->st == CF_SH_RX_ASK) rx_reply(x, CF_SH_V_WAIT);
            else if (x->st == CF_SH_RX_DATA) rx_reply(x, CF_SH_V_TAKE);   /* (A missed the TAKE) */
            return;
        }
        if (cf_sh_rx_busy(x) && x->st != CF_SH_RX_WAIT) { uint8_t r6[6] = { CF_SH_V_BUSY, 0, 0, 0, 0, 0 }; rx_sendto(x, src, sid, CF_SH_T_REPLY, r6, 6); }
        return;
    }
    if (!mine) {
        /* a session this unit does not know (it restarted, or gave up): say so, so the sender stops at once */
        if ((type == CF_SH_T_DATA || type == CF_SH_T_END) && sid && (x->st == CF_SH_RX_WAIT || x->st == CF_SH_RX_ASK || !x->sid)) {
            const uint8_t c = CF_SH_E_UNKNOWN;
            rx_sendto(x, src, sid, CF_SH_T_CANCEL, &c, 1);
        }
        return;
    }
    x->t_heard = now;
    switch (type) {
    case CF_SH_T_DATA: {
        if (x->st != CF_SH_RX_DATA || bn < 5) {
            if ((x->st == CF_SH_RX_FINISH || x->st == CF_SH_RX_DONE) && since(now, x->t_ack) >= CF_SH_ACK_MS) rx_ack(x, 0);
            break;
        }
        const uint32_t off = get32(b), k = bn - 4u;
        if (off + k > x->total) { x->n_bad++; break; }
        if (off == x->have) {
            rx_take(x, b + 4, k);
            if (x->st != CF_SH_RX_DATA) break;
            if (x->have >= x->total || x->have - x->acked >= CF_SH_WIN * CF_SH_DATA_MAX / 2u) rx_ack(x, 0);
        } else if (off < x->have) {
            x->n_dup++;
            if (since(now, x->t_ack) >= CF_SH_ACK_MS) rx_ack(x, 0);
        } else {
            x->n_ooo++;
            if (since(now, x->t_nak) >= CF_SH_ACK_MS) rx_ack(x, 1);
        }
        break;
    }
    case CF_SH_T_END:
        if (bn < 44) break;
        if (x->st == CF_SH_RX_DATA) {
            if (get32(b) != x->total || x->have < x->total) { rx_ack(x, 0); break; }
            x->want.crc = get32(b + 4); memcpy(x->want.sha, b + 8, 32); x->want.scrc = get32(b + 40);
            x->have_end = 1;
            rx_check(x);
            rx_result(x, 0, 0);
        } else if (x->st == CF_SH_RX_FINISH) rx_result(x, 0, 0);
        else if (x->st == CF_SH_RX_DONE) rx_result(x, CF_SH_OK, 1);
        else if (x->st == CF_SH_RX_FAIL) rx_result(x, (uint8_t)(x->err ? x->err : CF_SH_E_WRITE), 1);
        break;
    case CF_SH_T_CANCEL:
        if (cf_sh_rx_busy(x)) rx_fail(x, CF_SH_E_STOPPED, 0);
        break;
    default: break;
    }
}

void cf_sh_rx_tick(cf_sh_rx_t *x, uint32_t now)
{
    x->now = now;
    switch (x->st) {
    case CF_SH_RX_WAIT:
        if (since(now, x->t_state) >= CF_SH_WAIT_MS) { x->err = CF_SH_E_NOANSWER; rx_set(x, CF_SH_RX_FAIL); break; }
        if (since(now, x->t_beacon) >= CF_SH_BEACON_MS) {
            uint8_t b[6];
            put32(b, x->tag); b[4] = x->own[4]; b[5] = x->own[5];
            rx_sendto(x, kAll, 0, CF_SH_T_BEACON, b, sizeof b);
            x->t_beacon = now;
        }
        break;
    case CF_SH_RX_ASK:
        if (since(now, x->t_heard) >= 15000u) { rx_fail(x, CF_SH_E_LOST, 0); break; }
        if (since(now, x->t_state) >= CF_SH_ASK_MS) { rx_reply(x, CF_SH_V_NO); x->err = CF_SH_E_NOANSWER; rx_set(x, CF_SH_RX_FAIL); }
        break;
    case CF_SH_RX_DATA:
        if (since(now, x->t_heard) >= CF_SH_LOST_MS) { rx_fail(x, CF_SH_E_LOST, 1); break; }
        if (x->have < x->total && since(now, x->t_take) >= CF_SH_GAP_MS && since(now, x->t_nak) >= CF_SH_GAP_MS) rx_ack(x, 1);   /* (where it is: a lost frame with none after it) */
        else if ((x->have != x->acked && since(now, x->t_ack) >= CF_SH_ACK_MS) || since(now, x->t_ack) >= 500u) rx_ack(x, 0);
        break;
    case CF_SH_RX_FINISH: {
        if (!x->checked_ok) {
            if (x->finish) (void)x->finish(x->r.u, 0);
            x->err = CF_SH_E_CHECK;
            rx_set(x, CF_SH_RX_FAIL);
            rx_result(x, CF_SH_E_CHECK, 1);
            logm(&x->r, "share: the game did not check: not kept");
            break;
        }
        const int r = x->finish ? x->finish(x->r.u, 1) : 0;
        if (r == 1) {
            if (since(now, x->t_state) >= CF_SH_FINISH_MS) { if (x->finish) (void)x->finish(x->r.u, 0); x->err = CF_SH_E_WRITE; rx_set(x, CF_SH_RX_FAIL); rx_result(x, CF_SH_E_WRITE, 1); }
            break;
        }
        if (r < 0) { x->err = -r; rx_set(x, CF_SH_RX_FAIL); rx_result(x, (uint8_t)(-r), 1); break; }
        x->err = CF_SH_OK;
        rx_set(x, CF_SH_RX_DONE);
        rx_result(x, CF_SH_OK, 1);
        break;
    }
    default: break;
    }
}
