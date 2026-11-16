/* cf_pack.h: SGZ1 packing ON THE UNIT (lane ui110dev): a game from the microSD card kept ON CHROMATIC (the FPGA flash's
 * library) takes about half the room. The format is cf_sgz.h's (host/sgz.py the reference); the unit's own unpacker,
 * the backend's and the page's read what this writes.
 *
 * Each block (1 << shift bytes of the ROM, the last one shorter) is matched on its own (LZ77, a 3-byte hash with chains
 * inside the block: blocks stand alone) and coded as ONE raw DEFLATE stream with the shortest of a dynamic Huffman code,
 * the fixed code, or RAW (the block as it is). Two passes per block over the same matches: the first counts the symbols
 * (so the exact cost of each coding is known before a byte is written), the second writes. So nothing is ever written
 * twice: the sink may be flash (every byte programmed once; any order: the blocks first, each table entry as its block
 * ends, the 96-byte header last).
 *
 * Heap (freed at the end): the block (1 << shift), the hash heads (2 KiB), the chains (2 bytes a ROM byte of the block:
 * 8 KiB for 4 KiB blocks; when that does not fit, packing goes on without chains: shorter matches, nothing else), the
 * symbol counts and codes (~2.3 KB), the SHA-256 context, a 256-byte write buffer. Largest piece: the chains. */
#ifndef CF_PACK_H
#define CF_PACK_H
#include <stdint.h>

#define CF_PACK_E_READ   (-1)          /* the source did not give the bytes */
#define CF_PACK_E_WRITE  (-2)          /* the sink did not take them */
#define CF_PACK_E_MEM    (-3)
#define CF_PACK_E_ARG    (-4)          /* a size or shift the format has not */
#define CF_PACK_E_NOGAIN (-5)          /* the container would not be shorter than the ROM: keep the ROM itself */
#define CF_PACK_E_BIG    (-6)          /* the container would be longer than the sink's room (max) */

/* rd: ROM bytes [off, off + n) (n <= 1 << shift); wr: container bytes at off (each byte once). max: the most bytes
   the container may take (0: any). *total = its length. pct (may be NULL): 0..100 as the blocks are done. 0 = done. */
int cf_pack(int (*rd)(void *u, uint32_t off, uint8_t *b, uint32_t n), int (*wr)(void *u, uint32_t off, const uint8_t *b, uint32_t n), void *u,
            uint32_t rom_size, uint8_t shift, uint32_t max, uint32_t *total, void (*pct)(void *u, int p));
/* the same container written IN ORDER (a sink that only appends: the library's stream): two passes, begin(u, total) between them
   (the length known: plan the room; nonzero = stop), then wr_seq gets every byte from 0, once. Heap: as cf_pack + 96 + 8 a block. */
int cf_pack_seq(int (*rd)(void *u, uint32_t off, uint8_t *b, uint32_t n), int (*begin)(void *u, uint32_t total),
                int (*wr_seq)(void *u, const uint8_t *b, uint32_t n), void *u, uint32_t rom_size, uint8_t shift, uint32_t max,
                uint32_t *total, void (*pct)(void *u, int p));
/* the last pack: blocks by coding, chains used, the time by part (microseconds) */
typedef struct { uint32_t dyn, fix, raw, chains; int64_t read_us, match_us, write_us; } cf_pack_prof_t;
extern cf_pack_prof_t cf_pack_prof;
/* bench / tests: the matcher's depth (chain steps per position; default 24) and the lazy match (default on) */
extern int cf_pack_depth, cf_pack_lazy, cf_pack_chains;   /* chains 0: as if their memory were not there */
#endif
