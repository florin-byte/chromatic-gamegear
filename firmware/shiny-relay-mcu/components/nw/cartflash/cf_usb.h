/* cf_usb.h: the owner's own cart backup and restore over the USB console (lane cartflash1).
 * The whole protocol is here (docs/CART_BACKUP_USB_PROTOCOL.md, "CBU1"): the device
 * (cf_dev.c) and the host simulation (host/cf_sim.c) both call it, so the reference tool
 * (host/cart_usb.py) is tested against the code the Chromatic runs.
 *
 *   cart usb info | status | cancel
 *   cart usb read rom [offset [length]] | read sav
 *   cart usb write rom <size> <crc32> [sha256] | write sav
 *   cart usb d <offset> <base64> <crc32>          one chunk the device asked for (CBU1 want)
 *   cart usb baud <115200|230400|460800|921600>
 *
 * Nothing is stored on the ESP32 and nothing goes to any server.
 */
#ifndef CF_USB_H
#define CF_USB_H
#include <stdint.h>

#include "cf_esp.h"

/* A write chunk. Its line ("cart usb d <offset> <base64> <crc32>", ~160 characters for 96 bytes)
 * must fit the console's receive buffer WHOLE: the IDF REPL gives UART0 a 256-byte ring behind
 * the 128-byte hardware FIFO and takes about 1.3 characters per ms out of it (it echoes and
 * waits for every character), while 11.5 arrive. 0.18.0-rc1 used 512-byte chunks (700-character
 * lines): every one lost its middle after ~435 characters (unit 1, 2026-10-11). */
#define CF_USB_CHUNK     96u
#define CF_USB_RAM_CHUNK 64u           /* the save: a size that divides its 8 KiB banks */
#define CF_USB_CHUNK_MAX 1024u         /* the buffer: a binary frame's payload (the host test also asks for the old 512-byte text chunk to show it fails) */
/* Binary mode (`write rom ... bin`, `write sav bin`): the command does not return to the console
 * until the job ends; the console task itself reads UART0 in blocks, with no echo, and gives the
 * bytes to cf_usb_rx. One frame per `CBU1 want`:
 *   A5 'C' 'B' 'D', offset u32 LE, length u16 LE, data, crc32 LE (over offset, length, data)
 * 1024 data bytes per frame: ~5 KiB/s instead of under 1 with text lines. */
#define CF_USB_BIN_CHUNK 1024u
#define CF_USB_BIN_IDLE_MS 30000u      /* no good frame for this long: the console is given back (the job waits on) */
#define CF_USB_WANT_MS 2000u           /* an unanswered CBU1 want is repeated this often */

typedef struct {
    cf_esp_t *esp;
    void     (*line)(void *u, const char *s);       /* a text line to the computer (in order with the frames) */
    void      *user;
    int       (*heap_ok)(void *u);                  /* enough memory to arm a job (may be NULL) */
    uint32_t  (*heap_free)(void *u);                /* for the refusal's words (may be NULL) */
    uint32_t  heap_need;
    /* the link hold (usb-cart-6): on = the radio and its sessions give their heap back before a
       cart job (waits for them); off = they come back by themselves. Taken by every job command,
       lifted 30 s after the last `cart usb` command with no job running. NULL: none. */
    void      (*net_hold)(void *u, int on);
    int       held;
    unsigned  n_holds;
    /* switch the console's speed ~100 ms from now (NULL: only 115200). The device side goes back
       to 115200 by itself after 10 s without a `cart usb` line while no job runs. */
    void      (*set_baud)(void *u, uint32_t baud);
    uint32_t  baud, last_cmd_t;
    uint32_t  now;
    /* the chunk in flight */
    uint32_t  want_off, want_t, prog_t;
    uint16_t  want_n;
    int       want_open, have;
    /* buf, nbuf and rx are ONE heap block (3 KiB), there only while a write over the console
       runs (cf_usb.c bufs / bufs_free): as static RAM they cost every unit 3 KiB of heap for good */
    uint8_t  *mem;
    uint8_t  *buf;                                  /* [CF_USB_CHUNK_MAX] */
    /* Binary mode asks one chunk AHEAD: while the cart programs the chunk it has, the next one
       is already on the cable. next_* = that chunk once it has arrived (or is still awaited). */
    uint8_t  *nbuf;                                 /* [CF_USB_CHUNK_MAX] */
    uint32_t  next_off, limit;                      /* limit: the image's size (no chunk is asked past it) */
    uint16_t  next_n;
    int       next_open, next_have;
    uint16_t  chunk;                                /* 0 = CF_USB_CHUNK (tests only) */
    /* binary mode */
    int       bin;                                  /* the running write takes binary frames */
    uint32_t  bin_idle_ms;                          /* 0 = CF_USB_BIN_IDLE_MS (the simulation's clock runs ahead of its tool: it sets more) */
    uint32_t  bin_t;                                /* the last good frame (or the mode's start) */
    uint8_t  *rx;                                   /* [16 + CF_USB_CHUNK_MAX] the frame being received */
    uint16_t  rx_n;
    unsigned  n_frames, n_bad_frames, n_dup_frames;
    /* the job this side armed (0 none): its end is reported once */
    uint8_t   job;
    int       gb_timed;
    unsigned  n_chunks, n_bad_chunks, n_wants, n_garbage;
    /* the staging area (device: esp_flash; simulation: a buffer). NULL: no staged install. */
    int       (*stage_read)(void *u, int sav, uint32_t off, uint8_t *b, uint32_t n);   /* 0 ok */
    int       (*stage_room)(void *u);               /* 0 = the firmware image ends below the staging area */
    void      (*stage_erase)(void *u);              /* starts erasing rom + save; */
    int       (*stage_busy)(void *u);               /* ... 1 while that runs, then 0 (blank) or -1 (failed) */
    int       st_rom, st_sav, st_fast;              /* checked since this boot */
    uint32_t  st_size, st_crc;
    uint8_t   st_sha[32];
    int       st_clearing, st_ready_after;
    uint32_t  st_cmds;                              /* cartspeed2: stage / install / clear commands since boot (a clear owed from before a restart gives way) */
    int       tp_sp;                                /* per-phase times: the phase running (1 + index, 0 none) */
    uint32_t  tp_t, tp_t0;
} cf_usb_t;

/* The staged install (usb-cart-1): the computer writes the ROM (and the save) into the ESP32's
 * own flash in bootloader mode, at fixed offsets outside the firmware; then, over the console:
 *   cart usb stage rom <size> <crc32> <sha256>   checks what is in the flash (nothing is erased before)
 *   cart usb stage sav <crc32>                   (optional) the 32,768-byte save
 *   cart usb install [fast] [nosav]              writes the cart from the staged data
 *   cart usb clear                               erases the staged data (and gives up an unfinished install)
 * After an install that ended OK the staged data is erased by the firmware itself. */
#define CF_STAGE_SAV_OFF  0x208000u    /* in the ESP32's flash */
/* 1.1.0: the ROM's place follows the running app slot (partitions_ota.csv): the slot that is NOT running.
   0x210000 on the 1.0.x table and when ota_0 runs (ota_1 + the free top 64 KiB: the 1.0.x addresses),
   0x010000 when ota_1 runs (ota_0 + the free 64 KiB before otadata). Set once at boot (cf_dev.c). */
#define CF_STAGE_ROM_A    0x210000u
#define CF_STAGE_ROM_B    0x010000u
#define CF_STAGE_ROM_OFF  cf_stage_rom_off
#define CF_STAGE_ROM_MAX  0x1F0000u    /* 2,031,616: to the end of the 4 MB flash; also the cart's writable size */

void cf_usb_init(cf_usb_t *u, cf_esp_t *esp, void (*line)(void *, const char *), void *user);
/* `cart usb <argv...>` (argv[0] is the word after "usb"). 0 handled. */
int  cf_usb_cmd(cf_usb_t *u, int argc, char **argv);
/* cf_host_t.chunk for the USB source */
int  cf_usb_chunk(cf_usb_t *u, uint32_t off, uint16_t n, uint8_t *dst);
void cf_usb_tick(cf_usb_t *u, uint32_t now_ms);
/* take the link hold now (the device does it before it makes the console path) */
void cf_usb_hold(cf_usb_t *u);
/* Binary mode: bytes from the computer, as they come. */
void cf_usb_rx(cf_usb_t *u, const uint8_t *b, uint32_t n);
/* 1 while the console should stay with the binary reader (a binary write is running and frames
   still come); 0: give it back. */
int  cf_usb_bin_active(cf_usb_t *u);
#endif
