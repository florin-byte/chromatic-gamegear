/* cf_sup.h: SAVE TO MY GAMES (lane savecloud1, Shiny Relay 1.1.9+; owner 2026-11-08: manual only, only a game kept ON CHROMATIC).
 *
 * The newest save of one kept game goes up to the paired account's My games: ONE streamed POST of the save's bytes as they are
 *   POST /link_device/my_games/save/upload?rcrc=<8 hex>&rsz=<bytes>&scrc=<8 hex>&t=<title, percent-encoded>
 * (the game's ROM crc32 and size say which game it is, scrc is the save's own crc32: the server stores nothing that does not hash
 * to it). It is NOT a CwlOnline_DeviceCallStart call: that one carries a 256-byte body and answers into a 720-byte buffer, and a
 * save is up to 128 KB with no heap to hold it. Here the bytes are read from the flash in 1 KB pieces and written to the
 * connection as they are read; the answer is one short JSON object (kept in 192 bytes).
 *
 * This file is the decisions only (portable C, host-tested by host/cf_sup_test.c): the order of the steps, the checks that
 * make a bad read or a changed save stop BEFORE anything is sent, the words for every outcome. The connection, the flash read
 * and the clock are hooks (cf_dev.c on the unit). Nothing here ever writes to the library: a failure cannot alter the stored
 * save. The step function runs on the network's HTTP task, a little at a time (it returns 1 while there is more to do).
 */
#ifndef CF_SUP_H
#define CF_SUP_H
#include <stddef.h>
#include <stdint.h>

#define CF_SUP_CHUNK   1024u
#define CF_SUP_FOOTER_MAX 48u           /* a clock game's save is followed by the clock (the emulators' footer) */
#define CF_SUP_TITLE   24
#define CF_SUP_TARGET  160
#define CF_SUP_ANSWER  192
#define CF_SUP_STEP_MAX 4u               /* pieces read or sent in one step (the HTTP task goes back to its queue between steps) */
#define CF_SUP_OPEN_MS 25000u            /* waiting for the handshake's heap, and its retries, at most */

/* how it ended */
enum {
    CF_SUP_OK = 0,                       /* saved */
    CF_SUP_ALREADY,                      /* the account has exactly this save */
    CF_SUP_E_NOSAVE,                     /* this game has no stored save (or not whole) */
    CF_SUP_E_READ,                       /* the flash did not give the save back / it no longer hashes as it was stored */
    CF_SUP_E_NET,                        /* no connection / the connection broke */
    CF_SUP_E_MEM,                        /* the heap for the handshake did not come */
    CF_SUP_E_AUTH,                       /* not paired (any more) */
    CF_SUP_E_FULL8,                      /* the game has 8 saves in My games */
    CF_SUP_E_LIBFULL,                    /* My games has 32 games and this one is not in it */
    CF_SUP_E_BIG,                        /* the server says it is too big */
    CF_SUP_E_RATE,                       /* too many tries */
    CF_SUP_E_SERVER,                     /* the server was busy / said something else */
    CF_SUP_E_OLD,                        /* the server has no such route yet */
    CF_SUP_E_BAD,                        /* the server did not get the save whole */
    CF_SUP_E_STOP,                       /* asked to stop / took too long */
    CF_SUP_E_BUSY,                       /* something else holds the unit */
    CF_SUP_E_PLAYING,                    /* a game is playing */
    CF_SUP_E_PAIR,                       /* not paired */
    CF_SUP_E_WIFI,                       /* no Wi-Fi network saved */
    CF_SUP_N
};

typedef struct {
    void *u;
    /* the save's bytes [off, off + n) as stored (0 = ok) */
    int (*read)(void *u, uint32_t off, uint8_t *b, uint32_t n);
    /* the connection for `target` (path and query) with a body of body_len bytes: 0 open; 1 the heap is not there yet (asked
       again); < 0 it cannot be opened now (no network). */
    int (*open)(void *u, const char *target, uint32_t body_len);
    int (*write)(void *u, const uint8_t *b, uint32_t n);            /* 0 = all of it written */
    /* the end of the request: the HTTP status (0 none) and the answer's start in out (cap bytes, NUL ended); 0 = an answer came */
    int (*answer)(void *u, int *http, char *out, uint32_t cap);
    void (*close)(void *u);                                          /* the connection let go (any time, twice is fine) */
    uint32_t (*now)(void *u);                                        /* milliseconds */
    int (*stop)(void *u);                                            /* nonzero: stop (the player left, the job was cancelled) */
} cf_sup_hooks_t;

enum { CF_SUP_ST_IDLE = 0, CF_SUP_ST_CHECK, CF_SUP_ST_OPEN, CF_SUP_ST_SEND, CF_SUP_ST_ANSWER, CF_SUP_ST_DONE };

typedef struct {
    cf_sup_hooks_t h;
    uint32_t rom_crc, rom_size;          /* which game */
    uint32_t size, want_crc;             /* the save: its bytes and the crc32 the library recorded for it */
    char     title[CF_SUP_TITLE];
    int      st, res;                    /* res: CF_SUP_* once st is DONE */
    uint32_t off, crc;                   /* the pass in progress: bytes done, their crc32 */
    uint32_t t0;                         /* the open's start */
    int      opens, hard;                /* open tries made; of them that could not connect */
    int      pct;                        /* 0..100 for the page */
    int      http;                       /* the answer's status */
    char     target[CF_SUP_TARGET];
    char     ans[CF_SUP_ANSWER];
    uint8_t  buf[CF_SUP_CHUNK];
} cf_sup_t;

/* the request line's path and query for these facts into out; 0, or -1 when it does not fit */
int  cf_sup_target(char *out, size_t n, uint32_t rom_crc, uint32_t rom_size, uint32_t save_crc, const char *title);
/* begin: want_crc and size are the library's record of the newest save (cf_lib_save_t crc, size); title as the unit shows it */
void cf_sup_begin(cf_sup_t *s, const cf_sup_hooks_t *h, uint32_t rom_crc, uint32_t rom_size, uint32_t size, uint32_t want_crc, const char *title);
/* one piece of work: 1 = call again soon, 0 = ended (s->st == CF_SUP_ST_DONE, s->res) */
int  cf_sup_step(cf_sup_t *s);
/* the answer of the server as an outcome (CF_SUP_*) from its status and body */
int  cf_sup_outcome(int http, const char *body);
/* the page's words for an outcome: "ROW1|ROW2|ROW3", at most 47 characters (the card task's result buffer is 48) */
const char *cf_sup_words(int res);
/* the outcome the unit may be asked for before anything starts (0 = go): paired, a Wi-Fi network saved, nothing playing */
int  cf_sup_gate(int paired, int wifi_saved, int playing, int save_stored);
#endif
