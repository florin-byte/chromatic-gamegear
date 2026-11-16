/* cf_lib.h: the game library on the FPGA's boot flash (lane gamecomp1): the games a person keeps on the Chromatic,
 * each with its own save (and MBC3 clock), crash safe. Portable C over a flash interface: on the unit the GAME AREA ops
 * of cf_rc_dev.c (FPGA 18.31: 4 KiB erase, program, FREAD, crc), on the host a NOR model with power cuts (host/cf_lib_test.c).
 *
 * The area is cut into 4 KiB SECTORS (the erase size); the library's own bookkeeping is the first 64 KiB:
 *   sectors 0-7   the INDEX ring: each index write goes into the next sector (erased first) with seq + 1; the live index
 *                 is the valid one (magic, version, crc32) with the highest seq. A cut while one is written leaves the
 *                 one before it live.
 *   sectors 8-15  the SAVE JOURNAL: 8 x 128 records of 32 bytes (game uid, seq, slot, size, crc32 of the slot's bytes,
 *                 the clock, the record's own crc32), written in order; a game's save is the slot its newest valid
 *                 record names. Before a sector is erased for reuse, the newest record of every game that lives only
 *                 there is written again (carried) into the sector being left, so nothing live is ever erased.
 *   sectors 16+   games and save slots, as EXTENTS (runs of sectors): a game's bytes fill up to 8 extents in order; its
 *                 save region (one extent) holds K >= 2 slots of the save's size (rounded up to 4 KiB), used in turn
 *                 (the slot written is never the live one).
 * A game is added by: sectors chosen from the free ones, erased as the bytes reach them (64 KiB at a time where they
 * line up), programmed, each extent read back by crc32, its save region erased, THEN the index that has it. A cut before
 * the index: the sectors are free again. The bytes can come all at once (cf_lib_add) or as a stream (cf_lib_add_begin /
 * cf_lib_tx_write / cf_lib_add_end: a Wi-Fi download straight into the flash, or a USB copy in parts across restarts:
 * the transaction is a small struct the caller may keep). Deleted: an index without it. uids never repeat.
 * Wear: index sectors take one erase per 8 index writes, journal sectors one per ~900 saves, a save slot one per K saves
 * of its game (a 32 KiB save: K = 2; 8 KiB: K = 4; 2 KiB: K = 8). */
#ifndef CF_LIB_H
#define CF_LIB_H
#include <stdint.h>

#define CF_LIB_SECT        0x1000u
#define CF_LIB_META        16u            /* sectors of bookkeeping: index ring 8 + journal 8 */
#define CF_LIB_BUF         0x800u         /* scratch: 2 KiB */
#define CF_LIB_MAX_GAMES   16
#define CF_LIB_MAX_SECT    1024u          /* a 4 MiB area at most */
#define CF_LIB_EXT         8              /* extents per game */
#define CF_LIB_TITLE       24
#define CF_LIB_SAVE_MAX    0x20000u       /* 128 KiB of save RAM (MBC5's most) */

/* errors */
#define CF_LIB_E_FLASH   (-1)
#define CF_LIB_E_FULL    (-2)             /* not enough free room (cf_lib_need says how many bytes more) */
#define CF_LIB_E_MANY    (-3)             /* 16 games already */
#define CF_LIB_E_NONE    (-4)             /* no such game */
#define CF_LIB_E_ARG     (-5)
#define CF_LIB_E_VERIFY  (-6)             /* the bytes read back are not the bytes written */
#define CF_LIB_E_NOAREA  (-7)             /* no area (this FPGA flash has no game area) */
#define CF_LIB_E_STALE   (-8)             /* the library changed under an open transaction (its room taken): begin again */

typedef struct {
    void *u;
    uint32_t base, size;                                   /* the area: base 4 KiB aligned, size a multiple of 4 KiB */
    int (*erase)(void *u, uint32_t a, uint32_t n);         /* 4 KiB aligned; 0 ok */
    int (*prog)(void *u, uint32_t a, const uint8_t *b, uint32_t n);   /* inside erased flash */
    int (*read)(void *u, uint32_t a, uint8_t *b, uint32_t n);
    int (*crc)(void *u, uint32_t a, uint32_t n, uint32_t *crc);      /* crc32 of the bytes (may be NULL: read is used) */
} cf_lib_flash_t;

typedef struct { uint16_t start, count; } cf_lib_ext_t;   /* sectors [start, start + count) of the area */

#define CF_LIB_F_SGZ  0x01u               /* the stored bytes are an SGZ1 container (else the ROM itself) */
#define CF_LIB_F_RTC  0x02u               /* the game has the MBC3 clock */
typedef struct {
    uint32_t uid;
    uint8_t  flags, next, kslots, rsv;    /* next: extents used */
    cf_lib_ext_t ext[CF_LIB_EXT];         /* the game's bytes, in order */
    cf_lib_ext_t sext;                    /* its save region (count 0: no save) */
    uint32_t stored_size, stored_crc;     /* the bytes kept (container or ROM) and their crc32 */
    uint32_t rom_size, rom_crc32;         /* the game */
    uint32_t save_size;                   /* bytes of save RAM (0: none) */
    uint32_t added;                       /* the index seq it came in with (the list's order) */
    char     title[CF_LIB_TITLE];
} cf_lib_ent_t;

typedef struct {
    uint32_t magic, seq;
    uint16_t version, n;
    uint32_t next_uid;
    cf_lib_ent_t e[CF_LIB_MAX_GAMES];
    uint32_t crc;                         /* of everything above */
} cf_lib_index_t;

typedef struct {
    uint32_t uid, seq;                    /* seq 0 = no save yet */
    uint16_t slot;
    uint32_t size, crc;
    uint8_t  rtc[5];
    uint32_t tsec;                        /* 1.1.7: when the clock was true: whole seconds after 2025-01-01 modulo 2^24 (sd_core.h sdc_tsec_*), 0 = not known */
} cf_lib_save_t;

/* a game being added as a stream (small: the caller may keep it, e.g. in NVS across a restart) */
typedef struct {
    uint32_t magic;
    cf_lib_ent_t e;                       /* the entry it will be (its extents chosen at begin) */
    uint32_t have, crc, erased;           /* bytes written, their crc32, bytes of its extents erased so far */
    uint32_t ecrc[CF_LIB_EXT];            /* crc32 of each extent's bytes as written */
    uint32_t seq0;                        /* the index seq at begin */
} cf_lib_tx_t;

typedef struct {
    const cf_lib_flash_t *f;
    int ok, fresh;                        /* fresh: no index found (the journal is erased at the first add) */
    uint32_t nsect;
    uint8_t  isect;                       /* the live index's sector */
    cf_lib_index_t ix;
    cf_lib_index_t nx;                    /* the next index being made (heap, not the caller's stack) */
    int jok;                              /* the journal (scanned at the first save asked for, then kept up to date) */
    uint8_t  jsect;
    uint16_t jnext;
    uint32_t jseq;
    cf_lib_save_t sv[CF_LIB_MAX_GAMES];   /* per index entry (same order) */
    uint8_t  svsect[CF_LIB_MAX_GAMES];    /* the sector each newest record is in (0xFF: none) */
    int      ready;                       /* savefast1: ready_slot of ready_uid is erased (this boot only) */
    uint32_t ready_uid;
    uint16_t ready_slot;
    uint8_t  buf[CF_LIB_BUF];             /* scratch */
} cf_lib_t;

int  cf_lib_open(cf_lib_t *L, const cf_lib_flash_t *f);      /* the live index (none = an empty library); 0 ok */
int  cf_lib_format(cf_lib_t *L);                              /* bench: an empty library */
uint32_t cf_lib_total_bytes(const cf_lib_t *L);               /* what games + saves may use */
uint32_t cf_lib_free_bytes(const cf_lib_t *L);
uint32_t cf_lib_save_region(uint32_t save_size, uint8_t *kslots);
uint32_t cf_lib_cart_save(uint8_t type, uint8_t ram);                /* the save bytes a cart header (0x147, 0x149) asks for */   /* bytes of a game's save region */
uint32_t cf_lib_room_for(uint32_t stored_size, uint32_t save_size);  /* bytes a game takes (its sectors + save region) */
/* 0 = it fits now; else the bytes still missing (delete games to make them) */
uint32_t cf_lib_need(const cf_lib_t *L, uint32_t stored_size, uint32_t save_size);
int  cf_lib_find(const cf_lib_t *L, uint32_t uid);            /* index entry number, -1 none */
int  cf_lib_find_rom(const cf_lib_t *L, uint32_t rom_crc32, uint32_t rom_size);

/* adding a game: meta's stored_size, sizes, flags, title. Stream: begin chooses its sectors; tx_write takes the bytes
   in order (any lengths); end checks them and writes the index. All at once: get(u, off, b, n) (n <= 2048, in order).
   The game is in the library (and *uid set) only when add / add_end returns 0. */
int  cf_lib_add_begin(cf_lib_t *L, const cf_lib_ent_t *meta, cf_lib_tx_t *tx);
/* the same from a free map taken earlier (cf_lib_free_map): a download plans its sectors with the library closed */
void cf_lib_free_map(const cf_lib_t *L, uint32_t map[CF_LIB_MAX_SECT / 32], uint32_t *nsect, uint32_t *seq);
int  cf_lib_plan(const uint32_t map[CF_LIB_MAX_SECT / 32], uint32_t nsect, uint32_t seq, int games, const cf_lib_ent_t *meta, cf_lib_tx_t *tx);
/* the save size once the game's header is known: the planned save region shrinks to it, or a bigger one is found */
int  cf_lib_tx_set_save(cf_lib_t *L, cf_lib_tx_t *tx, uint32_t save_size);
int  cf_lib_tx_write(const cf_lib_flash_t *f, cf_lib_tx_t *tx, const uint8_t *b, uint32_t n);
void cf_lib_tx_rewind(cf_lib_tx_t *tx);              /* from the first byte again (a download hashed wrong) */
int  cf_lib_tx_read(const cf_lib_flash_t *f, const cf_lib_tx_t *tx, uint32_t off, uint8_t *b, uint32_t n);   /* bytes written so far */
int  cf_lib_add_end(cf_lib_t *L, cf_lib_tx_t *tx, uint32_t *uid);
int  cf_lib_add(cf_lib_t *L, const cf_lib_ent_t *meta, int (*get)(void *u, uint32_t off, uint8_t *b, uint32_t n), void *gu,
                uint32_t *uid, void (*pct)(void *u, int p));
int  cf_lib_del(cf_lib_t *L, uint32_t uid);
/* the stored bytes of a game [off, off + n) */
int  cf_lib_read(cf_lib_t *L, uint32_t uid, uint32_t off, uint8_t *b, uint32_t n);
/* the game's newest save: *sv (seq 0 = none yet); its bytes [off, off + n) by cf_lib_save_read */
int  cf_lib_save_get(cf_lib_t *L, uint32_t uid, cf_lib_save_t *sv);
int  cf_lib_save_read(cf_lib_t *L, uint32_t uid, const cf_lib_save_t *sv, uint32_t off, uint8_t *b, uint32_t n);
/* a new save (get gives its bytes, n <= 2048, in order): into the next slot, read back, then its journal record */
int  cf_lib_save_prepare(cf_lib_t *L, uint32_t uid);          /* savefast1: erase the slot the next save goes to, ahead of it */
int  cf_lib_save_put(cf_lib_t *L, uint32_t uid, uint32_t size, const uint8_t rtc[5],
                     int (*get)(void *u, uint32_t off, uint8_t *b, uint32_t n), void *gu);
/* the same with the time the clock was true (the journal record's spare bytes: records from before have 0 there = not known) */
int  cf_lib_save_put_t(cf_lib_t *L, uint32_t uid, uint32_t size, const uint8_t rtc[5], uint32_t tsec,
                       int (*get)(void *u, uint32_t off, uint8_t *b, uint32_t n), void *gu);
#endif
