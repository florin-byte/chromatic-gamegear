/* nw_esp.c: see nw_esp.h. */
#include "nw_esp.h"
#include <string.h>

#define KEEPALIVE_MS      1000u   /* CONFIG every second (FPGA watchdog: 4 s) */
#define TX_STALL_MS        150u   /* request stream stalled: ask for the rest */
#define POST_TIMEOUT_MS    300u   /* no POSTED / RXNAK after RXPOST: ask for STATE */
#define POST_MAX_TRIES      10
#define NAK_DEBOUNCE_MS    250u   /* the FPGA NAKs every out-of-order chunk; one resend per NAK point */

static void pkt(nw_esp_t *e, uint8_t type, uint8_t id, uint16_t a, uint16_t b, int nb)
{
    uint8_t p[6] = { type, id, (uint8_t)a, (uint8_t)(a >> 8), (uint8_t)b, (uint8_t)(b >> 8) };
    e->send(e->send_user, p, nb);
}

static void send_config(nw_esp_t *e)
{
    uint8_t p[2] = { NW_R_CONFIG, (uint8_t)((e->enabled ? NW_CFG_SVC : 0) |
                                            (e->enabled && e->online ? NW_CFG_ONLINE : 0) |
                                            (e->enabled && e->notify ? NW_CFG_NOTIFY : 0) |
                                            (e->awake ? NW_CFG_AWAKE : 0)) };
    e->send(e->send_user, p, 2);
    e->last_keepalive = e->now;
}

void nw_esp_init(nw_esp_t *e, const nw_esp_config_t *cfg, const nw_backend_t *be,
                 void (*send)(void *user, const uint8_t *p, int n), void *send_user)
{
    memset(e, 0, sizeof(*e));
    e->cfg = *cfg;
    e->be = *be;
    e->send = send; e->send_user = send_user;
    e->enabled = 1;
    e->awake = 1;
    e->tokens_milli = (uint32_t)cfg->rate_burst * 1000u;
    e->last_nak_have = 0xFFFF;
}

void nw_esp_reboot(nw_esp_t *e)
{
    nw_esp_config_t cfg = e->cfg; nw_backend_t be = e->be;
    void (*send)(void *, const uint8_t *, int) = e->send; void *su = e->send_user;
    int en = e->enabled, on = e->online, aw = e->awake;
    uint8_t (*lop)(void *, uint8_t, const uint8_t *, uint16_t, uint8_t *, uint16_t, uint16_t *) = e->local_op;
    void *lu = e->local_user;
    nw_esp_init(e, &cfg, &be, send, su);
    e->enabled = en; e->online = on; e->awake = aw;
    e->local_op = lop; e->local_user = lu;
    send_config(e);
    pkt(e, NW_R_STATUS, 0, 0, 0, 1);
}

void nw_esp_set_enabled(nw_esp_t *e, int on) { e->enabled = on; send_config(e); }
void nw_esp_set_online(nw_esp_t *e, int on)  { e->online = on; send_config(e); }
void nw_esp_set_notify(nw_esp_t *e, int p)   { e->notify = p; send_config(e); }
void nw_esp_set_awake(nw_esp_t *e, int a)    { if (a != e->awake) { e->awake = a; send_config(e); } }

int nw_path_ok(const char *p, size_t max_len)
{
    size_t n = strlen(p), i;
    if (n == 0 || n > max_len || p[0] == '/') return 0;
    if (strstr(p, "..") || strstr(p, "//")) return 0;
    for (i = 0; i < n; i++) {
        const char c = p[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) continue;
        if (c == '.' || c == '_' || c == '~' || c == '/' || c == '-' || c == '?' || c == '=' || c == '&' || c == '+') continue;
        return 0;   /* no ':' (scheme / host), '@', '%' (encoding tricks), '\\', spaces, controls */
    }
    return 1;
}

/* ---------------- response path ---------------- */

static void send_rest(nw_esp_t *e)
{
    while (e->resp_sent < e->resp_len) {
        uint16_t n = (uint16_t)(e->resp_len - e->resp_sent);
        uint8_t p[4 + NW_RXDATA_MAX];
        if (n > NW_RXDATA_MAX) n = NW_RXDATA_MAX;
        p[0] = NW_R_RXDATA; p[1] = e->id; p[2] = (uint8_t)e->resp_sent; p[3] = (uint8_t)(e->resp_sent >> 8);
        memcpy(p + 4, e->resp + e->resp_sent, n);
        e->send(e->send_user, p, 4 + n);
        e->resp_sent = (uint16_t)(e->resp_sent + n);
    }
    pkt(e, NW_R_RXPOST, e->id, e->resp_len, 0, 4);
    e->post_sent_at = e->now;
    e->post_tries++;
}

static void respond(nw_esp_t *e, uint8_t status, uint8_t handle, uint32_t total, const uint8_t *body, uint16_t len)
{
    const uint16_t room = NW_RX_SIZE - NW_RESP_HDR;
    if (len > room) len = room;
    if (total < len) total = len;
    e->resp[0] = NW_MSG_VER; e->resp[1] = status; e->resp[2] = handle;
    e->resp[3] = (uint8_t)(total > len ? NW_RF_MORE : 0);
    e->resp[4] = (uint8_t)len; e->resp[5] = (uint8_t)(len >> 8);
    e->resp[6] = (uint8_t)total; e->resp[7] = (uint8_t)(total >> 8);
    e->resp[8] = (uint8_t)(total >> 16); e->resp[9] = (uint8_t)(total >> 24);
    if (len && body != e->resp + NW_RESP_HDR) memmove(e->resp + NW_RESP_HDR, body, len);
    e->resp_len = (uint16_t)(NW_RESP_HDR + len);
    e->resp_sent = 0;
    e->post_tries = 0;
    e->state = NW_ESP_RESPONDING;
    e->want_state = 0;
    e->last_nak_have = 0xFFFF;
    send_rest(e);
}

void nw_esp_complete(nw_esp_t *e, uint8_t id, uint8_t status, uint8_t handle,
                     uint32_t total_len, const uint8_t *body, uint16_t body_len)
{
    if (e->state != NW_ESP_CALLING || id != e->id) return;   /* aborted or stale */
    respond(e, status, handle, total_len, body, body_len);
}

/* ---------------- request path ---------------- */

static int take_token(nw_esp_t *e)
{
    const uint32_t cap = (uint32_t)e->cfg.rate_burst * 1000u;
    e->tokens_milli += (e->now - e->last_refill) * e->cfg.rate_per_s;
    e->last_refill = e->now;
    if (e->tokens_milli > cap) e->tokens_milli = cap;
    if (e->tokens_milli < 1000u) return 0;
    e->tokens_milli -= 1000u;
    return 1;
}

static void handle_request(nw_esp_t *e)
{
    const uint8_t *r = e->req;
    const uint16_t n = e->req_len;
    e->n_requests++;
    if (n < NW_REQ_HDR) { respond(e, NW_S_BAD_REQUEST, 0, 0, NULL, 0); return; }
    if (r[0] != NW_MSG_VER) { respond(e, NW_S_UNSUPPORTED, 0, 0, NULL, 0); return; }
    const uint8_t op = r[1];
    const uint8_t *body = r + NW_REQ_HDR;
    const uint16_t blen = (uint16_t)(n - NW_REQ_HDR);

    if (op == NW_OP_INFO) {
        const uint8_t info[10] = { NW_VERSION, e->cfg.fw_major, e->cfg.fw_minor, (uint8_t)e->online,
                                   (uint8_t)e->enabled, (uint8_t)e->identified,
                                   (uint8_t)NW_TX_SIZE, (uint8_t)(NW_TX_SIZE >> 8),
                                   (uint8_t)NW_RX_SIZE, (uint8_t)(NW_RX_SIZE >> 8) };
        respond(e, NW_S_OK, 0, 0, info, sizeof info);
        return;
    }
    if (op >= NW_OP_LINK_FIRST && op <= NW_OP_LINK_LAST) {
        /* the game's multiplayer room: answered here every frame, never via HTTPS */
        /* the answer is built straight into the response buffer (no second 1 KB) */
        uint8_t *const out = e->resp + NW_RESP_HDR;
        uint16_t ol = 0;
        uint8_t st = NW_S_UNSUPPORTED;
        if (e->local_op) st = e->local_op(e->local_user, op, body, blen, out, NW_RX_SIZE - NW_RESP_HDR, &ol);
        e->n_local++;
        e->n_requests--;   /* counted apart: requests are gateway calls */
        respond(e, st, 0, 0, out, ol);
        return;
    }
#ifdef NW_CARTFLASH
    if (op >= 0x20 && op <= 0x21) {   /* cart flash: NW_OP_CF_FIRST..NW_OP_CF_LAST */
        uint8_t *const out = e->resp + NW_RESP_HDR;
        uint16_t ol = 0;
        uint32_t total = 0;
        uint8_t st = NW_S_UNSUPPORTED;
        if (e->cf_op) st = e->cf_op(e->cf_user, op, body, blen, out, NW_RX_SIZE - NW_RESP_HDR, &ol, &total);
        e->n_local++;
        e->n_requests--;
        respond(e, st, 0, total, out, ol);
        return;
    }
#endif
    if (op == NW_OP_IDENTIFY) {
        size_t t;
        if (blen < 18) { respond(e, NW_S_BAD_REQUEST, 0, 0, NULL, 0); return; }
        memcpy(e->game_id, body, 16);
        e->game_ver = (uint16_t)(body[16] | (body[17] << 8));
        memset(e->title, 0, sizeof e->title);
        for (t = 0; t < 16 && 18 + t < blen && body[18 + t]; t++)
            e->title[t] = (body[18 + t] >= 0x20 && body[18 + t] < 0x7F) ? (char)body[18 + t] : '?';
        e->identified = 1;
        respond(e, NW_S_OK, 0, 0, NULL, 0);
        return;
    }
    if (!e->identified) { respond(e, NW_S_NOT_IDENTIFIED, 0, 0, NULL, 0); return; }
#ifdef NW_CARTFLASH
    const int probe = e->cf_probe_owns && e->cf_probe_owns(e->cf_user, e->game_id) &&
                      (op == NW_OP_GET || op == NW_OP_POST);
#else
    const int probe = 0;
#endif
    if (!probe && !e->online) { respond(e, NW_S_OFFLINE, 0, 0, NULL, 0); return; }
    if (!probe && !take_token(e)) { e->n_rate_limited++; respond(e, NW_S_RATE_LIMITED, 0, 0, NULL, 0); return; }

    nw_call_t c;
    memset(&c, 0, sizeof c);
    c.op = op; c.game_id = e->game_id; c.game_ver = e->game_ver;
    c.body = body; c.body_len = blen;
    switch (op) {
    case NW_OP_GET:
    case NW_OP_POST: {
        const uint8_t *z = memchr(body, 0, blen);
        if (!z) { respond(e, NW_S_BAD_REQUEST, 0, 0, NULL, 0); return; }
        if (!nw_path_ok((const char *)body, 200)) { e->n_denied++; respond(e, NW_S_DENIED, 0, 0, NULL, 0); return; }
        c.path = (const char *)body;
        c.body = z + 1; c.body_len = (uint16_t)(blen - (uint16_t)(z + 1 - body));
        if (op == NW_OP_GET && c.body_len) { respond(e, NW_S_BAD_REQUEST, 0, 0, NULL, 0); return; }
        break;
    }
    case NW_OP_BLOB_READ: if (blen < 7) { respond(e, NW_S_BAD_REQUEST, 0, 0, NULL, 0); return; } break;
    case NW_OP_LB_SUBMIT: if (blen < 13) { respond(e, NW_S_BAD_REQUEST, 0, 0, NULL, 0); return; } break;
    case NW_OP_LB_TOP:    if (blen < 4) { respond(e, NW_S_BAD_REQUEST, 0, 0, NULL, 0); return; } break;
    case NW_OP_SERVICE:   if (blen < 1) { respond(e, NW_S_BAD_REQUEST, 0, 0, NULL, 0); return; } break;
    case NW_OP_UPDATE_CHECK:
    case NW_OP_POLL:
        break;
    default:
        respond(e, NW_S_UNSUPPORTED, 0, 0, NULL, 0);
        return;
    }
    e->state = NW_ESP_CALLING;
    e->call_started = e->now;
#ifdef NW_CARTFLASH
    if (probe) { e->cf_probe_call(e->cf_user, e, e->id, &c); return; }
#else
    (void)probe;
#endif
    e->be.call(e->be.user, e, e->id, &c);
}

static void cancel_current(nw_esp_t *e)
{
    if (e->state == NW_ESP_CALLING && e->be.cancel) e->be.cancel(e->be.user, e->id);
    e->state = NW_ESP_IDLE;
}

static void start_request(nw_esp_t *e, uint8_t id, uint16_t len)
{
    if (e->state != NW_ESP_IDLE) cancel_current(e);
    if (len == 0 || len > NW_TX_SIZE) return;
    e->id = id; e->req_len = len; e->have = 0;
    e->state = NW_ESP_RECEIVING;
    e->last_progress = e->now;
}

void nw_esp_on_packet(nw_esp_t *e, const uint8_t *p, int n)
{
    if (n < 4) return;
    const uint8_t type = p[0], id = p[1];
    const uint16_t a = (uint16_t)(p[2] | (p[3] << 8));
    switch (type) {
    case NW_T_COMMIT:
        start_request(e, id, a);
        break;
    case NW_T_TXDATA: {
        const uint16_t dn = (uint16_t)(n - 4);
        if (e->state != NW_ESP_RECEIVING || id != e->id) break;
        if (a <= e->have && a + dn > e->have) {
            uint16_t k = (uint16_t)(e->have - a), m = (uint16_t)(dn - k);
            if (e->have + m > e->req_len) m = (uint16_t)(e->req_len - e->have);
            memcpy(e->req + e->have, p + 4 + k, m);
            e->have = (uint16_t)(e->have + m);
            e->last_progress = e->now;
        }
        if (e->have == e->req_len) handle_request(e);
        else if (a + dn >= e->req_len) {               /* end of the stream with a gap */
            e->n_txread++;
            pkt(e, NW_R_TXREAD, e->id, e->have, (uint16_t)(e->req_len - e->have), 6);
            e->last_progress = e->now;
        }
        break;
    }
    case NW_T_ABORT:
        if (id == e->id && e->state != NW_ESP_IDLE) cancel_current(e);
        break;
    case NW_T_POSTED:
        if (id == e->id && e->state == NW_ESP_RESPONDING) e->state = NW_ESP_IDLE;
        break;
    case NW_T_RXNAK:
        if (id == e->id && e->state == NW_ESP_RESPONDING) {
            if (a == e->last_nak_have && e->now - e->last_nak_at < NAK_DEBOUNCE_MS) break;
            e->last_nak_have = a; e->last_nak_at = e->now;
            e->n_rxnak++;
            e->resp_sent = a < e->resp_len ? a : e->resp_len;
            send_rest(e);
        }
        break;
    case NW_T_STATE: {
        if (n < 8) break;
        const uint8_t f = p[2];
        const uint16_t act_len = (uint16_t)(p[4] | (p[5] << 8)), rx_have = (uint16_t)(p[6] | (p[7] << 8));
        const int unlocked = f & NW_SF_UNLOCKED;
        if (unlocked != e->fpga_unlocked) { e->identified = 0; e->fpga_unlocked = unlocked; }
        if (!(f & NW_SF_ACTIVE)) {
            if (e->state != NW_ESP_IDLE) cancel_current(e);          /* the FPGA dropped it */
        } else if (f & NW_SF_POSTED) {
            if (id == e->id && e->state == NW_ESP_RESPONDING) e->state = NW_ESP_IDLE;
        } else if (id != e->id || e->state == NW_ESP_IDLE) {
            /* a request this ESP32 does not know (it rebooted): fetch it again */
            e->n_resync++;
            if (e->state != NW_ESP_IDLE) cancel_current(e);
            start_request(e, id, act_len);
            pkt(e, NW_R_TXREAD, id, 0, act_len, 6);
        } else if (e->state == NW_ESP_RESPONDING && e->want_state) {
            e->want_state = 0;               /* our post went missing: resend what the FPGA lacks */
            e->resp_sent = rx_have < e->resp_len ? rx_have : e->resp_len;
            send_rest(e);
        }
        break;
    }
    default:
        break;   /* RXACK: the game is done with the response */
    }
}

void nw_esp_tick(nw_esp_t *e, uint32_t now)
{
    e->now = now;
    if (now - e->last_keepalive >= KEEPALIVE_MS) send_config(e);
    switch (e->state) {
    case NW_ESP_RECEIVING:
        if (now - e->last_progress >= TX_STALL_MS) {
            e->n_txread++;
            pkt(e, NW_R_TXREAD, e->id, e->have, (uint16_t)(e->req_len - e->have), 6);
            e->last_progress = now;
        }
        break;
    case NW_ESP_CALLING:
        if (now - e->call_started >= e->cfg.call_timeout_ms) {
            if (e->be.cancel) e->be.cancel(e->be.user, e->id);
            respond(e, NW_S_TIMEOUT, 0, 0, NULL, 0);
        }
        break;
    case NW_ESP_RESPONDING:
        if (now - e->post_sent_at >= POST_TIMEOUT_MS) {
            if (e->post_tries >= POST_MAX_TRIES) { e->state = NW_ESP_IDLE; break; }
            e->post_sent_at = now; e->post_tries++;
            e->want_state = 1;
            pkt(e, NW_R_STATUS, 0, 0, 0, 1);
        }
        break;
    default:
        break;
    }
}
