/* cf_install.h: "Send to my Chromatic" over Wi-Fi, the device's whole path (lane cartflash1;
 * receive mode + code: lane wifirecv1, owner-approved design 2026-10-14).
 * Owner rulings 2026-10-11: ANY ROM; NO backup and NO staging on the ESP32 (the owner backs the
 * cart up to their own computer over USB first, docs/CART_BACKUP_USB_PROTOCOL.md); the image
 * is STREAMED straight to the cart (cf_esp.c CF_JOB_STREAM).
 *
 * NOTHING polls in the background any more (wifirecv1). The player asks for a game on the unit:
 * SYSTEM > DOWNLOAD > RECEIVE GAME. Then, and only then:
 *   hold     the link stops (game link, room, relay seat, the network window): its heap is for TLS
 *   register POST /link_device/register {} once, if the unit has no registration token (NVS): an
 *            account-less device credential. Pairing is NOT needed; every call below uses it.
 *   start    POST /link_device/recv/start {}      -> {"code": "ABC234", "expires_in": 600, "poll_ms"}
 *   wait     the code on the screen, WAITING FOR A GAME; the sender types it on the site
 *   poll     POST /link_device/recv/poll {"code"} -> {"session": "waiting"} | {"session": "expired"} |
 *            {"session": "sent", "pending": true, "id", "title"?, "size", "crc32", "sha256",
 *             "enc_alg": "AES-256-CTR", "enc_key", "enc_iv", "blob_url", ...}   (TLS)
 *   peek     the image's first bytes (one ranged GET of the ciphertext over plain http, decrypted)
 *   write    no question (the player asked for it): BATTERY LOW waits for the charger, or A (INSTALL
 *            ANYWAY, 1.0.5) writes on the low battery; then the
 *            stream job: first block erased first, blocks journalled, the cart read back and
 *            checked against the record's crc32 + sha256, the header written last
 *   report   POST /link_device/cart_install/ack {"id", "state": "downloading"|"resuming"|"installed"|"declined"|"failed", "detail"}
 *            ("failed" is final: the server deletes the image. An install that can still be resumed never sends it.)
 *   cancel   B before the write starts: POST /link_device/recv/cancel {"code"} (or "declined" once a game came)
 *   resume   POST /link_device/recv/resume {"id"} -> the same offer, for an install bound to this device
 *
 * An install that did not finish (link gone, power lost, a failed check) stays in the journal:
 * SYSTEM > DOWNLOAD shows INSTALL NOT FINISHED (A: RESUME  B: CANCEL). A pause in receive mode
 * resumes by itself a few times (the resume route, not a poll); after a restart it waits for A.
 *
 * Portable C, driven by cf_install_tick() from the nw task; nothing here blocks.
 */
#ifndef CF_INSTALL_H
#define CF_INSTALL_H
#include <stdint.h>

#define CF_INST_RETRY_MS     (60u * 1000u)         /* a paused install: the next automatic resume */
#define CF_INST_API_MS       (60u * 1000u)         /* one device API call at most */
#define CF_INST_PEEK_MS      (60u * 1000u)
#define CF_INST_MAX_AUTO     3                     /* automatic resumes of one install before it waits for A */
#define CF_INST_REPORT_TRIES 6
#define CF_INST_REPORT_QUICK 30       /* gamecomp1: a report that cannot start, tried again every 2 s (a minute) with receive mode kept */
#define CF_INST_REPORT_SOON_MS (5u * 1000u)    /* a final report that could not start at once (the image connection still closing) */
#define CF_INST_WRITABLE     2031616u              /* the ModRetro cart: 2 MiB less its protected top block */
/* receive mode (wifirecv1) */
#define CF_RX_HOLD_MS        (6u * 1000u)          /* the link session ends (its heap given back): at most this long */
#define CF_RX_WIFI_MS        (30u * 1000u)         /* no Wi-Fi connection within this: NO WI-FI */
#define CF_RX_POLL_MS        4000u                 /* between polls (the server's poll_ms, 2-10 s, wins) */
#define CF_RX_BUSY_MAX_MS    60000u                /* 1.0.3: the longest 429 backoff */
#define CF_RX_BUSY_FIRST_MS  4000u
#define CF_RX_BUSY_GIVEUP_MS 600000u               /* a start refused (429) this long: TOO MANY TRIES */
#define CF_RX_POLL_FAILS     5                     /* polls in a row with no answer: NO CONNECTION */
#define CF_RX_PEEK_TRIES     3                     /* the image's header did not come: poll again, this often */
#define CF_RX_POWER_MS       (10u * 60u * 1000u)   /* BATTERY LOW waits for the charger this long */
#define CF_RX_ACK_WAIT_MS    (20u * 1000u)         /* 'downloading' waits this long for the header read's connection to close (unit 2, 2026-10-14) */
#define CF_RX_CODE_LEN       6
#define CF_RX_CHAIN_MS       (10u * 60u * 1000u)   /* 1.0.8: after a backup the same code waits this long for the install */
#define CF_RX_ALPHABET       "BCDFGHJKMNPQRSTVWXZ23456789"   /* the server's; no vowels, no 0 1 O I L U Y */

enum { CI_IDLE, CI_RX_HOLD, CI_RX_REG, CI_RX_START, CI_RX_WAIT, CI_RX_POLL, CI_RX_RESUME, CI_RX_PAUSED,
       CI_RX_EXPIRED, CI_RX_FAIL, CI_RX_DRAIN, CI_PEEK, CI_WAIT_POWER, CI_ACKDL, CI_ARMED, CI_REPORT, CI_REPORT_WAIT,
       CI_RX_SIDE,       /* lane ui110dev: GET GAMES's own call (a catalog page, an order) between two polls */
       CI_NSTATES };
#define CF_SIDE_PATH 40
#define CF_SIDE_BODY 176
#define CF_CAT_RENEW_MAX 12   /* GET GAMES: codes asked again by themselves (each lasts 10 minutes) before the page says EXPIRED */
/* What SYSTEM > DOWNLOAD shows (cf_install_view): the machine owns the page */
enum { CV_MENU,          /* RECEIVE GAME (A)                                   */
       CV_GETTING,       /* GETTING READY: the hold, registering, asking for a code (B: CANCEL) */
       CV_CODE,          /* the code + WAITING FOR A GAME + minutes left (B: CANCEL) */
       CV_EXPIRED,       /* CODE EXPIRED (A: NEW CODE  B: BACK)                */
       CV_FAIL,          /* two rows of why (A: TRY AGAIN  B: BACK)            */
       CV_ARRIVED,       /* a game came: its title, reading its header (B: CANCEL) */
       CV_BATTERY,       /* BATTERY LOW / CHARGE IT TO INSTALL, waits for power (A: INSTALL ANYWAY  B: CANCEL) */
       CV_INSTALLING,    /* INSTALLING <title> NN% (no buttons; the host fills NN%) */
       CV_RESULT,        /* INSTALLED / NOT INSTALLED + two rows of why (A: OK) */
       CV_UNFINISHED };  /* INSTALL NOT FINISHED <title> (A: RESUME  B: CANCEL) */
typedef struct {
    int      kind;
    char     title[24];    /* (m110t: was 17, which cut "NOT  ON  THE  CARD") */
    char     note[48];     /* CV_FAIL / CV_RESULT: "ROW1|ROW2" (m110t: was 40, which cut "NOT  ENOUGH  MEMORY|RESTART,  SEND  AGAIN") */
    char     code[CF_RX_CODE_LEN + 1];
    unsigned mins;         /* CV_CODE: whole minutes left (rounded up) */
    int      anyway;       /* CV_INSTALLING: the player chose INSTALL ANYWAY on a low battery (1.0.5) */
    int      backup;       /* CV_INSTALLING: 1.0.6, a backup of the cart to the website (BACKING UP, B: STOP) */
} cf_view_t;
#define CF_INST_RESULT_MS    (10u * 60u * 1000u)
#define CF_INST_RESULT_REOPEN_MS (8u * 1000u)    /* after the cart's restart at the job's end */

/* The image travels over a PLAIN http connection as AES-256-CTR ciphertext (owner-approved
   2026-10-12, docs/CART_INSTALL_WIRE_FORMAT.md): the poll (TLS) carries the key, the first counter
   block and the blob URL. The key lives in RAM only, for the install being asked about or written,
   and is wiped when the install leaves the machine (cf_offer_wipe). Never printed, never stored. */
#define CF_BLOB_URL_MAX 200
typedef struct {
    char     id[33];
    char     title[17];
    char     sha256[65];
    uint32_t size, crc32;
    char     blob_url[CF_BLOB_URL_MAX + 1];   /* http://host/gbnet/v1/install/<id>/blob/<token> */
    uint8_t  key[32], iv[16];
    uint8_t  kind;                            /* 1.0.5: CF_KIND_ROM, or CF_KIND_PATCH (a patch bundle the unit applies to its own cart) */
    /* 1.0.9, CF_KIND_ROMSAVE: the image is the game (rom_size bytes, its own crc32 / sha256) then its 32 KiB save */
    uint32_t rom_size, rom_crc32;
    char     rom_sha256[65];
    /* lane sdcard1: a send to the unit's microSD card ("sd": "keep" | "play", with "file"): kinds play and save */
    uint8_t  sd;                              /* CF_SD_NONE / KEEP / PLAY */
    char     file[61];                        /* the file's name on the card (the server's, made safe again here) */
    /* lane gamecomp1, CF_KIND_PLAY with "z": "sgz1": the image is an SGZ1 container (cf_sgz.h; size / crc32 / sha256 are
       the container's, what is downloaded); rom_size / rom_crc32 / rom_sha256 are then the game's ("game_*") */
    uint8_t  z;
    /* lane ui110dev, CF_KIND_PLAY with no card send and "lib": "keep" (CF_LIB_KEEP: keep it ON CHROMATIC, do not start it) or
       "none" (CF_LIB_NONE: play it, keep nothing); 0 = keep it when it fits, and play it (1.1.0's play send) */
    uint8_t  lib;
    /* lane mygames1 (caps psave1), CF_KIND_PLAY with "save_size": the game's save comes WITH it, after the game's object (the
       ROM or its container): size / crc32 / sha256 are then the whole download's, obj_size / obj_crc32 the game's object's,
       save_size / save_crc32 the save's (the save RAM the game asks for; onto a card: the .sav file as it is). 0 = no save */
    uint32_t obj_size, obj_crc32, save_size, save_crc32;
} cf_offer_t;
/* lane mygames1: the game's part of a play offer's download (all of it when no save comes along), and its crc32 */
#define CF_OBJ_SIZE(o)  ((o)->save_size ? (o)->obj_size : (o)->size)
#define CF_OBJ_CRC(o)   ((o)->save_size ? (o)->obj_crc32 : (o)->crc32)
#define CF_SD_GAME_MAX  0x800000u             /* lane mygames1: a game kept on the card only (too big to play): 8 MiB */
#define CF_LIB_KEEP 1
#define CF_LIB_NONE 2
#define CF_Z_MAX      0x1F0000u               /* a container staged whole: the staging area (CF_STAGE_ROM_MAX); a bigger one goes only into a library (caps lib1) */
#define CF_SD_NONE 0
#define CF_SD_KEEP 1                          /* bank the game (or the save) on the card */
#define CF_SD_PLAY 2                          /* ... and play the game now */
#define CF_SD_SAVE_MAX 131120u                /* a card save: 128 KiB of save RAM + the 48-byte clock footer */
#define CF_KIND_ROM   0
#define CF_KIND_PATCH 1
#define CF_KIND_SAVE   3                      /* 1.0.9: a 32 KiB save file written into the cart's save RAM (the game is not touched) */
#define CF_KIND_ROMSAVE 4                     /* 1.0.9: a game with its save: the game written, then the save instead of an empty one */
#define CF_KIND_PLAY  5                       /* 1.1.0 (FPGA 18.30): a game played from the FPGA's RAM with no cartridge (cf_rc_dev.c):
                                                 kept in the staging area when it fits, else streamed into PSRAM */
#define CF_PLAY_MAX   0x400000u               /* the RAM cart's 256 banks */
#define CF_KIND_BACKUP 2                      /* 1.0.6 (owner 2026-10-20): read this cart and stream it to the sender's browser
                                                 (the blob URL is the upload's; nothing is written to the cart) */
#define CF_PATCH_MIN  64u                     /* a patch bundle's size: 64 B .. 512 KiB (cf_bps.h) */
#define CF_PATCH_MAX  0x80000u
/* recv/start's body: what this firmware can take (the server sends a patch only to a unit that says patch1).
   lane sdcard1: the host's start_body (may be NULL) adds the card (",sd1" + "sd" summary) when one is mounted */
#define CF_RX_CAPS "patch1,backup1,chain1,save1,play1,z1,keep1,psave1"   /* z1 (lane gamecomp1): a play send may come as an SGZ1 container;
                                                                      keep1 (lane ui110dev): its "lib": "keep" / "none" */
#define CF_RX_START_BODY "{\"caps\":\"patch1,backup1,chain1,save1,play1,z1,keep1,psave1\"}"   /* chain1 (1.0.8): the code is kept after a backup; play1 (1.1.0) */
void cf_offer_wipe(cf_offer_t *o);              /* zero it, key first (not optimised away) */
/* AES-256-CTR over bytes off..off+n of the image, in place (wire format section 3: counter block j =
   iv + j, a 128-bit big-endian number, the carry through all 16 bytes). The block cipher is the
   caller's: ecb(u, in16, out16) = AES-256-ENCRYPT(key, in16), 0 ok (ESP32: mbedtls, the hardware AES;
   host test: OpenSSL). Portable so the counter rule is tested on the Mac against OpenSSL's CTR. */
int cf_ctr_xor(int (*ecb)(void *u, const uint8_t in[16], uint8_t out[16]), void *u, const uint8_t iv[16], uint32_t off, uint8_t *b, uint32_t n);

typedef struct {
    void *user;
    /* the device API on the link task, with the unit's REGISTRATION token (never the pairing's):
       start = 1 when started; result = 0 while running, else the HTTP code (-1 no connection).
       "/link_device/register" goes without a token; the host keeps the token from its answer and
       hands back "{}" (the token never reaches this machine) */
    int  (*api_start)(void *u, const char *path, const char *body, char *out, int cap);
    int  (*api_result)(void *u);
    const char *(*gateway_base)(void *u);             /* "https://host/gbnet/v1" */
    int  (*registered)(void *u);                     /* 1 = the unit has a registration token */
    void (*reg_forget)(void *u);                     /* the server refused it (401): drop it, register again */
    int  (*fpga_ok)(void *u);                        /* 1 = a console chip with the cart writer in its flash (18.24+) */
    int  (*wifi)(void *u);                           /* 0 no network saved, 1 connecting, 2 connected */
    void (*hold)(void *u, int on);                   /* receive mode: the link and its heap give way (on), come back (off) */
    int  (*held)(void *u);                           /* 1 = the link session has ended for the hold */
    /* the image's header, by one ranged GET: start = 1; result = -1 running, 0 ok (title + note filled), else failed */
    int  (*peek_start)(void *u, const cf_offer_t *o);
    int  (*peek_result)(void *u, char *title17, char *note24);
    int  (*power_ok)(void *u);
    void (*open)(void *u);                           /* put SYSTEM > DOWNLOAD on the screen (a result, an unfinished install) */
    int  (*arm)(void *u, const cf_offer_t *o);       /* 0 = the stream job is armed (new, or a resume of the same image);
                                                        CF_ARM_WAIT = the image is still downloading (asked again next tick);
                                                        CF_ARM_NO_DATA = the download stopped (no data for long): paused */
    int  (*job_result)(void *u);                     /* -1 running, else the CF_D_* code (0xEE: no writer came) */
    int  (*pending)(void *u, char *id33, char *title17);   /* 1 = an unfinished Wi-Fi install is in the journal */
    void (*cancel)(void *u);                         /* forget it */
    void (*log)(void *u, const char *m);
    int  (*call_busy)(void *u);                      /* 1 = a device API call may still write into the answer buffer */
    /* 1.0.5: A on BATTERY LOW (INSTALL ANYWAY): on = 1 makes the host's power check pass for this install's
       write (RAM only); on = 0 when the install has ended. May be NULL. */
    void (*power_anyway)(void *u, int on);
    /* 1.0.6: B while a backup runs: the host stops its read / upload (arm() then answers CF_ARM_BK_STOPPED). May be NULL. */
    void (*backup_stop)(void *u);
    /* lane sdcard1: recv/start's body into b (n bytes). May be NULL (then CF_RX_START_BODY). */
    void (*start_body)(void *u, char *b, int n);
} cf_inst_host_t;
#define CF_ARM_WAIT    0x100   /* wifispeed1: arm() while the image downloads into the staging area */
#define CF_ARM_NO_DATA 0x101   /* ... the download got nothing for long: the install pauses (resumes like a stopped write) */
#define CF_ARM_WRONG_CART 0x102 /* 1.0.5, a patch: the cart holds none of the ROMs the bundle has a patch for (nothing written) */
#define CF_ARM_ALREADY    0x103 /* ... the cart already holds the patched ROM (nothing written) */
#define CF_ARM_PATCH_BAD  0x104 /* ... the patch did not apply or its result did not check out (nothing written) */
#define CF_ARM_BACKED_UP  0x105 /* 1.0.6, a backup: the cart was read and all of it reached the website */
#define CF_ARM_BK_STOPPED 0x106 /* ... the website stopped taking it (the tab closed, Cancel) or B: nothing more is sent */
#define CF_ARM_BK_READ    0x107 /* ... the cart read failed */
#define CF_ARM_BK_NET     0x108 /* ... the upload got nowhere for long */
#define CF_ARM_NO_CARD    0x109 /* lane sdcard1: a card send, and no card that takes it (none, locked) */
#define CF_ARM_CARD_FULL  0x10A /* ... the card has no room for it */
#define CF_ARM_CARD_FAIL  0x10B /* ... the card did not take it (a write failed, the card was pulled) */
#define CF_ARM_LIB_FULL   0x10C /* lane ui110dev: a "lib": "keep" send the library has no room for (nothing kept) */
#define CF_ARM_TOO_BIG    0x10D /* m110t: longer than the place it is for takes (nothing kept): said as that, never as memory */
#define CF_ARM_CANT_PLAY  0x10E /* m110t: not a game this Chromatic plays (its header: the mapper, more than 4 MiB) */
#define CF_ARM_LIB_FAIL   0x10F /* m110t: the library did not take it (a write or its read-back failed; nothing kept) */
#define CF_INST_BUF 1024   /* the longest poll answer: about 615 (AES fields, a 2 MiB send, betadl blob host); lane mygames1: a card send of a packed game with its save is about 880 (serde sorts the keys, so a cut loses "z" first). Heap only while receive mode runs */

typedef struct {
    cf_inst_host_t h;
    int      st;
    uint32_t now, t0;
    char    *buf;                      /* API answer: heap (CF_INST_BUF) only while the machine is busy, so 1.0.3
                                          costs the idle unit as little static RAM as possible (the TLS gate is 21,000) */
    volatile int btn;                  /* the page's last button, taken by the tick: -1 none, 0 B, 1 A */
    cf_offer_t offer;
    char     note[24];
    char     done_id[33];              /* reported (or being reported): never installed again this boot */
    /* receive mode */
    int      rx;                       /* the hold is on */
    int      rx_resume;                /* this receive mode is for the unfinished install (recv/resume), not a code */
    int      rx_reg_tries, rx_fails, rx_peeks, rx_cancel_sent, rx_inflight, rx_sent;
    char     code[CF_RX_CODE_LEN + 1];
    uint32_t code_until, poll_ms, next_poll, power_until, resume_at, ack_from;
    const char *fail_rows;             /* CV_FAIL: "ROW1|ROW2" (a literal) */
    /* the end of the last install, on SYSTEM > DOWNLOAD until A / B */
    int      res_shown;
    int      rx_after;
    int      rx_conn_tries;            /* cartspeed2: device calls with no connection, asked again (rx_again) */
    uint32_t rx_busy_wait;                  /* 1.0.3: the last 429 backoff in ms (0: none; 4 s doubling to 60 s without a server wait) */
    uint32_t rx_busy_since;                 /* 1.0.3: when the 429s began (a start that stays refused 10 min: TOO MANY TRIES) */
    uint32_t rx_jit;                        /* 1.0.3: jitter state */
    uint8_t  rx_busy;                       /* 1.0.3: the server said 429 to the last call: BUSY, RETRYING on the page */
    uint32_t rx_wait_until;                 /* cartspeed2: A came while the last install's report was going out: receive once it is done */
    uint32_t res_until, res_reopen_at;   /* the job's end restarts the game and closes the menu: shown again after it */
    const char *res_title, *res_note;  /* literals */
    /* an unfinished install: shown on the OSD; resumed by A, or by itself in receive mode */
    int      unfinished, unf_opened, auto_tries, resuming;
    int      arming;                   /* CI_ACKDL: arm() said CF_ARM_WAIT (the image is downloading) */
    int      anyway;                   /* 1.0.5: INSTALL ANYWAY was pressed on BATTERY LOW for this install */
    int      chain_wait;               /* 1.0.8: a backup ended well; the same code waits for the install that follows */
    char     unf_id[33], unf_title[17];
    /* a report that did not get through yet: sent again on a timer (this is not a poll for games) */
    int      rep_left;
    uint32_t rep_at;
    uint8_t  rep_quick;      /* gamecomp1: quick tries of a report that could not start (the heap after a big install), receive mode kept */
    char     rep_id[33], rep_state[12], rep_detail[48];
    unsigned n_polls, n_codes, n_offers, n_installs, n_declined, n_failed, n_resumes, n_expired;
    /* lane ui110dev: GET GAMES (SYSTEM > GET GAMES): receive mode with no code on the screen, and the page's own calls
       made over it between two polls (the catalog's pages, the order that binds a game to this unit's code) */
    int      cat;                      /* this receive mode is GET GAMES's (the code is asked for again when it ends) */
    int      cat_renew;
    int      cat_pending;              /* GET GAMES asked while the last receive mode was still ending: started when it has */
    volatile int side_st;              /* 0 none, 1 asked, 2 running, 3 answered (side_code, side_out) */
    int      side_code;
    char     side_path[CF_SIDE_PATH];
    char     side_body[CF_SIDE_BODY];
    char    *side_out;                 /* the page's buffer */
    int      side_cap;
} cf_inst_t;

/* The power rule for a cart write: on the charger, or a valid reading at least this kind's
   "mid" mark plus 0.05 V of margin (AA 3.20 V, LiPo 3.45 V). A 2 MiB write takes minutes. */
int  cf_power_policy(int valid, float level_v, float low_v, int charging);
void cf_install_init(cf_inst_t *c, const cf_inst_host_t *h, uint32_t now_ms);
void cf_install_tick(cf_inst_t *c, uint32_t now_ms);
void cf_install_receive(cf_inst_t *c);                 /* RECEIVE GAME (A on the menu, console `cart receive`) */
/* lane ui110dev: GET GAMES. catalog: receive mode for the page (no code shown; a code that ends is asked for again).
   side: one call of the page's (path, a JSON body; "%s" in it is this receive mode's code) made between two polls, its
   answer into out (cap bytes); 0 = asked, -1 not now (no code yet, or one running). side_done: -1 none, 0 running, else the
   HTTP code (-1 no connection: then *code) and out filled; it is then forgotten. */
void cf_install_catalog(cf_inst_t *c);
int  cf_install_side(cf_inst_t *c, const char *path, const char *body, char *out, int cap);
int  cf_install_side_done(cf_inst_t *c, int *code);
int  cf_install_has_code(const cf_inst_t *c);
void cf_install_button(cf_inst_t *c, int a);           /* the page's A (1) / B (0); taken by the next tick */
void cf_install_view(const cf_inst_t *c, cf_view_t *v);
int  cf_install_busy(const cf_inst_t *c);              /* 1 = receive mode, an install, or a report under way */
int  cf_install_report_only_waits(const cf_inst_t *c);   /* gamecomp1: only a report on its minute timer is left */
int  cf_rx_code_ok(const char *code);                  /* 6 characters of CF_RX_ALPHABET */
const char *cf_install_state(const cf_inst_t *c);
/* parse a poll answer (exposed for the tests). 1 = an offer, 0 = nothing pending, -1 = refused (reason in why) */
int  cf_install_parse(const char *json, const char *gateway_base, cf_offer_t *o, const char **why);

/* 1.0.7: an HTTP/1.1 answer read as it arrives, for the backup's own upload connection (cf_dev.c bk_request).
   Headers are read LINE BY LINE and only each line's start is kept, so their length does not matter
   (1.0.6 needed all of them in 512 bytes; Cloudflare sends ~970: every piece looked lost on prod). Takes
   Content-Length or chunked bodies; keeps the body's first CF_HTTP_BODY - 1 bytes (the answer is one short line). */
#define CF_HTTP_LINE 128
#define CF_HTTP_BODY 48
#define CF_HTTP_BODY_MAX 4096u        /* a longer body is not an answer of ours */
typedef struct {
    int      st;                       /* 0 status line, 1 headers, 2 body, 3 chunk size, 4 chunk data, 5 chunk end, 6 trailer, 9 done, -1 bad */
    int      code, closing, chunked, have_len;
    uint32_t len, left, total;
    char     line[CF_HTTP_LINE];
    uint16_t ln;
    char     body[CF_HTTP_BODY];
    uint16_t bn;
} cf_http_ans_t;
void cf_http_ans_init(cf_http_ans_t *a);
/* 1 = the whole answer is in (code, closing, body), 0 = more, -1 = not an answer we take */
int  cf_http_ans_feed(cf_http_ans_t *a, const char *b, uint32_t n);
#endif
