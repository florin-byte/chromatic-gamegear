/* cf_rc_pad.inc (lane m110t): a game file SHORTER than its header's ROM size (trimmed: its trailing FF bytes cut off, as
 * the free games catalog serves SHINY COLLECTION: 1,769,472 bytes for a 2 MiB header) plays: the RAM cart's ROM area past
 * the file, up to the header's size (the banks the mapper reaches: rom_banks x 16 KiB), is filled with 0xFF before the core
 * starts, so every bank the game can select reads what its untrimmed file holds there (a cart's erased flash reads FF, and
 * the emulators pad a short ROM with FF the same way). Before m110t the header's parse refused such a file (SIZE), and every
 * place said so its own wrong way (ON CHROMATIC: NOT ENOUGH MEMORY).
 *
 * A file that ends inside a KiB: that KiB is read back (the includer reads with the core let go), its tail set to FF and
 * written again; the KiBs after it are written from one FF KiB, the first and every 64th read back (as a card load does).
 *
 * Built into cf_rc_dev.c and into the Mac test host/cf_pad_test.c (the same code). The includer gives:
 *   static int pd_write(uint32_t off, const uint8_t *kib, int verify);   1 KiB at the ROM area's offset off (0 = written)
 *   static int pd_read(uint32_t off, uint8_t *kib);                      1 KiB from the ROM area's offset off (0 = read)
 * Offsets are from the ROM area's start (CF_RC_ROM_BASE on the unit). */
#include <stdint.h>
#include <string.h>

/* 0 = the ROM area reads FF from `have` up to rom_banks x 16 KiB (nothing to do when the file is that long); else the
   write's / read's error. buf: 1 KiB the includer owns (DMA-capable on the unit). *kib = KiBs written */
static int pd_rom_pad(uint32_t have, uint16_t rom_banks, uint8_t *buf, uint32_t *kib)
{
    const uint32_t end = (uint32_t)rom_banks * 0x4000u;
    *kib = 0;
    if (have >= end) return 0;
    uint32_t a = have & ~1023u;
    if (have & 1023u) {
        int e = pd_read(a, buf);
        if (e) return e;
        memset(buf + (have & 1023u), 0xFF, 1024u - (have & 1023u));
        e = pd_write(a, buf, 1);
        if (e) return e;
        a += 1024u; (*kib)++;
    }
    const uint32_t first = a;
    memset(buf, 0xFF, 1024);
    for (; a < end; a += 1024u) {
        const int e = pd_write(a, buf, a == first || ((a >> 10) & 63u) == 0);
        if (e) return e;
        (*kib)++;
    }
    return 0;
}
