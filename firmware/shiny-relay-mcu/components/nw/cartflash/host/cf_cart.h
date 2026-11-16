/* cf_cart.h: a simulated ModRetro-style flash cartridge (lane cartflash1).
 *
 * What the research says these carts are (CART_FLASH_DESIGN.md "What the cart is"):
 * an AMD-command parallel NOR flash with /WE on the cart /WR line (unlock 0xAAA/0x555),
 * a 32 KiB parallel FRAM, a small Gowin FPGA as an MBC5-like mapper. The model:
 *  - the flash command state machine (autoselect, CFI, byte program, sector/block erase,
 *    chip erase, reset), busy time with DQ6 toggle / DQ7 data# status reads, program =
 *    AND (bits only go 1 -> 0), erase = FF, stuck-at-0 bits as a fault, writes while busy
 *    counted (a correct writer makes none), stray single writes ignored;
 *  - a mapper that is MBC5 (the research's reading) or, as a hypothesis variant, picks
 *    MBC1 / MBC3 / MBC5 / none from header byte 0x147 at power-on (unknown types: MBC5);
 *  - variants: D0/D1 swapped on the board, /WE not reaching the chip (gated mapper);
 *  - power cut: an operation in flight leaves its bytes indeterminate.
 */
#ifndef CF_CART_H
#define CF_CART_H
#include <stdint.h>

enum { CM_MBC5 = 0, CM_HEADER = 1 };
enum { MK_NONE = 0, MK_MBC1 = 1, MK_MBC3 = 3, MK_MBC5 = 5 };

typedef struct {
    uint8_t  id16[16];       /* what autoselect reads at flash 0..15 */
    uint8_t  cfi[128];       /* what CFI mode reads at flash 0..127 */
    int      cfi_sst;        /* CFI entry = unlock + 98 at U1 (SST); else single 98 at 0xAA/0x55 */
    uint32_t size;
    int      aaa;            /* unlock 0xAAA/0x555 (else 0x555/0x2AA) */
    struct { uint32_t size; int count; } reg[4];
    int      nreg;
    uint8_t  erase_op;       /* erase of a reg[] unit */
    uint8_t  small_op;       /* 0 or a second erase op for small units (SST 4 KiB) */
    uint32_t small_size;
    double   prog_us, erase_ms, chip_erase_ms;
    int      swap;           /* the board swaps D0/D1 */
    int      we_gated;       /* writes never reach the chip */
    int      mapper;         /* CM_MBC5 / CM_HEADER */
    /* save RAM hypotheses after the 2026-10-10 run (first KiB of each bank stable, rest
       floating): only ram_kib KiB per 8 KiB bank are mapped (0 = all 8), or the RAM enable
       lapses ram_timeout_us after the last mapper write (0 = never). Unmapped / lapsed reads
       float (0xAD / 0xED with bit 6 varying, as seen), writes there are lost. */
    int      ram_kib;
    double   ram_timeout_us;
    /* the ModRetro truth (unit 1, 2026-10-10): GB-timed write cycles reach the mapper but not the
       flash; only the FPGA cart bus engine's static-pin cycles do (cart_write_engine). 1 = every
       write reaches the chip (a plain DIY flash cart). */
    int      gb_writes_flash;
    uint32_t protect_top;    /* WP# low: program / erase ignored in the top N bytes (SST39VF1682: 64 KiB) */
    uint32_t protect_bottom; /* WP# low: program / erase ignored in the bottom N bytes (SST39VF1681: 64 KiB; 0 = WP# high) */
    int      garble_ignored; /* fault: after a program the chip ignored, that byte reads back with bit 5 flipped (stable): neither value */
} cart_cfg_t;

typedef struct {
    cart_cfg_t cfg;
    uint8_t   *flash;
    uint8_t    ram[32768];
    /* mapper */
    int        mk;
    uint16_t   rom_bank;
    uint8_t    ram_bank, mbc1_hi, mbc1_mode;
    int        ram_en;
    /* flash */
    int        fs;
    int        busy, busy_kind, toggle;
    double     busy_end;
    uint32_t   busy_addr, busy_len;
    uint8_t    busy_data;
    double     now_us;
    double     last_mbc_us;
    unsigned   n_ram_float_reads;
    int        has_stuck;
    uint32_t   stuck_addr;
    uint8_t    stuck0;       /* bits stuck at 0 */
    uint32_t   rng;
    /* stats */
    unsigned   n_prog, n_erase, n_chip_erase, n_stray, n_busy_writes, n_autoselect, n_cfi, n_resets;
    unsigned   n_power_cuts, n_power_on;
    unsigned   n_gb_writes_lost, n_prot_ignored, n_prot_bottom_ignored;
    uint32_t   garble_at;    /* garble_ignored: the byte (+ 1) that reads wrong */
} cart_t;

void     cart_init(cart_t *c, const cart_cfg_t *cfg, const uint8_t *img, uint32_t n, const uint8_t *ram);
void     cart_free(cart_t *c);
void     cart_power_on(cart_t *c);
/* power is lost now: an operation in flight leaves indeterminate bytes */
void     cart_power_cut(cart_t *c);
void     cart_time(cart_t *c, double now_us);
uint8_t  cart_read(cart_t *c, uint16_t addr);
uint32_t cart_rom_offset(cart_t *c, uint16_t addr);   /* the flash offset a CPU read of addr (0000-7FFF) reaches now */
void     cart_write(cart_t *c, uint16_t addr, uint8_t v);
void     cart_write_engine(cart_t *c, uint16_t addr, uint8_t v);   /* the engine's cycle: reaches the chip */
int      cart_busy(const cart_t *c);
const char *cart_mk_name(int mk);

/* chip presets */
void     cart_cfg_sst39vf1681(cart_cfg_t *k);   /* BF C8, 2 MiB, 64 KiB blocks (0x30), 4 KiB sectors (0x50) */
void     cart_cfg_sst39vf1682(cart_cfg_t *k);   /* BF C9, unit 1's chip: + top 64 KiB WP#-protected */
void     cart_cfg_is29gl032(cart_cfg_t *k);     /* 9D 9D 7E 7E, 4 MiB, 64 KiB sectors, CFI */
void     cart_cfg_unknown(cart_cfg_t *k);       /* an AMD chip not in the table (01 22) */
#endif
