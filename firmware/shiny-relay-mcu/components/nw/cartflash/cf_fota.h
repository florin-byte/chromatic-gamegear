/* cf_fota.h: the FPGA's own boot flash (4 MiB SPI NOR), laid out for updates over Wi-Fi with a recovery design
 * (layout "rf", FPGA 18.31, lane fpgacomp1; measured on units 1 and 2: rel103_work/fpgacomp1/NOTES.md and
 * fpgaota1/FPGA_BOOT_FINDINGS.md). Replaces 1.1.0's two image slots A/B ("ab", never shipped).
 *
 * The GW5A-25 boots from 0x000000, scans up for a sync (A5 C3) in the first 2 KiB and boots the FIRST complete
 * jump header it finds; a header whose sync is not complete (or programmed to 00 00) is skipped; there is no
 * fallback after that (a bad image behind a complete sync does not start). So:
 *   0x000000-0x0007FF  32 jump headers, 64 B each. 0x7C0 = the RECOVERY header (-> 0x000800), written once by the
 *                      USB install and never by the ESP32 (the FPGA refuses FHDR at 0x7C0 and up). 0x780 down to
 *                      0x000 = FULL design headers, the lowest complete one is live; an update invalidates the live
 *                      one (sync -> 00 00) before it touches the full design, and writes the next lower one
 *                      (everything but the sync, then C3, then A5) after the design checks out. While no full header
 *                      is live, the recovery design boots.
 *   0x000800-0x05BFFF  the recovery design (USB install only; the FPGA refuses erase/program below 0x05C000).
 *   0x05C000-0x25DFFF  the GAME AREA (fixed, independent of the live header).
 *   0x260000-0x32FFFF  slot GG (1); 0x330000-0x3FFFFF slot GB (0).
 *                      Each design sits at the top of its slot, rounded up to 4 KiB.
 *   0x25E000-0x25FFFF  selector A and B (one 4 KiB sector each). A level-1 header points to a selector;
 *                      its 31 level-2 headers point to designs. Both levels use the same cut-safe write order.
 * Switching appends in the active selector; migration or rotation consumes one level-1 place.
 * A selector is erased only after every complete level-1 reference to it has been invalidated.
 * Sector 0 is never erased by the ESP32 (FHDRERASE is refused by 18.31): after 31 full headers (the USB install
 * writes the first) the next FPGA update or selector rotation is a USB one.
 * A new full design takes over at the next power-on (nothing here can restart the FPGA).
 * Portable C: the flash ops come from the host (cart_xs ops 17-22 through the FPGA). */
#ifndef CF_FOTA_H
#define CF_FOTA_H
#include <stdint.h>

#define CF_FOTA_HDR_STEP   0x40u
#define CF_FOTA_HDR_AREA   0x800u                 /* the chip's sync scan stops between 0x7E2 and 0x822 */
#define CF_FOTA_REC_HDR    0x7C0u                 /* the recovery header */
#define CF_FOTA_FULL_TOP   0x780u                 /* the first full header (the USB install's) */
#define CF_FOTA_HDRS       31                     /* full header places: 0x780, 0x740, ... 0x000 */
#define CF_FOTA_HDR_LEN    54u
#define CF_FOTA_SYNC_AT    34u                    /* A5 C3 at place + 34, + 35 */
#define CF_FOTA_TARGET_AT  48u                    /* the jump target (u32 BE) at place + 48 */
#define CF_FOTA_REC_BASE   0x000800u
#define CF_FOTA_REC_MAX    0x05B800u              /* 0x000800-0x05BFFF */
#define CF_FOTA_GAME_BASE  0x05C000u
#define CF_FOTA_GAME_END   0x25E000u
#define CF_FOTA_SELECTOR_A 0x25E000u
#define CF_FOTA_SELECTOR_B 0x25F000u
#define CF_FOTA_GG_BASE    0x260000u
#define CF_FOTA_GB_BASE    0x330000u
#define CF_FOTA_SLOT_GB    0
#define CF_FOTA_SLOT_GG    1
#define CF_FOTA_FLASH_END  0x400000u
#define CF_FOTA_FULL_MAX   0x0D0000u              /* the biggest full design this firmware installs (Gowin: <= 928 KB raw) */
#define CF_FOTA_IMG_MIN    0x40000u               /* a GW5A-25 image: compressed >= ~0.3 MB */
#define CF_FOTA_SECTOR     0x1000u

/* A target belongs to a slot only when it is sector aligned. */
static inline int cf_fota_slot_of(uint32_t target)
{
    if (target & (CF_FOTA_SECTOR - 1u)) return -1;
    if (target >= CF_FOTA_GB_BASE && target < CF_FOTA_FLASH_END) return CF_FOTA_SLOT_GB;
    if (target >= CF_FOTA_GG_BASE && target < CF_FOTA_GB_BASE) return CF_FOTA_SLOT_GG;
    return -1;
}
static inline uint32_t cf_fota_slot_end(int slot)
{
    return slot == CF_FOTA_SLOT_GB ? CF_FOTA_FLASH_END : slot == CF_FOTA_SLOT_GG ? CF_FOTA_GB_BASE : 0;
}
/* The GB slot's top, retained for callers constructing a USB layout. */
static inline uint32_t cf_fota_full_base(uint32_t size) { return CF_FOTA_FLASH_END - ((size + CF_FOTA_SECTOR - 1u) & ~(CF_FOTA_SECTOR - 1u)); }

enum { CF_FL_RF = 1,      /* the update layout, a full design live, a header place free */
       CF_FL_RF_FULL,     /* the update layout, a full design live, no header place left: USB */
       CF_FL_RF_REC,      /* the update layout, NO full header live: the recovery design boots */
       CF_FL_LEGACY };    /* anything else (the stock image at 0, 1.1.0's A/B, a missing recovery header): USB */

typedef struct {
    void *user;
    /* crc32 (zlib) and "all FF" of n bytes at a: 0 = ok */
    int (*fcrc)(void *u, uint32_t a, uint32_t n, uint32_t *crc, int *all_ff);
    /* n (<= 64) bytes into the header area at a (a + n <= 0x7C0); FF bytes are left alone: 0 = ok */
    int (*hdr)(void *u, uint32_t a, const uint8_t *b, uint32_t n);
    /* the 4 KiB sector (len 0x1000) or the 64 KiB block (len 0x10000, a aligned) at a (>= 0x05C000) erased: 0 = ok */
    int (*erase)(void *u, uint32_t a, uint32_t len);
    /* n (<= 4096) bytes programmed at a (>= 0x05C000, only clears bits; FF preserves existing bytes): 0 = ok */
    int (*prog)(void *u, uint32_t a, const uint8_t *b, uint32_t n);
    /* n (<= 256) flash bytes at a into b (FREAD): 0 = ok */
    int (*read)(void *u, uint32_t a, uint8_t *b, uint32_t n);
} cf_fota_host_t;

typedef struct {
    int      kind;        /* CF_FL_* */
    uint32_t live_at;     /* the live full header's place (CF_FL_RF / _FULL), CF_FOTA_REC_HDR for CF_FL_RF_REC */
    uint32_t full_base;   /* where the full design is: the live header's target, else the newest written header's
                             (an invalidated one keeps its target), else 0 (none: a USB install's layout always has one) */
    uint32_t next_at;     /* the next full header's place (valid while free > 0) */
    int      used;        /* full header places written (whole, invalidated or cut) */
    int      free;        /* full header places left */
    uint32_t l1_target;   /* live level-1 target, or newest complete body during recovery */
    uint32_t selector;    /* selector sector in use, 0 for direct */
    uint32_t l2_next;     /* absolute address of the next level-2 place */
    int      l2_free;     /* places below every written level-2 place */
} cf_fota_layout_t;

static inline int cf_fota_switches_left(const cf_fota_layout_t *L)
{
    if (L->kind != CF_FL_RF && L->kind != CF_FL_RF_FULL) return -1;
    return L->free * CF_FOTA_HDRS + (L->selector ? L->l2_free : 0);
}

/* the 64 B header place that jumps to target (54 header bytes, FF after) */
void cf_fota_header(uint8_t out[64], uint32_t target);
uint16_t cf_fota_crc16(const uint8_t *b, uint32_t n);
uint32_t cf_fota_crc32(uint32_t c, const uint8_t *b, uint32_t n);
/* read the header area and, for a chain, its selector: 0 = read (L filled), else a flash error */
int cf_fota_scan(const cf_fota_host_t *h, cf_fota_layout_t *L);
/* every complete full header made skippable (its sync programmed to 00 00, read back): 0 = no full header is live */
int cf_fota_invalidate(const cf_fota_host_t *h, cf_fota_layout_t *L);
/* program the next full header -> target in the safe order, each step read back: 0 = it is live (L updated) */
int cf_fota_commit(const cf_fota_host_t *h, cf_fota_layout_t *L, uint32_t target);
/* a GW5A-25 image as Gowin writes it (the sync + ID check at the start, this project's user code at the end):
   0 = plausible. first = its first 64 bytes, last = its last 64 bytes. */
int cf_fota_image_ok(const uint8_t first[64], const uint8_t last[64], uint32_t size);

/* the whole FPGA update, from any state it can be cut in (resumable, idempotent): the full design read from
   src (the staged copy: n <= 4096 bytes at offset o) and checked against crc; then:
     the live full design is already this one (crc) -> 0, nothing written;
     no header place left -> CF_FOTA_E_FULL; too big -> CF_FOTA_E_SIZE, nothing written;
     else: the live full header invalidated, the place erased + programmed + read back, the next header committed.
   progress(u, done, total) is called now and then (may be NULL); buf = 4 KiB of the caller's (heap: DRAM is short).
   0 = the new design boots at the next power-on. */
typedef int (*cf_fota_src_t)(void *u, uint32_t o, uint8_t *b, uint32_t n);
enum { CF_FOTA_E_FLASH = -1, CF_FOTA_E_LAYOUT = -3, CF_FOTA_E_FULL = -4, CF_FOTA_E_SRC = -5, CF_FOTA_E_CHECK = -6,
       CF_FOTA_E_SIZE = -7, CF_FOTA_E_GAMES = -8, CF_FOTA_E_EMPTY = -9 };
/* slot = -1 selects the slot of L.full_base (GB if none). Before the first mutation, the caller must
   persist the chosen slot and pass it explicitly on every resume: an older header may become live during
   invalidation. No blank-game check is needed because both slots are reserved. */
int cf_fota_install(const cf_fota_host_t *h, cf_fota_src_t src, void *src_u, uint32_t size, uint32_t crc, int slot,
                    void (*progress)(void *u, uint32_t done, uint32_t total), void *pu, uint8_t buf[4096]);
/* Find the first nonblank sector; CF_FOTA_E_EMPTY if the reserved slot is blank. */
int cf_fota_slot_base(const cf_fota_host_t *h, int slot, uint32_t *base);
/* Append a level-2 header, migrating or rotating with one level-1 header if needed. Prefix checks only. */
int cf_fota_switch(const cf_fota_host_t *h, cf_fota_layout_t *L, int slot, uint8_t buf[4096]);

/* The fixed game area of an rf layout; both design slots stay reserved, including in recovery. */
static inline int cf_fota_game_region(const cf_fota_layout_t *L, uint32_t *base, uint32_t *size)
{
    if (!L || L->kind == CF_FL_LEGACY || L->kind == 0) return -1;
    *base = CF_FOTA_GAME_BASE; *size = CF_FOTA_GAME_END - CF_FOTA_GAME_BASE;
    return 0;
}
#endif
