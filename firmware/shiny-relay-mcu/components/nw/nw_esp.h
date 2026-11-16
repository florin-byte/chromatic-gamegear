/* nw_esp.h: ESP32-side network window service, portable reference (no ESP-IDF).
 *
 * Speaks the FPGA transport (NETWORK_WINDOW.md "FPGA <-> ESP32"), reassembles
 * the game's request, applies the security policy, hands the call to a
 * backend (on the device: HTTPS to the Shiny Gen gateway with the device
 * token; in tests: a mock), and streams the response back.
 *
 * Porting to mcu/: nw_esp_on_packet() from fpga_rx.c ProcessMessage for
 * address 0x0B; send() = fpga_tx packet to address 0x11; nw_esp_tick() from
 * the CWL task every few ms; the backend runs the HTTP call on its own task
 * and calls nw_esp_complete() from there (guard with the CWL mutex).
 */
#ifndef NW_ESP_H
#define NW_ESP_H
#include <stdint.h>
#include <stddef.h>
#include "nw_proto.h"

struct nw_esp;

typedef struct {
    uint8_t        op;
    const uint8_t *game_id;      /* 16 bytes, from IDENTIFY */
    uint16_t       game_ver;
    const char    *path;         /* GET / POST: validated, relative to the game's service root */
    const uint8_t *body;         /* op body after the path (POST content, SERVICE, LB_*, BLOB_READ) */
    uint16_t       body_len;
} nw_call_t;

typedef struct {
    void *user;
    /* Start a call. Must lead to exactly one nw_esp_complete(esp, id, ...),
     * possibly from inside this function, unless cancel() comes first. */
    void (*call)(void *user, struct nw_esp *esp, uint8_t id, const nw_call_t *c);
    void (*cancel)(void *user, uint8_t id);
} nw_backend_t;

typedef struct {
    uint8_t  fw_major, fw_minor;
    uint16_t rate_per_s;          /* request budget, token bucket */
    uint16_t rate_burst;
    uint32_t call_timeout_ms;     /* the backend's answer deadline; then TIMEOUT */
} nw_esp_config_t;

enum { NW_ESP_IDLE, NW_ESP_RECEIVING, NW_ESP_CALLING, NW_ESP_RESPONDING };

typedef struct nw_esp {
    nw_esp_config_t cfg;
    nw_backend_t    be;
    void          (*send)(void *user, const uint8_t *p, int n);   /* packet to UART address 0x11 */
    void           *send_user;

    int      enabled, online, notify;
    int      awake;               /* CONFIG AWAKE: the MCU stays awake (no wake preamble); clear it before light sleep */
    uint32_t now, last_keepalive;
    /* service session (one game at a time; cleared on lock / GB reset) */
    int      fpga_unlocked;
    int      identified;
    uint8_t  game_id[16];
    uint16_t game_ver;
    char     title[17];
    uint32_t tokens_milli, last_refill;
    /* current request */
    int      state;
    uint8_t  id;
    uint16_t req_len, have;
    uint8_t  req[NW_TX_SIZE];
    uint32_t last_progress, call_started;
    /* response */
    uint8_t  resp[NW_RX_SIZE];
    uint16_t resp_len, resp_sent;
    uint32_t post_sent_at;
    int      post_tries;
    int      want_state;          /* a STATUS query after a post timeout is outstanding */
    uint32_t last_nak_at;
    uint16_t last_nak_have;
    /* Ops the ESP32 answers itself with no gateway call, no IDENTIFY and no
     * rate limit (the SHLINK ops 0x0B-0x0D, nw_link.c): returns an NW status,
     * the answer body in out. NULL = UNSUPPORTED. */
    uint8_t (*local_op)(void *user, uint8_t op, const uint8_t *body, uint16_t blen,
                        uint8_t *out, uint16_t cap, uint16_t *out_len);
    void    *local_user;
    uint32_t n_local;
#ifdef NW_CARTFLASH
    /* The cart flash ops 0x20-0x21 (lane cartflash1, cartflash/cf_proto.h), answered
     * by the ESP32 itself like the SHLINK ops; total = the response's total_len. */
    uint8_t (*cf_op)(void *user, uint8_t op, const uint8_t *body, uint16_t blen,
                     uint8_t *out, uint16_t cap, uint16_t *out_len, uint32_t *total);
    void    *cf_user;
    /* The read-only cart probe over USB (no Wi-Fi, release builds): when cf_probe_owns()
     * says this IDENTIFY game id is the probe's and a `cart probe` session is on, its
     * GET/POST skip the online check and the rate limit and go to cf_probe_call() (which
     * answers with nw_esp_complete(), like a backend) instead of the gateway. */
    int     (*cf_probe_owns)(void *user, const uint8_t *game_id);
    void    (*cf_probe_call)(void *user, struct nw_esp *e, uint8_t id, const nw_call_t *c);
#endif
    /* stats */
    uint32_t n_requests, n_txread, n_rxnak, n_resync, n_denied, n_rate_limited;
} nw_esp_t;

void nw_esp_init(nw_esp_t *e, const nw_esp_config_t *cfg, const nw_backend_t *be,
                 void (*send)(void *user, const uint8_t *p, int n), void *send_user);
void nw_esp_set_enabled(nw_esp_t *e, int on);     /* OSD switch: SVC */
void nw_esp_set_online(nw_esp_t *e, int online);  /* Wi-Fi + gateway reachable */
void nw_esp_set_notify(nw_esp_t *e, int pending); /* e.g. an update is available */
void nw_esp_set_awake(nw_esp_t *e, int awake);    /* CONFIG AWAKE (default 1) */
void nw_esp_on_packet(nw_esp_t *e, const uint8_t *p, int n);
void nw_esp_tick(nw_esp_t *e, uint32_t now_ms);
void nw_esp_complete(nw_esp_t *e, uint8_t id, uint8_t status, uint8_t handle,
                     uint32_t total_len, const uint8_t *body, uint16_t body_len);
/* Forget all state as after a reboot (for tests). */
void nw_esp_reboot(nw_esp_t *e);
/* The SHLINK ops (lane chromshlink1): op codes answered by local_op. */
#define NW_OP_LINK_FIRST 0x0Bu
#define NW_OP_LINK_LAST  0x10u   /* 1.0.2: 0x0E LINK_ROOM, 0x0F LINK_STATUS, 0x10 LINK_UI */

/* Policy helpers (exported for tests). */
int  nw_path_ok(const char *path, size_t max_len);

#endif
