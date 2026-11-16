/* cf_sgz.h: SGZ1, a compressed Game Boy game (lane gamecomp1). The unit only UNPACKS; the backend
 * (shiny_game_data sgz.rs) and the cart page (release web lib/sgz.js) pack. The format, exactly
 * (host/sgz.py is the reference; host/sgz_golden the vectors all three sides test):
 *
 *   0  "SGZ1"   4  version 1   5  block_shift 12|13|14   6  nblocks u16   8  rom_size u32 (0x150 .. 4 MiB)
 *  12  rom_crc32   16  rom_sha256[32]   48  the ROM's bytes 0x134..0x14F [28]   76  data_size u32
 *  80  reserved[8] = 0   88  flags u32 = 0   92  header_crc32 (bytes 0..91, then the whole table)
 *  96  table: per block u32 (bits 0..23 stored length L, bit 31 RAW, 24..30 zero) + u32 crc32 of its ROM bytes
 *      then the blocks back to back. A block is one raw DEFLATE stream (RFC 1951) of L bytes inflating to
 *      exactly its length (1 << block_shift; the last: what is left), or RAW (L = that length, as is).
 *
 * Memory while unpacking (heap, freed at the end): cf_inflate's state with its 1 KiB of input (~2.2 KB), the SHA-256
 * context, 256 bytes of table, and the block's bytes in pieces of at most 8 KiB (separate allocations). No 32 KiB
 * dictionary: blocks stand alone. (The ESP32 ROM's tinfl was the first choice: its 10,992-byte state + a block did
 * not fit the heap at a play's start, 17,220 free / largest 12,288 on unit 2, 2026-10-22.)
 * Nothing is written out before the header and the whole table have checked (header crc32, lengths, sizes);
 * each block is written once its crc32 is right; the end checks the whole ROM's crc32 and sha256. A caller
 * that writes into a place a failure must not leave half-filled treats any error as "nothing written". */
#ifndef CF_SGZ_H
#define CF_SGZ_H
#include <stdint.h>

#define CF_SGZ_HDR        96u
#define CF_SGZ_ROM_MIN    0x150u
#define CF_SGZ_ROM_MAX    0x400000u
#ifndef CF_SGZ_SHIFT_MAX
#define CF_SGZ_SHIFT_MAX  14u          /* 16 KiB blocks decode too (two 8 KiB pieces); the encoders use 8 KiB (the heap at a play's start) */
#endif

/* errors (negative) */
#define CF_SGZ_E_READ    (-1)          /* the source did not give the bytes */
#define CF_SGZ_E_WRITE   (-2)          /* the sink did not take them */
#define CF_SGZ_E_MEM     (-3)
#define CF_SGZ_E_MAGIC   (-4)
#define CF_SGZ_E_HEADER  (-5)          /* version, shift, sizes, flags */
#define CF_SGZ_E_HCRC    (-6)
#define CF_SGZ_E_TABLE   (-7)          /* a block length, data_size, the container's length */
#define CF_SGZ_E_INFLATE (-8)
#define CF_SGZ_E_BCRC    (-9)
#define CF_SGZ_E_ROM     (-10)         /* the whole ROM's crc32 or sha256 */
#define CF_SGZ_E_BIG     (-11)         /* a block size this unit does not take */

typedef struct {
    uint8_t  shift;
    uint16_t nblocks;
    uint32_t rom_size, rom_crc32, data_size, data_start, total;
    uint8_t  sha256[32];
    uint8_t  cart[28];                 /* ROM 0x134..0x14F: title, type at [0x147 - 0x134] */
} cf_sgz_hdr_t;

/* source: container bytes [off, off + n) into b; sink: ROM bytes [off, off + n) (one block at a time, in order).
   0 = ok. */
typedef int (*cf_sgz_read_fn)(void *u, uint32_t off, uint8_t *b, uint32_t n);
typedef int (*cf_sgz_write_fn)(void *u, uint32_t off, const uint8_t *b, uint32_t n);

int  cf_sgz_is(const uint8_t *b, uint32_t n);                          /* 1 = starts with "SGZ1" */
/* the fixed header only (no table): 0 or an error. total = the container's length as stored / sent. */
int  cf_sgz_parse(const uint8_t *h, uint32_t total, cf_sgz_hdr_t *o);
/* the whole check + unpack. hdr_out may be NULL. pct (may be NULL): 0..100 as blocks are done. */
int  cf_sgz_unpack(cf_sgz_read_fn rd, cf_sgz_write_fn wr, void *u, uint32_t total, cf_sgz_hdr_t *hdr_out,
                   void (*pct)(void *u, int p));
/* the same with a fast reader for the blocks' bytes that may be wrong now and then (a block that does not inflate or check
   is read again with rd; the header and the table always with rd) */
int  cf_sgz_unpack2(cf_sgz_read_fn rd_fast, cf_sgz_read_fn rd, cf_sgz_write_fn wr, void *u, uint32_t total, cf_sgz_hdr_t *hdr_out,
                    void (*pct)(void *u, int p));
/* the checks alone (no output; every block inflated and its crc32 checked, the ROM's hashes too) */
int  cf_sgz_verify(cf_sgz_read_fn rd, void *u, uint32_t total, cf_sgz_hdr_t *hdr_out);
const char *cf_sgz_why(int e);
/* the last unpack's time by part, microseconds (reads of compressed blocks, inflating, crc32 + sha256, the sink) */
typedef struct { int64_t read_us, inflate_us, check_us, write_us, sha_us; uint32_t retries, mem_at, fail_block, fail_src, fail_len; uint8_t seg_shift; } cf_sgz_prof_t;   /* mem_at: the allocation that failed (size; bit 31: piece i << 16); seg_shift: the pieces used (11 = 2 KiB, 10 = 1 KiB) */
extern cf_sgz_prof_t cf_sgz_prof;
/* heap an unpack needs for this block shift (for a caller's gate) */
uint32_t cf_sgz_heap_need(uint8_t shift);
#endif
