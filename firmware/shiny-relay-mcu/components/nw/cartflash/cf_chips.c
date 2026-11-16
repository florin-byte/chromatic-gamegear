/* cf_chips.c: flash chips known on ModRetro Chromatic cartridges (lane cartflash1). DRAFT.
 *
 * Facts only (part numbers, JEDEC IDs, command bytes, geometry); no code is taken from
 * any GPL tool. Sources (CART_FLASH_DESIGN.md, "Sources"):
 *  [CLI]  ModRetro @modretro/chromatic-cli 1.2.1: the five part numbers of its chip table
 *  [FGBX] FlashGBX config fc_DMG_39VF1681.txt, fc_DMG_iG_4MB.txt, fc_DMG_MX29LV320EB.txt
 *         (flash_ids, AMD command set at 0xAAA/0x555, write pin WR, sector maps)
 *  [PCB]  ergzay's board photo: SST39VF1681 + FM28V020 FRAM + GW1NZ-LV1
 *
 * verified = 1 only for an ID read from a real cart (SST39VF1682 BF C9, unit 1, 2026-10-10);
 * bottom_test = 1 (1.0.2: SST39VF1681) is allowed behind the bottom-block write test;
 * every other entry refuses writes unless the job runs with allow_unverified (simulation).
 */
#include "cf_esp.h"

const cf_chip_t cf_chips[] = {
    {   /* [CLI][FGBX][PCB] Tetris carts (Nerdly Pleasures: 2 MiB flash, Tetris uses 256 KiB) */
        .part = "SST39VF1681", .vendor = "Microchip/SST",
        .id = { 0xBF, 0xC8 }, .id_len = 2, .size = 0x200000,
        .cmdset = CF_CS_AMD_AAA,
        /* DS25040A (SST39VF1681/1682): Sector-Erase 50H = 4 KiB, Block-Erase 30H = 64 KiB
         * (the reverse of older SST parts). We erase 64 KiB blocks. */
        .erase_op = 0x30, .cfi_mode = 2, .cfi_addr = 0,
        .geometry_from_cfi = 0, .regions = { { 0x10000, 32 } },
        .protect_bottom = 0x10000,      /* DS25040A: the 1681's WP# protects the BOTTOM 64 KiB */
        /* 1.0.2 (owner 2026-10-13): allowed. Not yet seen on our own bench, but a customer's Tetris
           v1.4 reads BF C8. Whether its board holds WP# low is unknown (likely not: Cart Clinic
           rewrites Tetris on these carts), so every write job first tests that the bottom block
           takes a write, and refuses with CF_D_LOCKED (nothing changed) when it does not. */
        .bottom_test = 1,
        .erase_tmo = 400, .verified = 0,
        .source = "CLI table, FlashGBX fc_DMG_39VF1681 (BF C8), PCB photo",
    },
    {   /* [CLI][FGBX] e.g. Centipede (FlashGBX PR #129) */
        .part = "SST39VF1682", .vendor = "Microchip/SST",
        .id = { 0xBF, 0xC9 }, .id_len = 2, .size = 0x200000,
        .cmdset = CF_CS_AMD_AAA, .erase_op = 0x30, .cfi_mode = 2,
        .geometry_from_cfi = 0, .regions = { { 0x10000, 32 } },
        .protect_top = 0x10000,         /* DS25040A: the 1682's WP# protects the TOP 64 KiB */
        .erase_tmo = 400, .verified = 1,
        .source = "unit 1 (MR.TETRIS v1.4) read BF C9 on 2026-10-10 (Fred Emmott's GPL Chromatic "
                  "stack); DS25040A: x8 AAA/555, block erase 30H = 64 KiB, sector 50H = 4 KiB",
    },
    {   /* [CLI][FGBX] 4 MiB carts, DevDay demo cart (64 KiB sectors per kseth/modretro) */
        .part = "IS29GL032", .vendor = "ISSI",
        .id = { 0x9D, 0x9D, 0x7E, 0x7E }, .id_len = 4, .size = 0x400000,
        .cmdset = CF_CS_AMD_AAA, .erase_op = 0x30, .cfi_mode = 1, .cfi_addr = 0x00AA,
        .geometry_from_cfi = 1, .regions = { { 0x10000, 64 } },   /* checked against CFI */
        .erase_tmo = 400, .verified = 0,
        .source = "CLI table, FlashGBX fc_DMG_iG_4MB (9D 9D 7E 7E, sector erase SA:30, CFI geometry)",
    },
    {   /* [CLI][FGBX] FlashGBX lists it in fc_DMG_MX29LV320EB with 8 x 8 KiB + 63 x 64 KiB */
        .part = "S29JL032J", .vendor = "Infineon",
        .id = { 0x01, 0x01, 0x53, 0x53 }, .id_len = 4, .size = 0x400000,
        .cmdset = CF_CS_AMD_AAA, .erase_op = 0x30, .cfi_mode = 1, .cfi_addr = 0x00AA,
        .geometry_from_cfi = 1, .regions = { { 0x2000, 8 }, { 0x10000, 63 } },
        .erase_tmo = 400, .verified = 0,
        .source = "CLI table, FlashGBX fc_DMG_MX29LV320EB (01 01 53 53, ID unconfirmed for this part)",
    },
    {   /* [CLI] only: no ID known (no FlashGBX profile). Disabled until the probe reads one. */
        .part = "EN29LV320CB", .vendor = "ESMT/Eon",
        .id = { 0 }, .id_len = 0, .size = 0x400000,
        .cmdset = CF_CS_AMD_AAA, .erase_op = 0x30, .cfi_mode = 1, .cfi_addr = 0x00AA,
        .geometry_from_cfi = 1, .regions = { { 0x2000, 8 }, { 0x10000, 63 } },
        .erase_tmo = 400, .verified = 0,
        .source = "CLI table only (no ID known: entry never matches)",
    },
};
const int cf_n_chips = (int)(sizeof cf_chips / sizeof cf_chips[0]);
