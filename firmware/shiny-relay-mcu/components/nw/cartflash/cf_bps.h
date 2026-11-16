/* cf_bps.h: a BPS patch (byuu's beat format, version 1) applied on the unit, streaming (Shiny Relay 1.0.5,
 * Tetris PARTY BATTLE over Wi-Fi). Nothing is held whole in RAM: the source, the patch and the target are
 * read and written through the caller's callbacks (on the ESP32: the staging area in its own flash), with
 * one 4 KiB target page and a few small buffers (CF_BPS_WORK bytes, the caller's).
 * The same rules as the cart page's lib/rompatch.js applyBps: the patch's own crc32, the source's size and
 * crc32, every action inside its bounds, the target filled exactly, the target's crc32. The caller pins the
 * source and target sha256 on top (cf_pbundle). Portable C: host-tested against the real patch files. */
#ifndef CF_BPS_H
#define CF_BPS_H
#include <stdint.h>

#define CF_BPS_PAGE 4096u
#define CF_BPS_WORK (CF_BPS_PAGE + 3u * 256u)

enum { CF_BPS_OK = 0, CF_BPS_E_PATCH = 1, CF_BPS_E_SOURCE = 2, CF_BPS_E_RESULT = 3, CF_BPS_E_IO = 4 };

typedef struct {
    void *u;
    int (*src_read)(void *u, uint32_t off, uint8_t *b, uint32_t n);         /* 0 ok */
    int (*pat_read)(void *u, uint32_t off, uint8_t *b, uint32_t n);         /* 0 ok (offset within the patch) */
    int (*tgt_read)(void *u, uint32_t off, uint8_t *b, uint32_t n);         /* bytes already written by tgt_write */
    int (*tgt_write)(void *u, uint32_t off, const uint8_t *b, uint32_t n);  /* in order, CF_BPS_PAGE at a time (the last shorter) */
} cf_bps_io_t;

typedef struct {
    uint32_t src_size, tgt_size, src_crc, tgt_crc;   /* from the patch's header and footer */
} cf_bps_info_t;

/* the header and footer only (the patch's own crc32 checked): 0 ok */
int cf_bps_info(const cf_bps_io_t *io, uint32_t patch_len, uint8_t *work, cf_bps_info_t *inf);
/* the whole apply. src_len = the source's bytes; max_tgt = the most the target may be. On CF_BPS_OK the
   target is written and *tgt_crc / tgt_sha (sha256 of the target) are filled. why: a short reason. */
int cf_bps_apply(const cf_bps_io_t *io, uint32_t patch_len, uint32_t src_len, uint32_t max_tgt, uint8_t *work,
                 uint32_t *tgt_len, uint32_t *tgt_crc, uint8_t tgt_sha[32], const char **why);

/* ---- the patch bundle the site sends (kind "patch"): every patch of one release for one game, the unit
 * picks the one made for the cart it holds. Little endian:
 *   "SRPB" u8 version (1) u8 count (1..32) u16 0
 *   count entries of 80 bytes: source sha256[32], target sha256[32], source size u32, target size u32,
 *                              offset u32 (of the BPS file in the bundle), length u32
 *   the BPS files */
#define CF_PB_HDR   8u
#define CF_PB_ENTRY 80u
#define CF_PB_MAX   32
typedef struct {
    uint8_t  src_sha[32], tgt_sha[32];
    uint32_t src_size, tgt_size, off, len;
} cf_pb_entry_t;
/* the bundle's index, checked against its length: count (>0) or -1 */
int cf_pb_count(const uint8_t hdr[CF_PB_HDR], uint32_t bundle_len);
int cf_pb_entry(const uint8_t raw[CF_PB_ENTRY], uint32_t bundle_len, cf_pb_entry_t *e);   /* 0 ok */
#endif
