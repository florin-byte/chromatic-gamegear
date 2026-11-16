/* sd_core.h: lane sdcard1: the card library's portable rules (host-tested: host/sd_core_test.c).
 * No ESP-IDF, no FatFs: the game header, file names, the index record and its sort, the save's RTC footer. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define SDC_GAMES_DIR   "/GAMES"
#define SDC_SAVES_DIR   "/SAVES"
#define SDC_SHINY_DIR   "/SHINY"
#define SDC_INDEX       "/SHINY/LIBRARY.IDX"
#define SDC_INDEX_TMP   "/SHINY/LIBRARY.TMP"
#define SDC_UNSORTED    "/SHINY/LIBRARY.NEW"
/* fr1: the folder view (the GAMES library page): the games in folder order, the folders, the folders' order */
#define SDC_DIRIDX      "/SHINY/LIBDIR.IDX"
#define SDC_DIRIDX_TMP  "/SHINY/LIBDIR.TMP"
#define SDC_FOLDERS     "/SHINY/FOLDERS.IDX"
#define SDC_FOLDERS_TMP "/SHINY/FOLDERS.TMP"
#define SDC_FORDER      "/SHINY/FORDER.IDX"
#define SDC_FORDER_TMP  "/SHINY/FORDER.TMP"
#define SDC_RECENT      "/SHINY/RECENT.TXT"
#define SDC_RECENT_MAX  10u
#define SDC_PATH_MAX    160                 /* bytes of a game's path in the index, NUL included (fr1: was 88) */
#define SDC_NAME_LEN    72                  /* bytes of the name shown (fr1: was 24; the library page scrolls it) */
#define SDC_MAX_FOLDERS 4000u               /* folders with games the folder view keeps (more: their games only in A-Z) */
#define SDC_ROM_MAX     0x400000u           /* the RAM cart's 256 banks (FPGA 18.30 ramcart.v) */
#define SDC_SAVE_MAX    0x20000u            /* 16 banks of 8 KiB */
#define SDC_RTC_FOOTER  48u
#define SDC_MAX_GAMES   20000u
#define SDC_DEPTH_MAX   6                   /* folders below the root that are searched for games */

/* a cart header, as the RAM cart plays it */
typedef struct {
    char     title[17];                     /* ASCII, upper case, trimmed ("" when the header has none) */
    uint8_t  cgb;                           /* 0x143 */
    uint8_t  type, romsz, ramsz;            /* 0x147 0x148 0x149 */
    int8_t   mbc;                           /* 0 1 2 3 5, -1 a mapper the RAM cart does not play */
    uint8_t  battery, rtc;
    uint16_t rom_banks;                     /* 16 KiB banks the header says */
    uint32_t sav_size;                      /* bytes of save RAM (0 none) */
    const char *why;                        /* not playable: the OSD's word ("" when playable) */
} sdc_hdr_t;
/* 0 = playable; the header at h (0x150 bytes) of a file of `size` bytes */
int  sdc_hdr_parse(const uint8_t *h, uint32_t size, sdc_hdr_t *o);
/* m110t: the size rule every place plays and keeps by (the card, the RAM cart, the library): the header's ROM size byte
 * (0x148) and the file's length. A file SHORTER than its header's size is a game too (trimmed: its trailing FF bytes cut
 * off, e.g. SHINY COLLECTION, 1,769,472 bytes for a 2 MiB header): it plays with the rest of its banks read as 0xFF, the
 * open-bus value a cart's erased flash and the emulators (SameBoy pads to the next power of two with 0xFF) give; the
 * player fills them. 0 = it fits (rom_banks the header's), else why the OSD word: "TOO  BIG" (the header says more than
 * 4 MiB, or the file is longer than max), "SIZE" (shorter than a header). */
int  sdc_rom_fit(uint8_t romsz, uint32_t size, uint32_t max, uint16_t *rom_banks, const char **why);

/* file names */
int  sdc_is_game_name(const char *name);                 /* .gb / .gbc (any case), not hidden */
int  sdc_skip_dir(const char *name);                     /* folders never searched (hidden, the system's, ours) */
/* /SAVES/<the ROM file name without its extension>.sav; 0 = fits */
int  sdc_save_path(const char *rom_path, char *out, size_t n);
/* the name to show: the file name the user chose (no extension, no trailing "(USA)" / "[!]" tags, _ as a space,
   upper case ASCII), else the header title; at most n - 1 characters */
void sdc_display_name(const char *title, const char *path, char *out, size_t n);
/* a file name that came over Wi-Fi made safe for the card (basename, a safe set, <= 60, ext kept); 0 = usable */
int  sdc_clean_file(const char *in, const char *want_ext, char *out, size_t n);

/* the index: a header the size of a record, then records sorted by title (then by when they were found).
 * fr1 (version 4): 256-byte records (the whole name, longer paths, the game's folder). LIBDIR.IDX holds the same
 * records (and header) sorted by folder, then name: a folder's games are one run of it (sdc_folder_t). */
#define SDC_IDX_MAGIC   0x31424C53u          /* "SLB1" */
#define SDC_REC_SIZE    256u
#define SDC_IDX_VERSION 5u                  /* 5: GG marker and per-core views; 4: 256-byte records and folders */
enum { SDC_F_PLAYABLE = 1, SDC_F_BATTERY = 2, SDC_F_RTC = 4, SDC_F_GG = 8 };
typedef struct {
    char     title[SDC_NAME_LEN];           /* the name shown (sdc_display_name); its first 16 bytes the sort key */
    uint8_t  cgb, type, romsz, ramsz, flags;
    uint8_t  pad[3];
    uint32_t size;
    uint32_t stamp;                         /* FAT date << 16 | time */
    uint32_t folder;                        /* fr1: its folder (FOLDERS.IDX id; 0 = the card's top) */
    uint8_t  reserved[4];
    char     path[SDC_PATH_MAX];
} sdc_rec_t;
typedef struct {
    uint32_t magic, version, count, fingerprint;
    uint32_t too_long, skipped, built_ms;
    uint32_t folders;                       /* fr1: records in FOLDERS.IDX (0: no folder view) */
    uint8_t  pad[SDC_REC_SIZE - 32];
} sdc_idx_hdr_t;
_Static_assert(sizeof(sdc_rec_t) == SDC_REC_SIZE, "index record");
_Static_assert(sizeof(sdc_idx_hdr_t) == SDC_REC_SIZE, "index header");
void sdc_rec_make(sdc_rec_t *r, const char *path, uint32_t size, uint32_t stamp, const sdc_hdr_t *h, int hdr_ok);

/* fr1: a folder of the card (FOLDERS.IDX: a header the size of a record, then one record per folder by id, in the
 * order the walk entered them; id 0 is the card's top). Its folders are FORDER.IDX[child_first ...] (u32 ids, a
 * folder's children sorted by name, only folders with games in them or below), its games LIBDIR.IDX[game_first ...]. */
#define SDC_FOLDER_SIZE 128u
#define SDC_FNAME_LEN   72
#define SDC_FOLDER_OFF(id) ((uint32_t)SDC_REC_SIZE + (uint32_t)SDC_FOLDER_SIZE * (uint32_t)(id))   /* after the header (a record long) */
typedef struct {
    char     name[SDC_FNAME_LEN];           /* as on the card ("" for the top) */
    uint32_t parent, depth;
    uint32_t child_first, child_count;
    uint32_t game_first, game_count;
    uint32_t total;                         /* games in it and in every folder below */
    uint8_t  pad[SDC_FOLDER_SIZE - SDC_FNAME_LEN - 28];
} sdc_folder_t;
_Static_assert(sizeof(sdc_folder_t) == SDC_FOLDER_SIZE, "folder record");
/* the sort keys of the folder order (sdc_sort's 16 bytes): an id in 3 characters that sort as numbers ('0' + 6 bits
 * each), then the first 13 characters of a name, upper case */
void sdc_key_folder(char t[17], uint32_t id, const char *name);

/* the walk's fingerprint: FNV-1a over every game file's (path, size, stamp), in walk order */
uint32_t sdc_fp_start(void);
uint32_t sdc_fp_add(uint32_t fp, const char *path, uint32_t size, uint32_t stamp);

/* the sort in bounded memory: passes of the `cap` smallest (title, index) above the last one emitted.
 * get(u, i, title) gives record i's title (17 bytes); emit(u, i) appends record i to the sorted index.
 * work: cap * sizeof(sdc_key_t) bytes. 0 = done. */
typedef struct { char t[16]; uint32_t i; } sdc_key_t;
int  sdc_sort(uint32_t n, int (*get)(void *u, uint32_t i, char t[17]), int (*emit)(void *u, uint32_t i), void *u,
              sdc_key_t *work, uint32_t cap, uint32_t *passes);
/* get() may read in bulk: sdc_sort calls it for i = 0, 1, ... n-1 in order within each pass */

/* the MBC3 clock in the save file's 48-byte footer (VBA-M / BGB / mGBA): {S M H DL DH} live, then latched, as u32 LE, then u64 LE unix time */
void sdc_rtc_put(uint8_t f[SDC_RTC_FOOTER], const uint8_t rtc[5], uint64_t unix_time);
int  sdc_rtc_get(const uint8_t *f, uint32_t len, uint8_t rtc[5], uint64_t *unix_time);   /* 0 = a footer (44 or 48 bytes) */
/* the clock advanced by s seconds (as the FPGA counts it: the day counter carries into DH bit 7) */
void sdc_rtc_advance(uint8_t rtc[5], uint64_t s);

/* 1.1.7: a game's clock from the load to its saves. The unit has no clock of its own when it is off, so the real time comes
 * from the network at power-on; a game that is saved says WHEN its clock was true (t), and the next load advances the clock by
 * the time that passed. When the real time is not known (no Wi-Fi), the clock is neither moved nor lost: the time the game
 * ran (uptime) is added to the last real time known for it, so the next time the network is there the whole gap is caught up.
 * All times are unix seconds, 0 = not known; up is the unit's uptime in seconds. */
typedef struct {
    uint64_t t_ref;     /* the real time the game's clock was true at the load; 0 = not known */
    int64_t  up0;       /* the uptime at the load */
    uint8_t  pending;   /* the load could not catch the clock up (no real time then): a save with the time known does */
} sdc_clk_t;
/* rtc: the clock as saved (advanced here when both times are known). 1 = it was advanced */
int  sdc_clk_load(sdc_clk_t *c, uint8_t rtc[5], uint64_t t_saved, uint64_t now, int64_t up);
/* rtc: the live clock (advanced here when the load could not catch up and the time is known now). Returns the time to keep
 * with it (0 = not known). Never moves the clock back; a halted clock does not move. */
uint64_t sdc_clk_save(const sdc_clk_t *c, uint8_t rtc[5], uint64_t now, int64_t up);

/* a time kept in 24 bits (the library's journal record has 3 spare bytes): whole seconds after 2025-01-01 modulo 2^24 (194 days),
 * 0 = not known. Whole seconds, so saving and loading never rounds: with no time passing the game clock never moves, and the
 * catch-up is exact. The day count is told by an anchor (the real time now, else the last one known): a time up to 194 days before
 * it (less SDC_TSEC_AHEAD) is read right; a gap longer than that is read as its remainder (the game clock counts 511 days at most). */
#define SDC_T_EPOCH 1735689600ull
#define SDC_TSEC_MASK 0xFFFFFFu
#define SDC_TSEC_AHEAD 604800u                         /* a saved time up to 7 days ahead of the anchor is read as ahead, not as 187 days ago */
uint32_t sdc_tsec_pack(uint64_t t);                    /* 0 = not known (t 0 or before 2025) */
uint64_t sdc_tsec_unpack(uint32_t v, uint64_t anchor); /* 0 = not known (v 0, or no anchor) */

uint32_t sdc_crc32(uint32_t c, const void *b, size_t n);
