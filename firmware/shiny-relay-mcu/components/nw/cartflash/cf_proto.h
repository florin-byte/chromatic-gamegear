/* cf_proto.h: cart flash ("Send to my Chromatic") protocol constants, lane cartflash1.
 *
 * DRAFT. Nothing here is compiled into the ESP32 firmware unless NW_CARTFLASH is
 * defined (components/nw/CMakeLists.txt does not list cartflash/ by default).
 * Design: ${CWL_ROOT:?set_CWL_ROOT_to_your_work_folder}/docs/CART_FLASH_DESIGN.md.
 *
 * Two independent pieces use the network window (NETWORK_WINDOW.md):
 *
 * 1. The READ-ONLY cart probe (sm83/cartprobe_boot.inc), a TEST boot ROM. It
 *    needs no new ESP32 firmware: it uses the v1 ops IDENTIFY / GET / POST, so
 *    0.17.0 forwards everything to the gateway URL set with `nw url`, which for
 *    the probe must be the owner's Mac (host/cart_probe_rx.py). The probe refuses
 *    to read the cart unless GET cp/hello answers CP_HELLO_MAGIC, which only
 *    the local receiver serves, so a cart dump can never go to a Shiny Gen
 *    server by accident.
 *
 * 2. The cart writer (sm83/cf_writer.asm, WRAM-resident) and its ESP32 side
 *    (cf_esp.c). Two new ops in the 0x20 block, answered by the ESP32 itself
 *    (local ops, like the SHLINK ops 0x0B-0x0D: no IDENTIFY, no gateway, no rate
 *    limit). The SM83 side is a dumb executor; the ESP32 holds the plan,
 *    the chip table, the journal and every safety check.
 */
#ifndef CF_PROTO_H
#define CF_PROTO_H

/* ------------------------------------------------------------------ probe */
#define CP_GAME_ID       "SHGCARTPROBE0001"   /* 16 bytes, IDENTIFY */
#define CP_HELLO_MAGIC   "CARTPROBE-LOCAL1"   /* 16 bytes: GET cp/hello body, local receiver only */
#define CP_VERSION       1u
#define CP_CHUNK         1024u                /* bytes per POST cp/r */

/* POST cp/h  content: [ver, flags, header 0x100-0x14F (80)]
 *            answer : [go, mbc, rom_banks u16, ram_banks, ram_kb, opts, ext_banks u16]
 * POST cp/r  content: [kind, bank u16, off u16, len u16, sum u16, rereads, data...]
 * POST cp/x  content: [first u16, count, count x (sum u16, first 16 bytes)]
 * POST cp/i  content: [variant, 16 bytes at flash 0 in autoselect mode,
 *                      128 bytes at flash 0 in CFI mode, reset_ok]
 * POST cp/e  content: [result, chunks u16, rereads u16, bad u16, retries u16,
 *                      gsum u16, hsum, bank0]                                  */
#define CP_GO_STOP      0u
#define CP_GO_DUMP      1u
#define CP_OPT_EXTENDED 0x01u   /* fingerprint banks rom_banks..ext_banks-1 */
#define CP_OPT_FLASHID  0x02u   /* step 2 (owner approval): autoselect + CFI, needs a CP_FLASHID build */
#define CP_OPT_UNMAP    0x04u   /* non-MBC5: read bank 0's boot-ROM-shadowed ranges after FF50 (no normal boot after) */
#define CP_KIND_ROM     'R'
#define CP_KIND_RAM     'S'
#define CP_KIND_HOLE    'Z'
#define CP_MBC_NONE     0u
#define CP_MBC1         1u
#define CP_MBC3         3u
#define CP_MBC5         5u

/* ------------------------------------------------------------------ writer */
#define NW_OP_CF_LOAD  0x20u  /* body: off u16 -> body: writer image bytes, total_len = image size */
#define NW_OP_CF_NEXT  0x21u  /* body: seq, res, rlen u16, result... -> body: seq, cmd, args... */
#define NW_OP_CF_FIRST 0x20u
#define NW_OP_CF_LAST  0x21u

#define CF_WRAM_ORG    0xC000u   /* writer image load + entry address */
#define CF_WRAM_MAX    0x0E00u   /* C000-CDFF code; CE00-CEFF vars; CF00-CFFF stack */
#define CF_BUF         0xD000u   /* 4 KiB data buffer, D000-DFFF */
#define CF_BUF_SIZE    4096u
#define CF_WRITER_VER  1u

/* CF_NEXT request body: [seq, res, rlen u16, result[rlen]]
 *   seq = the seq of the command this result answers (0 + res HELLO at start)
 *   HELLO result: [writer_ver, 1, header 0x100-0x14F (80)]  */
#define CF_REQ_HDR 4u
#define CF_RESULT_MAX 2040u

/* results (res) */
#define CF_R_OK        0u
#define CF_R_HELLO     1u
#define CF_R_TIMEOUT   2u   /* flash busy past the command's timeout */
#define CF_R_DQ5       3u   /* chip reported an internal program/erase failure */
#define CF_R_MISMATCH  4u   /* PROGRAM / RAMWRITE / BLANK: rlen 6, [off u16, want, got, count u16] */
#define CF_R_BADCMD    5u
#define CF_R_RESET     6u   /* ID: the chip did not return to read mode (header reads differently) */
#define CF_R_UNSTABLE  7u   /* READ: never read the same twice in 4 tries */

/* CF_NEXT answer body: [seq, cmd, args[16], data...]; every command uses the same
 * argument layout (fields it does not need are 0):
 *   0 mbc  1-2 bank  3-4 addr  5-6 len  7 flags  8 cmdset  9 op  10-11 tmo
 *   12 cfi_mode  13-14 cfi_addr  15 0
 * Flash address F maps as bank = F >> 14, CPU addr = 0x4000 + (F & 0x3FFF) (MBC5).
 * A chunk never crosses a 16 KiB bank. */
#define CF_ANS_HDR  2u
#define CF_ANS_ARGS 16u
#define CF_ANS_DATA (CF_ANS_HDR + CF_ANS_ARGS)
#define CF_C_WAIT     0u  /* tmo = milliseconds; then report OK */
#define CF_C_READ     1u  /* -> data (each byte read twice and compared) */
#define CF_C_ERASE    2u  /* unlock 80 unlock, op at bank/addr; wait tmo x 16 ms */
#define CF_C_PROGRAM  3u  /* data[len]: byte-program every non-FF byte, then compare all */
#define CF_C_CRC      4u  /* -> crc16 CCITT (poly 1021, init FFFF) of len bytes */
#define CF_C_ID       5u  /* cmdset, cfi_mode (0 none, 1 = 98 at cfi_addr, 2 = unlock + 98) ->
                             16 autoselect bytes + 128 CFI bytes from flash 0 + reset_ok */
#define CF_C_RAMWRITE 6u  /* data[len] to cart RAM (flags RAM), compare */
#define CF_C_DONE     7u  /* op = DONE code: lock the window, halt; the ESP32 power-cycles the cart */
#define CF_C_BLANK    8u  /* every byte reads FF? OK : MISMATCH */
#define CF_C_GBTEST  10u  /* read-only experiment: RAM by engine / GB-timed, flash ID GB-timed at 1AAA/1555 -> 196 bytes */
#define CF_C_HELLO    9u  /* start over: send HELLO (the ESP32 lost the session) */
#define CF_ID_RESULT  145u
#define CF_PROG_CHUNK 2048u  /* PROGRAM data per answer (RX holds 4086 - 18) */
#define CF_READ_CHUNK 2032u  /* READ data per result (TX holds 2048 - 4 - 4) */
/* READ / CRC / BLANK flags */
#define CF_F_RAM      0x01u  /* cart RAM at A000-BFFF, bank = RAM bank: enable before, disable after */
#define CF_F_GBT      0x08u  /* the GB-timed route: commands at 1AAA/1555, RAM enable at 1000 (writer v4) */
#define CF_F_ENG      0x04u  /* READ of cart RAM through the FPGA engine (static-pin cycles; needs ENGINE_ALLOW) */
#define CF_F_UNMAP    0x02u  /* write FF50 first (boot ROM off; bank 0 at 0x0000 for non-MBC5) */

/* cmdset: how commands reach the chip (the chip table picks it) */
#define CF_CS_AMD_555  0u    /* AA->0x555, 55->0x2AA (x8 / byte-addressed x16 in x8 mode) */
#define CF_CS_AMD_AAA  1u    /* AA->0xAAA, 55->0x555 (x16 chip in byte mode, address-shifted) */
#define CF_CS_SWAP     0x80u /* | D0/D1 swapped on the board (commands written swapped) */

/* DONE codes (also the job result the OSD shows) */
#define CF_D_OK         0u
#define CF_D_NOJOB      1u   /* netboot with no job armed: nothing done */
#define CF_D_WRONG_CHIP 2u   /* unknown flash ID: refused before any erase */
#define CF_D_BAD_HEADER 3u   /* cart header not the cart the job is for */
#define CF_D_FAILED     4u   /* erase/program/verify failed after retries; backup kept */
#define CF_D_NO_BACKUP  5u   /* refused: no verified backup of the original */
#define CF_D_ABORTED    6u   /* user / power policy */
#define CF_D_BAD_IMAGE  7u   /* image refused by policy (mapper, size, header) */
#define CF_D_VERIFY     9u   /* STREAM: the cart read back does not have the image's crc32 / sha256; the cart stays without its first block */
#define CF_D_PAUSED    10u   /* STREAM: no data for too long (link down / the computer stopped): resume later */
#define CF_D_OVERLAY   12u   /* the FPGA's patch overlay is (or may be) serving the cart's bytes: no cart job runs under it */
#define CF_D_GONE      11u   /* STREAM: the source says the image no longer exists */
#define CF_D_NO_ENGINE  8u   /* the FPGA has no cart bus engine (or did not allow it): no flash op possible */
#define CF_D_WPTEST    14u   /* 1.0.2: the write test did not finish cleanly (its read-back was neither the old nor the new
                                 value, or the block did not read back): stopped before anything was erased; the journal
                                 idle; at most the one test bit was written. Not CF_D_FAILED (4), which means the cart is in flux */
#define CF_D_LOCKED    13u   /* 1.0.2: the chip's write-protected boot block (SST39VF1681: the BOTTOM 64 KiB, WP# low) did not take
                                 the one-bit test write before the first erase: refused, nothing erased, the cart as it was */
/* HELLO result byte 1 flags */
#define CF_HF_CGB    0x01u
#define CF_HF_ENGINE 0x02u   /* the cart bus engine answers at FF60 (fpga cartflash1-engine) */


/* ------------------------------------------------------------------ express stream (cartspeed1, FPGA 18.26)
 * fpga cart_xs.v. ESP32 -> FPGA UART address 0x12, FPGA -> ESP32 address 0x0C (status, 32 bytes). */
#define CF_XS_ADDR_TO_FPGA   0x12u
#define CF_XS_ADDR_FROM_FPGA 0x0Cu
#define CF_XS_RING           4096u
#define CF_XS_CHUNK          1024u     /* bytes per QSPI write + post */
#define CF_XS_QSPI_BIT       0x80000000u
#define CF_XS_P_OP     1u   /* [1, seq, op, F0 F1 F2, n0 n1 n2, arg] */
#define CF_XS_P_POST   2u   /* [2, end u32, len u16, crc32] */
#define CF_XS_P_ABORT  3u
#define CF_XS_P_STATUS 4u
#define CF_XS_P_RESET  5u
#define CF_XS_O_ERASE  1u
#define CF_XS_O_PROG   2u
#define CF_XS_O_CRC    3u
#define CF_XS_O_CFG    4u
/* status flags (byte 1) */
#define CF_XS_F_SESSION 0x01u
#define CF_XS_F_RUNNING 0x02u
#define CF_XS_F_ERROR   0x04u
#define CF_XS_F_ALLOW   0x08u
#define CF_XS_F_STARVED 0x10u
#define CF_XS_F_POSTNAK 0x20u
#define CF_XS_F_CHECK   0x40u
/* errors (byte 3) */
#define CF_XS_E_PTMO   1u
#define CF_XS_E_VERIFY 2u
#define CF_XS_E_ETMO   3u
#define CF_XS_E_STARVE 4u
#define CF_XS_E_QFULL  5u
#define CF_XS_E_BADOP  6u
#define CF_XS_E_NOSESS 7u
#define CF_XS_E_ABORT  8u
#define CF_XS_FPGA_MINOR 26u   /* the first FPGA minor version with it */
#define CF_XS_FPGA_MINOR_LAST 28u   /* 1.0.3: the last minor known to carry it (18.26-18.28, 18.28 = the 1.0.3 release image).
                                      A later FPGA keeps the classic route until a firmware names it here. */

#endif
