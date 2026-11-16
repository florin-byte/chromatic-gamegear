/* cf_esp.h: the ESP32 side of "Send to my Chromatic" (cart flash), lane cartflash1. DRAFT.
 *
 * Portable C (no ESP-IDF), like nw_esp.c: the same file runs on the ESP32 (behind
 * NW_CARTFLASH, never on by default) and in the Mac simulation (host/cf_sim.c).
 *
 * The SM83 writer (sm83/cf_writer.asm) is a dumb executor. This file owns the job:
 * chip identification against a table, a full backup of the original cart before
 * anything is erased, the sector plan (header sector last), erase / blank check /
 * program / verify with retries, the save RAM, and a journal that survives power cuts so
 * a job always resumes (or restores) instead of leaving a dead cart.
 */
#ifndef CF_ESP_H
#define CF_ESP_H
#define CF_SRC_WIFI_STAGED 3   /* wifispeed1: strm_src of a Wi-Fi install written from the staging area */
#include <stdint.h>
/* 1.1.0: where the staged ROM is (cf_usb.h CF_STAGE_ROM_A / _B), set once at boot; the console's
   `CBU1 info` says it as stg= so the cart page writes there */
extern uint32_t cf_stage_rom_off;
/* 1.1.0: 1 while a new version is not confirmed: nothing may be staged (the idle slot is its way back); `info` says stg=000000 */
extern int cf_stage_hold;
#include "cf_proto.h"
#include "nw_esp.h"

#define CF_MAX_REGIONS 4
#define CF_MAX_SECTORS 1024
#define CF_RAM_SIZE    32768u   /* ModRetro carts: 32 KiB FRAM (4 x 8 KiB banks) */

typedef struct { uint32_t size; uint16_t count; } cf_region_t;

typedef struct {
    const char *part;
    const char *vendor;
    uint8_t     id[4];        /* autoselect bytes at flash 0..3 (byte mode) */
    uint8_t     id_len;       /* how many of id[] identify it (0: entry disabled) */
    uint32_t    size;
    uint8_t     cmdset;       /* CF_CS_* */
    uint8_t     erase_op;     /* sector (or block) erase opcode */
    uint8_t     cfi_mode;     /* 0 none, 1 single 98 at cfi_addr, 2 unlock + 98 */
    uint16_t    cfi_addr;
    uint8_t     geometry_from_cfi;
    cf_region_t regions[CF_MAX_REGIONS];   /* erase units, address order */
    uint16_t    erase_tmo;    /* x 16 ms */
    uint32_t    protect_top, protect_bottom;  /* bytes a WP#-protected boot block may hold: never planned */
    uint8_t     bottom_test;  /* 1.0.2: writes allowed although not verified, because every write job first tests
                                 that the protect_bottom block takes a write (one bit, cf_esp.c "WP TEST") */
    uint8_t     verified;     /* 1: confirmed on a real cart by our own probe; 0: writes refused
                                 unless the job allows unverified chips (simulation, owner test) */
    const char *source;
} cf_chip_t;

extern const cf_chip_t cf_chips[];
extern const int       cf_n_chips;

/* jobs */
#define CF_JOB_NONE    0u
#define CF_JOB_BACKUP  1u   /* back up the cart (ROM + save RAM), write nothing */
#define CF_JOB_INSTALL 2u   /* backup if none, then write the staged image */
#define CF_JOB_RESTORE 3u   /* write the backup back (ROM + save RAM) */
#define CF_JOB_SELFTEST 4u  /* hardware test: in a block that is blank (in the backup and on the cart),
                               program a 4 KiB pattern, verify, erase the block back, verify blank */
#define CF_JOB_SAVE_GET 8u  /* the cart's save RAM -> the save store (a second 32 KiB, apart from the cart backup) */
#define CF_JOB_SAVE_PUT 9u  /* the save store -> the cart's save RAM, read back and compared */
#define CF_JOB_SAVE_CLEAR 10u /* the cart's save RAM to 00 */
#define CF_JOB_RAMTEST 11u  /* hardware test: 1 KiB of RAM bank 1: read, pattern, read back, original back, read back */
#define CF_JOB_STREAM  12u  /* write an image STRAIGHT to the cart from a chunk source (the gateway over Wi-Fi, or the
                               owner's computer over USB): nothing is staged or backed up on the ESP32 (owner 2026-10-11) */
#define CF_JOB_USBREAD 13u  /* read-only: the cart's ROM or save RAM, as binary frames to the USB console */
#define CF_JOB_USBRAM  14u  /* the save RAM from the chunk source (USB restore of a .sav) */
#define CF_JOB_GBTEST  7u   /* read-only: do GB-timed cycles work when no write is aimed under the boot ROM? */
#define CF_JOB_RAMDUMP 6u   /* read-only: the cart RAM through the engine, printed as base64 lines (2026-10-11) */
#define CF_JOB_ROUNDTRIP 5u /* hardware test: rewrite the cart's own image from the backup (every
                               non-blank erase unit erased + programmed + verified), ROM only */

/* phases (journaled) */
enum { CF_PH_IDLE, CF_PH_HELLO, CF_PH_ID, CF_PH_BAK_ROM, CF_PH_BAK_RAM, CF_PH_PLAN,
       CF_PH_WRITE, CF_PH_VERIFY, CF_PH_RAM, CF_PH_DONE, CF_PH_FAILED, CF_PH_SELFTEST, CF_PH_RAMDUMP, CF_PH_GBTEST, CF_PH_SAVE, CF_PH_STREAM, CF_PH_USBREAD, CF_PH_USBRAM,
       CF_PH_WPTEST /* 1.0.2: session only (never journaled): the bottom block's write test */ };

#define CF_JOURNAL_MAGIC 0x43464A32u   /* "CFJ2" (2026-10-11: + the streamed install) */
typedef struct {
    uint32_t magic;
    uint8_t  job, phase, chip, mbc;
    uint8_t  result;                 /* CF_D_* of the last finished job */
    uint8_t  verify_pass;
    uint16_t nsec;
    uint32_t chip_size;
    /* the original cart (the backup) */
    uint8_t  bak_ok;
    uint8_t  orig_hdr[80];
    uint32_t bak_rom_size, bak_rom_crc32, bak_ram_crc32;
    uint32_t ram_present;            /* bit k: save RAM KiB k read the same twice (backed up);
                                        a floating KiB is never stored, restored or cleared */
    /* what is being written / was written last */
    uint32_t tgt_size, tgt_crc32;
    uint8_t  tgt_hdr[80];
    uint8_t  done[CF_MAX_SECTORS / 8];
    uint32_t n_power_resumes;
    /* the streamed install in flight (CF_JOB_STREAM): what a resume must match */
    char     strm_id[33];
    char     strm_title[17];
    uint8_t  strm_src;               /* 0 the gateway, 1 USB (console), 2 the image staged in the ESP32's flash,
                                        3 (CF_SRC_WIFI_STAGED) a Wi-Fi install downloaded into the same staging area */
    uint32_t strm_size, strm_crc32;
    uint8_t  strm_sha[32];
    uint8_t  strm_tries;             /* whole-image verify failures so far */
    uint32_t crc;                    /* crc32 of everything above */
} cf_journal_t;

typedef struct {
    uint8_t  phase, job, result;
    uint32_t done_bytes, total_bytes;
    uint16_t sector, nsec;
    char     text[48];               /* OSD line, e.g. "BACKUP 12/128 BANKS" */
} cf_status_t;

typedef struct {
    void *user;
    /* the staged image to install (already downloaded, sha-checked, signature-checked) */
    uint32_t img_size;
    int  (*img_read)(void *u, uint32_t off, uint8_t *b, uint32_t n);
    /* the backup store (ESP32: the "cart" data partition; NEVER uploaded anywhere) */
    int  (*bak_write)(void *u, uint32_t off, const uint8_t *b, uint32_t n);   /* ROM part */
    int  (*bak_read)(void *u, uint32_t off, uint8_t *b, uint32_t n);
    int  (*ram_write)(void *u, uint32_t off, const uint8_t *b, uint32_t n);   /* save RAM part */
    int  (*ram_read)(void *u, uint32_t off, uint8_t *b, uint32_t n);
    /* the save store: a game's save, taken and put back on its own (NEVER uploaded either) */
    int  (*sav_write)(void *u, uint32_t off, const uint8_t *b, uint32_t n);
    int  (*sav_read)(void *u, uint32_t off, uint8_t *b, uint32_t n);
    /* CF_JOB_STREAM / CF_JOB_USBRAM: bytes [off, off+n) of the image from its source. 1 = copied
       into dst; 0 = not here yet (asked for, ask again); -1 = the source says it is gone */
    int  (*chunk)(void *u, uint32_t off, uint16_t n, uint8_t *dst);
    uint32_t bak_capacity;           /* bytes the ROM part may hold */
    /* the journal (ESP32: NVS blob) */
    int  (*jr_load)(void *u, cf_journal_t *j);
    int  (*jr_save)(void *u, const cf_journal_t *j);
    /* the FPGA: boot image select + cart power cycle (link cmd 5) */
    void (*boot_select)(void *u, int netboot);
    void (*cart_cycle)(void *u);
    int  (*power_ok)(void *u);       /* battery high enough or USB power */
    void (*progress)(void *u, const cf_status_t *s);
    void (*log)(void *u, const char *msg);
    /* `cart probe` over USB: one line to the owner's computer (ESP32: the console) */
    void (*host_line)(void *u, const char *line);
    /* bulk probe data (cp/r chunks, cp/x fingerprints) as one binary frame, written in one
     * piece (ESP32: uart_write_bytes on the console UART, no CRLF translation):
     *   A5 'C' 'P' 'B', path letter, len u16 LE, body[len], crc32 LE (over letter, len, body)
     * The chunk is already answered to the GB (its 16-bit sum checked here), so the probe
     * never waits for the computer: the console streams while the next chunk is read.
     * NULL: those calls go as CPX lines like the rest. */
    void (*host_frame)(void *u, const uint8_t *frame, uint32_t n);
    /* the FPGA cart bus engine may take the cart bus (link cmd 8); on for a job, off after */
    void (*engine_allow)(void *u, int on);
    /* The FPGA's run-time patch overlay (18.22+) answers the CPU's cart ROM reads with the
     * patch's bytes, and the writer reads the ROM with CPU reads (only flash writes and cart RAM
     * go through the engine): unit 1, 2026-10-11, a USB backup under an ACTIVE Tetris patch was
     * the patched game. So no cart job runs under it:
     *   overlay_hold(1) before the job's cart power cycle: patches are held OFF (the overlay
     *     ends at that core reset and cannot start again), (0) before the job's last cycle;
     *   overlay_state(ask): 0 = the FPGA says the overlay is off (or this FPGA has none),
     *     1 = on, -1 = no report yet since the last ask (ask = 1: ask the FPGA for a fresh one).
     * NULL (an FPGA / firmware without the overlay): no check. */
    void (*overlay_hold)(void *u, int on);
    int  (*overlay_state)(void *u, int ask);
    /* cartspeed1: the FPGA's express cart stream (18.26+, cart_xs.v). The cart data goes over the
     * QSPI bus (the OSD's) into a 4 KiB ring in the FPGA, the ops and posts over the UART (address
     * 0x12), the FPGA programs the flash itself; the writer only waits meanwhile. Its status packets
     * (address 0x0C, 32 bytes) are handed to cf_esp_xs_status on the same task as cf_esp_op.
     *   xs_avail: 1 = the loaded FPGA has it; xs_send: one UART packet to address 0x12;
     *   xs_ring: bytes into the ring at offset off (one QSPI write, returns when it is on the bus),
     *   0 = done. NULL: the classic route (the writer programs every byte). */
    int  (*xs_avail)(void *u);
    void (*xs_send)(void *u, const uint8_t *p, uint8_t n);
    int  (*xs_ring)(void *u, uint16_t off, const uint8_t *b, uint16_t n);
    /* cartspeed2: a staged image that is still arriving / being checked while the cart is written
     * (the Wi-Fi download, the USB copy's hash). NULL = the whole image is there and checked.
     *   img_avail: bytes [0, n) of the image are in the staging area now (0xFFFFFFFF: all of it);
     *   img_gate:  0 = the whole image is there and its crc32 + sha256 are the offer's (block 0 and
     *              the save RAM may be written), 1 = not yet (wait), -1 = it is not the image (the
     *              blocks written from it are void: the job stops, a resume writes all of them again). */
    uint32_t (*img_avail)(void *u);
    int  (*img_gate)(void *u);
    /* cartspeed2: the loaded FPGA image's version, major << 8 | minor (0 unknown): `info` names it
       (fpga=18.27: its USB bridge follows esptool's CHANGE_BAUDRATE, the page's fast copy) */
    unsigned (*fpga_ver)(void *u);
    /* cartspeed2: the crc32 of the image's bytes in [off, off + n) (one erase block; FF past the image
       NOT included) as the source received them, 1 = known; the cart write compares it with the
       staged bytes it read (a staging area that does not hold what came: the job stops, void) */
    int  (*img_blk_crc)(void *u, uint32_t off, uint32_t n, uint32_t *crc);
} cf_host_t;

typedef struct {
    uint8_t job;
    uint8_t allow_unverified;        /* simulation / owner-approved first hardware runs only */
    uint8_t ram_clear;               /* INSTALL / STREAM: 1 = zero the save RAM for the new game; 2 = write the staged
                                        save (host hook sav_read) */
    uint8_t fast_verify;             /* STREAM from the staged image only: the cart is checked bank by bank with a
                                        CRC16 worked out on the Game Boy side, against the CRC16 of the staged data
                                        (already checked by crc32 + sha256), instead of reading every byte back */
    uint8_t max_retries;             /* per sector, default 2 */
    /* CF_JOB_STREAM / CF_JOB_USBRAM */
    char     strm_id[33];            /* the source's id of this image (a resume must name the same) */
    char     strm_title[17];
    uint8_t  strm_src;               /* 0 the gateway, 1 USB */
    uint16_t strm_chunk;             /* bytes per chunk asked from the source (2048 gateway, 512 USB) */
    uint32_t strm_size, strm_crc32;
    uint8_t  strm_sha[32];
    uint8_t  strm_have_sha;
    /* CF_JOB_USBREAD: 0 ROM, 1 save RAM, 2 info only */
    uint8_t  rd_skip;                /* USBREAD of the ROM: a 16 KiB bank that reads all FF (checked twice) is not sent, only named */
    uint8_t  rd_what;
    uint32_t rd_off, rd_len;
    uint32_t rd_dirty_from;          /* 1.0.5 (a patch reads its own cart): from this offset on, the first bank that is
                                        not blank ends the read ("CBU1 dirty OFF"): the cart is not the one; 0 = off */
    uint8_t  xs_off;                 /* cartspeed1: 1 = never the express stream (the classic route) */
    uint8_t  xs_timing;              /* cartspeed1: engine bus timing for the express stream: 0 safe (the timing of
                                        every earlier image), 1 short, 2 shortest (what the device asks for: measured on
                                        a unit); a block that does not verify drops back to 0 for the rest of the job */
    uint8_t gb_timed;                /* the writer's GB-timed route (F_GBT): no engine needed; UNTESTED
                                        on a ModRetro cart for program / erase (report 0m). Default 0:
                                        the engine route. */
} cf_job_t;

typedef struct {
    uint8_t  cmd, mbc, flags, cmdset, op, cfi_mode;
    uint16_t bank, addr, len, tmo, cfi_addr;
    uint8_t  src;                    /* PROGRAM / RAMWRITE data: 1 target, 2 RAM backup, 3 zeros */
    uint32_t src_off;
} cf_cmd_t;

/* cartspeed1: the express stream's state (cf_esp.c "EXPRESS") */
#define XS_SEG 48
struct cf_xs {
    uint8_t  on, st, timing, seq, e0_done, b0_step;
    struct { uint8_t seq, op, kind; int16_t blk; uint32_t f, n, want, pos0; } q[4];
    int      qh, qn;
    struct { uint8_t kind; int16_t blk; uint32_t f, n, want; } fix[8];   /* ops owed after a result */
    int      fixn;
    uint32_t pos_next, feed;          /* stream position after the last PROG sent; next to write into the ring */
    int      pblk, pstep;             /* the planner: block, and what of it was sent */
    uint32_t want, nz0, nz1;          /* the block in hand: crc32 of its bytes, its non-FF range */
    uint32_t seg[XS_SEG][2];          /* ... its non-FF segments (split at FF runs >= XS_FFRUN) */
    int      nseg;
    int      tblk;
    uint8_t  sync_seq;                /* the CFG of a (re)start: nothing else until it is done */
    uint8_t  have, flags, done_seq, err, naks_seen, all_ff, run_seq;
    uint32_t cons, valid, crc, faddr;
    uint32_t t_status, t_ask, t_start, n_status, n_posts, n_naks, n_resync, verified, n_missed, n_lost, t_sync, n_resync_lost, t_post;
    uint16_t retries;
    uint8_t  ram_tries;
    uint32_t m_pbusy, m_ebusy, m_nprog, m_ebusy_max;   /* the FPGA's measurement (hclk / bytes; 18.26 status bytes 21-31) */
    uint32_t t_wait, ms_wait, ms_gate;  /* cartspeed2: waiting for the image to arrive (since; total), for its check */
    uint8_t  rechk[CF_MAX_SECTORS / 8]; /* cartspeed2: a resume reads each block done before it back once (crc32 against the staged bytes) */
    uint16_t n_rechk, n_rebad;
    uint32_t wimg;                      /* cartspeed2: crc32 of the image bytes (no FF fill) of the block just scanned */
};

typedef struct cf_esp {
    cf_host_t    h;
    cf_journal_t j;
    cf_job_t     opt;
    int          armed;
    uint32_t     now, last_traffic;
    int          done_pending;
    uint32_t     done_at;
    /* session */
    int          session;
    uint8_t      seq, prev_seq;
    cf_cmd_t     last;
    uint8_t      hdr[80];
    int          ph;                 /* session phase (CF_PH_*); j.phase is the persisted
                                        cart state: IDLE (consistent), WRITE (in flux), FAILED */
    int          resume;             /* this session continues an interrupted write */
    int          engine;             /* the writer reported a cart bus engine */
    uint32_t     rd_bank_at;         /* pos + 1 of the bank rd_bstate is about */
    uint8_t      rd_bstate;          /* USBREAD skip: 0 = check this bank, 1 = it was blank once, 2 = not blank: read it */
    uint8_t      ovl_ok;             /* 0 not asked, 1 asked at the first hello, 2 confirmed off, 3 asked again (the USB-write belt) */
    uint32_t     ovl_t0;
    uint32_t     st_blk, st_size;    /* SELFTEST: the erase unit under test */
    int          have_cfi;
    /* plan */
    const cf_chip_t *chip;
    cf_region_t  reg[CF_MAX_REGIONS]; /* erase geometry in use (table or CFI) */
    int          nreg;
    int          nsec;               /* sectors of the whole chip */
    int          plan_n;             /* sectors that cover the target: order 1..n-1, then 0 */
    int          id_try;
    int          oi;                 /* index into order[] */
    int          step, retry;
    uint32_t     pos;                /* byte cursor inside the current unit */
    int          piece_mismatch;
    uint32_t     unit;               /* backup: start of the bank being read */
    uint16_t     bank_crc;           /* backup: crc16 of the bank just read */
    int          bank_retry;
    uint32_t     ramdump_crc;
    uint8_t      gbtest[8];         /* valid, ram gb==engine (engine enable), ram gb==engine (GB enable at 1000), id0, id1, after0, after1, id ok */
    uint8_t     *tbuf;              /* RAMTEST: the original KiB, on the heap for the job */
    uint32_t     save_crc;          /* crc32 of the last SAVE_GET / SAVE_PUT */
    uint8_t      ramtest_ok;
    uint8_t      ram_kept;          /* INSTALL: the replaced game's save is in the save store (or not wanted) */
    /* CF_JOB_STREAM */
    uint8_t     *sbuf;              /* one chunk from the source (heap, for the job) */
    int          sp;                /* sub-phase (SP_*) */
    int          s_blk, s_nblk;     /* block in hand; blocks that hold image bytes we can write */
    uint32_t     s_wr_end;          /* min(size, writable) */
    uint32_t     s_crc, s_crc0, s_crc0_cart;
    uint32_t     s_wait_ms;         /* how long the source has had nothing */
    uint32_t     s_wait_t0;
    int          s_have;            /* sbuf holds the chunk at c->pos */
    uint8_t      want_cycle_after;  /* a silent writer was dropped: power-cycle the cart once the FPGA is back to normal */
    uint16_t     s_want16;          /* fast verify: the CRC16 the bank being checked must have */
    uint8_t      s_probed;          /* the data channel was proven (the image's first bytes arrived) before the first erase */
    uint32_t     s_entry;           /* SP_BLK0: pos + 1 of the piece that holds the entry point (0x100), written after all others */
    struct { uint32_t h[8]; uint64_t bytes; uint8_t buf[64]; uint32_t fill; } s_sha;
    struct cf_xs *xs;               /* cartspeed1: the express stream (cf_esp.c "EXPRESS"): heap, only for a job that uses it */
    uint32_t     rd_crc;
    /* 1.0.2 WP TEST (chip->protect_bottom): before the first erase, one bit of one byte of the
       protected-if-WP#-low block is programmed and read back */
    uint8_t      wp_step;           /* 0 scan, 1 program, 2 read back */
    uint8_t      wp_done;           /* passed in this session */
    uint8_t      wp_v, wp_nv;       /* the byte before, and the value programmed (one 1 bit cleared) */
    uint8_t      wp_prog_res, wp_tries;
    uint32_t     wp_scan, wp_at;    /* the 1 KiB piece being read (top down); the byte chosen */
    uint32_t     wp_ff, wp_nz, wp_hdr;   /* fallbacks seen while scanning, + 1 (0 = none) */
    uint8_t     *pbuf;              /* probe work area (frame, line, answer body): heap, only while a probe is armed */
    /* `cart probe`: CF_LOAD serves the read-only probe; its GET/POST go to the host line */
    int          probe_armed, probe_flashid;
    struct nw_esp *probe_esp;
    int          probe_pending, probe_end_pending;
    uint8_t      probe_id;
    uint32_t     probe_lines, probe_frames, probe_bad_sums;
    /* stats */
    uint32_t     n_cmds, n_erase, n_program_bytes, n_read_bytes, n_retries, n_dup;
    uint32_t     n_sav_fail;                     /* save RAM pieces (1 KiB) the writer could not write and read back equal */
    cf_status_t  st;
} cf_esp_t;

void    cf_esp_init(cf_esp_t *c, const cf_host_t *h);
/* An unfinished streamed install (the cart has no first block): its id / title / source, or 0. */
int     cf_esp_stream_pending(const cf_esp_t *c, char id33[33], char title17[17], uint8_t *src);
/* Give it up: the journal forgets it. The cart stays as it is (it does not start). */
void    cf_esp_stream_cancel(cf_esp_t *c);
/* The user confirmed the job on the OSD (or the console): arm, select the netboot image,
 * power-cycle the cart. Returns 0, or a CF_D_* refusal. */
int     cf_esp_arm(cf_esp_t *c, const cf_job_t *job);
/* An interrupted job in the journal (after a power cut): 1 + what it was. */
int     cf_esp_pending(const cf_esp_t *c, uint8_t *job, uint8_t *phase);
/* nw_esp cf_op hook (ops 0x20-0x21) */
uint8_t cf_esp_op(void *user, uint8_t op, const uint8_t *body, uint16_t blen,
                  uint8_t *out, uint16_t cap, uint16_t *out_len, uint32_t *total);
void    cf_esp_tick(cf_esp_t *c, uint32_t now_ms);
/* `cart probe [flashid]` (the owner's computer on USB runs host/cart_probe_usb.py): arm, select
 * the netboot image, power-cycle the cart; the stub then gets the probe from CF_LOAD with the
 * ESP32 already up (no race with an FPGA SRAM load), and the probe's calls are forwarded as
 *   CPX <id> <G|P> <path> <base64 body>
 * lines; the computer answers with the console command `cart px <id> <nw status> <base64>`.
 * No Wi-Fi, no http, nothing leaves the device except over that USB cable. */
int     cf_esp_arm_probe(cf_esp_t *c, int flashid);
int     cf_esp_host_answer(cf_esp_t *c, const char *args);     /* "<id> <status> <b64>" */
int     cf_probe_owns(void *user, const uint8_t *game_id);
void    cf_probe_call(void *user, nw_esp_t *e, uint8_t id, const nw_call_t *k);
/* cartspeed1: a status packet of the express stream (UART address 0x0C), on the cf_esp_op task */
void    cf_esp_xs_status(cf_esp_t *c, const uint8_t *p, uint8_t n);
/* Forget RAM state as after an ESP32 reboot (the journal stays). */
void    cf_esp_reboot(cf_esp_t *c);

/* helpers (exported for tests) */
uint16_t cf_crc16(uint16_t crc, const uint8_t *b, uint32_t n);
uint32_t cf_crc32(uint32_t crc, const uint8_t *b, uint32_t n);
int      cf_header_ok(const uint8_t hdr80[80]);       /* logo + header checksum */
/* What the device enforces on an image (owner ruling 2026-10-11: ANY ROM may be installed, not
 * only Shiny Gen cartridges): its length, 32 KiB up to cap bytes (0 = no upper check here).
 * The header is NOT judged: no mapper, size-byte or identity rule. 0 ok, else CF_D_BAD_IMAGE. */
int      cf_image_policy(const uint8_t *hdr80, uint32_t size, uint32_t cap);
/* What the player should know before a foreign ROM goes on a ModRetro cart (an MBC5-like
 * mapper, 32 KiB of save RAM, no clock). Warnings only, never a refusal. */
#define CF_W_HEADER 0x01u   /* logo / header checksum wrong: the console will not start it */
#define CF_W_MAPPER 0x02u   /* not ROM-only or MBC5: bank switching may not work */
#define CF_W_RAM    0x04u   /* asks for more than 32 KiB of save RAM */
#define CF_W_RTC    0x08u   /* a clock cartridge: no clock here, time will not be kept */
unsigned cf_image_compat(const uint8_t *hdr80);
const char *cf_compat_text(unsigned w);                  /* the first warning as an OSD line ("" = none) */
/* The header's title, safe for the OSD: A-Z 0-9 and a few signs, upper case, at most 15,
 * "UNKNOWN GAME" when there is none. out: at least 17 bytes. */
void     cf_title_from_header(const uint8_t *hdr80, char *out);
int      cf_cfi_regions(const uint8_t cfi[128], cf_region_t *r, int max, uint32_t *size);
const cf_chip_t *cf_chip_match(const uint8_t id16[16]);

#endif
