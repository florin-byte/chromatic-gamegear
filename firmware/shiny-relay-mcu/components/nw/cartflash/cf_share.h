/* cf_share.h: one Chromatic sends a game straight to another nearby Chromatic (lane share1, Shiny Relay 1.1.0).
 * No website, no server: the two units talk over the Wi-Fi radio with ESP-NOW (components/cwl/cwl_share.c), on one
 * channel, with no access point. This file is the protocol, portable C (the same code in the Mac host test,
 * host/cf_share_test.c); cf_share_dev.c is the unit's side (the sources, the places a game lands, the screens' view).
 *
 * The person:  B (gets the game): SYSTEM > DOWNLOAD > B: FROM A CHROMATIC. B shows a 4-digit code.
 *              A (has the game):  SHARE on a game (CARTRIDGE, ON CHROMATIC or CARD), types B's code, A: SEND.
 *              B shows who sends what and asks where it goes (ON CHROMATIC / CARD / PLAY NOW): A: TAKE IT.
 *              Both show the progress; B checks the whole game (crc32 + sha256) before it counts.
 *
 * Pairing: B broadcasts a BEACON every 100 ms on its channel (the least busy of 1 / 6 / 11, by one scan) with tag = crc32("SHR1" | the 4 digits | B's MAC). A sweeps
 * the channels (its own first, then 1, 6, 11, then the rest) until it hears a beacon whose tag matches the typed code and the beacon's sender, then
 * talks to that MAC only (unicast: the radio's own ACK + retries). A chooses the session id (random, never 0).
 *
 * Frames (little-endian; at most 250 bytes, one ESP-NOW frame each):
 *   0 'S'  1 'h'  2 type  3 version (1)  4 sid u32   8.. the body   last 4: crc32 of everything before it
 *   BEACON  B -> all   tag u32, unit u16 (the last two MAC bytes: "CHROMATIC 1A2B")
 *   HELLO   A -> B     tag u32, unit u16, the offer (cf_sh_offer_t: flags, source, cart type / RAM code, sizes, crc32,
 *                      title, file name)                     repeated until B answers, then every second while B asks
 *   REPLY   B -> A     verdict (WAIT: the person is choosing, TAKE, NO, BUSY), the place, have u32 (0)
 *   DATA    A -> B     off u32, then up to CF_SH_DATA_MAX bytes of the stream
 *   ACK     B -> A     have u32 (every byte below it taken, in order), win u16 (frames A may send beyond have), flags
 *   END     A -> B     total u32, the game's crc32 + sha256, the save's crc32          repeated until B's final RESULT
 *   RESULT  B -> A     code (0 ok), stage (0 still finishing, 1 final)
 *   CANCEL  either     why
 * The stream is the game's bytes as A keeps them (an SGZ1 container stays packed: B keeps it as is ON CHROMATIC), then
 * the save's bytes (WITH SAVE). B takes bytes only in order (go-back-N): a frame past `have` is dropped and answered by an
 * ACK with the NAK flag; A goes back to `have`. A may have at most `win` frames beyond `have` in the air, and B's queue
 * holds more than that, so a busy sink (the FPGA flash erasing) never makes B drop frames. B, when nothing new came in
 * order for CF_SH_GAP_MS, says where it is with the NAK flag (a lost last frame); no ACK progress for CF_SH_RTO_MS: A sends
 * again from `have` (one frame per CF_SH_PROBE_MS while nothing answers).
 *
 * Cuts: the radio gone (walked away, the other side off) for less than CF_SH_LOST_MS: both sides wait and go on where
 * they were (A from B's `have`). Longer: both give up cleanly (B lets go of what it wrote: nothing half-written is kept).
 * B restarted: it does not know the session: it answers CANCEL (UNKNOWN), A stops at once.
 */
#ifndef CF_SHARE_H
#define CF_SHARE_H
#include <stdint.h>

#define CF_SH_VER        1
#define CF_SH_FRAME_MAX  250u
#define CF_SH_DATA_MAX   234u            /* 250 - 8 header - 4 offset - 4 crc */
#define CF_SH_CODE_LEN   4
#define CF_SH_RING       8192u           /* A: the stream bytes read but not yet taken by B (a power of 2) */
#define CF_SH_SEG        1024u           /* ... in pieces of 1 KiB (separate allocations: a unit's heap in fragments, unit 2 10-24: largest 4,096) */
#define CF_SH_NSEG       (CF_SH_RING / CF_SH_SEG)
#define CF_SH_WIN        16u             /* frames beyond `have` (B's queue holds CF_SH_QUEUE; 12 measured: 30 % slower, drops) */
#define CF_SH_QUEUE      24u
#define CF_SH_RTO_MS     1500u           /* longer than a busy sink's stall (the FPGA flash erasing): B asks for a gap itself */
#define CF_SH_PROBE_MS   400u
#define CF_SH_LOST_MS    (60u * 1000u)   /* the radio gone this long during a transfer: given up */
#define CF_SH_FIND_MS    (90u * 1000u)   /* A looks for B's beacon this long */
#define CF_SH_ASK_MS     (180u * 1000u)  /* B's person chooses within this */
#define CF_SH_WAIT_MS    (10u * 60u * 1000u)   /* B shows its code this long (then NO ANSWER) */
#define CF_SH_HELLO_MS   300u
#define CF_SH_BEACON_MS  100u
#define CF_SH_DWELL_MS   220u            /* A's sweep: listen on each channel this long */
#define CF_SH_END_MS     300u
#define CF_SH_ACK_MS     20u
#define CF_SH_GAP_MS     300u            /* B: nothing new in order for this long while bytes are missing: an ACK with NAK */
#define CF_SH_FINISH_MS  (120u * 1000u)  /* B's finish (the library's index, the card's file) at most */

enum { CF_SH_T_BEACON = 1, CF_SH_T_HELLO, CF_SH_T_REPLY, CF_SH_T_DATA, CF_SH_T_ACK, CF_SH_T_END, CF_SH_T_RESULT, CF_SH_T_CANCEL };
enum { CF_SH_V_WAIT = 0, CF_SH_V_TAKE, CF_SH_V_NO, CF_SH_V_BUSY };
/* where it goes on B */
enum { CF_SH_DEST_LIB = 0, CF_SH_DEST_CARD, CF_SH_DEST_PLAY, CF_SH_NDEST };
/* where it comes from on A */
enum { CF_SH_SRC_CART = 0, CF_SH_SRC_LIB, CF_SH_SRC_CARD };
#define CF_SH_F_PACKED 0x01u             /* the game's bytes are an SGZ1 container */
#define CF_SH_F_SAVE   0x02u             /* the save follows the game */
#define CF_SH_ACK_NAK  0x01u
/* why it ended (both sides; the screens' words come from these) */
enum { CF_SH_OK = 0, CF_SH_E_NOTFOUND, CF_SH_E_DECLINED, CF_SH_E_BUSY, CF_SH_E_LOST, CF_SH_E_CANCELLED, CF_SH_E_STOPPED,
       CF_SH_E_READ, CF_SH_E_WRITE, CF_SH_E_CHECK, CF_SH_E_UNKNOWN, CF_SH_E_NOANSWER, CF_SH_E_ROOM, CF_SH_E_RADIO,
       CF_SH_E_START,                    /* m110s: PLAY NOW: everything came and checked, the game did not start on B */
       CF_SH_NE };

typedef struct {
    uint8_t  flags;                      /* CF_SH_F_* */
    uint8_t  src;                        /* CF_SH_SRC_* */
    uint8_t  cart_type, cart_ram;        /* the game's header 0x147 / 0x149 (the save it needs) */
    uint32_t game_size;                  /* bytes of the game sent (the container, or the ROM) */
    uint32_t rom_size, rom_crc32;        /* the game itself (rom_crc32 0: not known before the end) */
    uint32_t save_size;                  /* bytes of save after the game (0: none) */
    char     title[17];
    char     file[33];                   /* a name for the card ("" = from the title) */
} cf_sh_offer_t;

typedef struct {                         /* what both sides keep of the hashes */
    uint32_t crc, scrc;
    uint8_t  sha[32];
} cf_sh_sum_t;

/* ---- frames (exposed for the tests) */
uint32_t cf_sh_tag(const char code[CF_SH_CODE_LEN], const uint8_t mac[6]);
int  cf_sh_code_ok(const char *code);    /* 4 digits */
/* builds a frame into f (CF_SH_FRAME_MAX), returns its length */
uint32_t cf_sh_build(uint8_t *f, uint8_t type, uint32_t sid, const uint8_t *body, uint32_t n);
/* 1 = a frame of ours with a good crc: *type, *sid, the body and its length */
int  cf_sh_parse(const uint8_t *f, uint32_t n, uint8_t *type, uint32_t *sid, const uint8_t **body, uint32_t *bn);
void cf_sh_offer_put(uint8_t *b, const cf_sh_offer_t *o);   /* CF_SH_OFFER_LEN bytes */
int  cf_sh_offer_get(const uint8_t *b, uint32_t n, cf_sh_offer_t *o);
#define CF_SH_OFFER_LEN 70u

/* ---- the radio and the clock, as each side sees them */
typedef struct {
    void *u;
    int  (*send)(void *u, const uint8_t dst[6], const uint8_t *f, uint32_t n);   /* 0 = handed to the radio; -1 = no room now */
    void (*channel)(void *u, uint8_t ch);                                           /* A's sweep (may be NULL) */
    void (*log)(void *u, const char *m);                                            /* may be NULL */
} cf_sh_radio_t;

/* ================================================================ A: the sender */
enum { CF_SH_TX_IDLE, CF_SH_TX_FIND, CF_SH_TX_HELLO, CF_SH_TX_ASKED, CF_SH_TX_SEND, CF_SH_TX_END, CF_SH_TX_DONE, CF_SH_TX_FAIL };
typedef struct {
    cf_sh_radio_t r;
    /* the stream's bytes [off, off + n) in order, each asked once: the game's, then the save's. 0 ok */
    int  (*read)(void *u, uint32_t off, uint8_t *b, uint32_t n);
    int  st, err;
    uint32_t now, t0, t_state, t_heard, t_ack, t_hello, t_probe, t_end, t_sweep;
    uint32_t sid, tag_seen;
    char     code[CF_SH_CODE_LEN + 1];
    uint8_t  peer[6], chan, chan0, unit_lo, unit_hi, sweep_i;
    uint16_t peer_unit;
    cf_sh_offer_t offer;
    uint32_t total;                      /* game_size + save_size */
    uint32_t have, next, win;            /* B's have; the next byte to send; B's window in frames */
    uint32_t ring_end;                   /* bytes read from the source (the ring holds [ring_end - CF_SH_RING, ring_end)) */
    uint8_t *seg[CF_SH_NSEG];            /* the ring: CF_SH_NSEG pieces of CF_SH_SEG bytes (the host's) */
    uint8_t  dest;                       /* the place B chose */
    uint8_t  b_stage;                    /* B's RESULT stage */
    void    *sha;                        /* the game's sha256 as read (the host's context: cf_share.c owns its use) */
    cf_sh_sum_t sum;
    int      sum_done;
    /* counters (the bench, the report) */
    uint32_t n_data, n_resend, n_rewind, n_nak, n_noroom, n_probe, n_frames_in, n_bad;
    uint32_t t_first, t_last;            /* the first DATA and B's final RESULT */
} cf_sh_tx_t;

/* seg: CF_SH_NSEG pieces of CF_SH_SEG bytes; sha: storage for a SHA-256 context (cf_sh_sha_size() bytes). mac: this unit's. */
uint32_t cf_sh_sha_size(void);
void cf_sh_tx_init(cf_sh_tx_t *t, const cf_sh_radio_t *r, int (*read)(void *u, uint32_t off, uint8_t *b, uint32_t n),
                   uint8_t *const seg[CF_SH_NSEG], void *sha, const uint8_t own_mac[6], uint8_t chan0, uint32_t sid, uint32_t now);
void cf_sh_tx_start(cf_sh_tx_t *t, const char code[CF_SH_CODE_LEN], const cf_sh_offer_t *o, uint32_t now);
void cf_sh_tx_frame(cf_sh_tx_t *t, const uint8_t src[6], const uint8_t *f, uint32_t n, uint32_t now);
void cf_sh_tx_tick(cf_sh_tx_t *t, uint32_t now);
void cf_sh_tx_cancel(cf_sh_tx_t *t, uint32_t now);     /* the person stopped it (B: STOP) */
int  cf_sh_tx_busy(const cf_sh_tx_t *t);              /* still talking (not DONE / FAIL / IDLE) */

/* ================================================================ B: the receiver */
enum { CF_SH_RX_IDLE, CF_SH_RX_WAIT, CF_SH_RX_ASK, CF_SH_RX_DATA, CF_SH_RX_CHECK, CF_SH_RX_FINISH, CF_SH_RX_DONE, CF_SH_RX_FAIL };
typedef struct {
    cf_sh_radio_t r;
    /* a new offer: the host shows it (the person answers with cf_sh_rx_answer) */
    void (*on_offer)(void *u, const cf_sh_offer_t *o, uint16_t unit);
    /* the place chosen: get ready (0), or say why not (CF_SH_E_ROOM / _WRITE) */
    int  (*begin)(void *u, const cf_sh_offer_t *o, uint8_t dest);
    /* the stream's bytes in order, [off, off + n). 0 ok */
    int  (*write)(void *u, uint32_t off, const uint8_t *b, uint32_t n);
    /* every byte came and the hashes are right (ok 1) or not (ok 0): the host keeps it (or lets it go). 0 done, 1 call
       again (still working), < 0 failed (CF_SH_E_*, negated) */
    int  (*finish)(void *u, int ok);
    int  st, err;
    uint32_t now, t0, t_state, t_heard, t_beacon, t_ack, t_nak, t_take;
    uint32_t sid;
    char     code[CF_SH_CODE_LEN + 1];
    uint8_t  own[6], peer[6];
    uint32_t tag;
    uint16_t peer_unit;
    cf_sh_offer_t offer;
    uint32_t total, have, acked;
    uint8_t  dest;
    void    *sha;
    cf_sh_sum_t sum, want;
    int      have_end, checked_ok;
    uint32_t n_data, n_dup, n_ooo, n_acks, n_bad;
    uint32_t t_first, t_end;
} cf_sh_rx_t;

void cf_sh_rx_init(cf_sh_rx_t *x, const cf_sh_radio_t *r, void *sha, const uint8_t own_mac[6], uint32_t now);
void cf_sh_rx_start(cf_sh_rx_t *x, const char code[CF_SH_CODE_LEN], uint32_t now);   /* beacons on (the host's channel) */
void cf_sh_rx_frame(cf_sh_rx_t *x, const uint8_t src[6], const uint8_t *f, uint32_t n, uint32_t now);
void cf_sh_rx_tick(cf_sh_rx_t *x, uint32_t now);
/* the person: take = 1 (with dest) or 0 (NO) */
void cf_sh_rx_answer(cf_sh_rx_t *x, int take, uint8_t dest, uint32_t now);
void cf_sh_rx_cancel(cf_sh_rx_t *x, uint32_t now);
int  cf_sh_rx_busy(const cf_sh_rx_t *x);

const char *cf_sh_why(int e);
#endif
