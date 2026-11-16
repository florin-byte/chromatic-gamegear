/* cf_pad_test.c (lane m110t): Mac test of ../cf_rc_pad.inc (the SAME code cf_rc_dev.c builds into every play from the RAM
 * cart: the library, the card, PLAY NOW, a kept or staged game) and of the size rule the card and the RAM cart share
 * (../../../sdcard/sd_core.c sdc_rom_fit). The final install gate on bundle m110s: SHINY COLLECTION as the free games
 * catalog serves it (1,769,472 bytes for a 2 MiB header) could not go ON CHROMATIC (the header's parse said SIZE, the
 * screen NOT ENOUGH MEMORY). A model of the RAM cart's PSRAM ROM area (4 MiB, old bytes in it): the file is loaded the
 * ways the unit loads it (its last KiB FF padded, or the KiB's tail left as it was, as FCOPY / a flash load leave it), then
 * padded; the area the mapper reaches must equal the untrimmed game byte for byte.
 * argv: [trimmed file] [untrimmed file] (the catalog's collection.926b5cc6.gbc and gamecomp1's shinycoll2m.gbc). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../sdcard/sd_core.h"

static int pass, fail;
#define CHECK(c, ...) do { if (c) pass++; else { fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

#define AREA 0x400000u
static uint8_t _Ps[AREA];
static int _Writes, _Verifies, _Reads, _FailAt = -1;
static int pd_write(uint32_t off, const uint8_t *kib, int verify)
{
    if (off + 1024u > AREA) return -9;
    if (_FailAt >= 0 && _Writes == _FailAt) return -4;
    _Writes++; if (verify) _Verifies++;
    memcpy(_Ps + off, kib, 1024);
    return 0;
}
static int pd_read(uint32_t off, uint8_t *kib) { _Reads++; if (off + 1024u > AREA) return -9; memcpy(kib, _Ps + off, 1024); return 0; }
#include "../cf_rc_pad.inc"

static uint8_t *slurp(const char *p, uint32_t *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = (uint32_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n ? *n : 1);
    if (fread(b, 1, *n, f) != *n) { fclose(f); free(b); return NULL; }
    fclose(f);
    return b;
}
/* the file into the area as the unit loads it: whole KiBs, the last one FF padded (ff_tail) or its tail left as it was */
static void load(const uint8_t *g, uint32_t n, int ff_tail)
{
    for (uint32_t i = 0; i < AREA; i++) _Ps[i] = (uint8_t)(0xA5 ^ (i * 131u >> 7));   /* the last game's bytes */
    memcpy(_Ps, g, n);
    if (ff_tail && (n & 1023u)) memset(_Ps + n, 0xFF, 1024u - (n & 1023u));
    _Writes = _Verifies = _Reads = 0;
}
static uint16_t banks_of(const uint8_t *h, uint32_t n)
{
    sdc_hdr_t o;
    return sdc_hdr_parse(h, n, &o) == 0 ? o.rom_banks : 0;
}

int main(int argc, char **argv)
{
    uint8_t buf[1024];
    uint32_t kib = 0;
    /* a made-up 2 MiB MBC5 game, its last 336 KiB FF, served trimmed at a bank (as the catalog's collection) and mid-KiB */
    static uint8_t full[0x200000];
    for (uint32_t i = 0; i < sizeof full; i++) full[i] = i < 0x1B0000u ? (uint8_t)(i * 7u + (i >> 9)) : 0xFF;
    memset(full + 0x134, 0, 0x1C); memcpy(full + 0x134, "SHINYCOLL", 9);
    full[0x143] = 0xC0; full[0x147] = 0x1B; full[0x148] = 6; full[0x149] = 3;
    for (uint32_t i = 0x1AFFF5u; i < 0x1B0000u; i++) full[i] = 0xFF;          /* (its true last non-FF byte: as shinycoll2m) */

    CHECK(banks_of(full, 0x1B0000u) == 128, "the trimmed file's header plays: 128 banks (got %u)", banks_of(full, 0x1B0000u));
    load(full, 0x1B0000u, 1);
    CHECK(pd_rom_pad(0x1B0000u, 128, buf, &kib) == 0 && kib == 320 && _Reads == 0, "trimmed at a bank: 320 KiB filled, no read (kib %u, reads %d)", kib, _Reads);
    CHECK(!memcmp(_Ps, full, sizeof full), "trimmed at a bank: the ROM area = the untrimmed game");
    CHECK(_Verifies == 5, "the first filled KiB and every 64th read back (%d)", _Verifies);
    CHECK(_Ps[0x200000] != 0xFF || _Ps[0x200001] != 0xFF, "nothing written past the header's 2 MiB");

    load(full, 0x1AFFF5u, 0);                                                   /* (FCOPY / a flash load: the KiB's tail as it was) */
    CHECK(pd_rom_pad(0x1AFFF5u, 128, buf, &kib) == 0 && _Reads == 1 && kib == 321, "trimmed mid-KiB: that KiB read, mended, written (reads %d, kib %u)", _Reads, kib);
    CHECK(!memcmp(_Ps, full, sizeof full), "trimmed mid-KiB, the tail left as it was: the ROM area = the untrimmed game");
    load(full, 0x1AFFF5u, 1);
    CHECK(pd_rom_pad(0x1AFFF5u, 128, buf, &kib) == 0 && !memcmp(_Ps, full, sizeof full), "trimmed mid-KiB, the tail FF already: the same");

    load(full, sizeof full, 1);
    CHECK(pd_rom_pad(sizeof full, 128, buf, &kib) == 0 && kib == 0 && _Writes == 0, "a whole file: nothing to fill");
    load(full, sizeof full, 1);
    CHECK(pd_rom_pad(sizeof full, 64, buf, &kib) == 0 && kib == 0 && _Writes == 0, "a file longer than its header: nothing to fill (the mapper reaches the header's banks)");

    /* a tiny ROM only game of just its header: banks 0-1 read FF past it */
    static uint8_t tiny[0x8000];
    memset(tiny, 0xFF, sizeof tiny); memset(tiny, 0x11, 0x150); tiny[0x147] = 0; tiny[0x148] = 0;
    CHECK(banks_of(tiny, 0x150) == 2, "a 336-byte ROM only file: 2 banks");
    load(tiny, 0x150, 0);
    CHECK(pd_rom_pad(0x150, 2, buf, &kib) == 0 && kib == 32 && !memcmp(_Ps, tiny, sizeof tiny), "336 bytes: 32 KiB, = the 32 KiB game");

    /* a 4 MiB header over a 64 KiB file: the whole area, nothing past it */
    load(full, 0x10000u, 1);
    CHECK(pd_rom_pad(0x10000u, 256, buf, &kib) == 0 && kib == 4032, "a 4 MiB header over 64 KiB: 4032 KiB filled (%u)", kib);
    { int ff = 1; for (uint32_t i = 0x10000u; i < AREA; i++) if (_Ps[i] != 0xFF) { ff = 0; break; } CHECK(ff && !memcmp(_Ps, full, 0x10000u), "the 64 KiB as they were, the rest FF"); }

    /* a write that fails: said, not hidden */
    load(full, 0x1B0000u, 1);
    _FailAt = 7;
    CHECK(pd_rom_pad(0x1B0000u, 128, buf, &kib) == -4 && kib == 7, "a PSRAM write that fails stops the fill with its error (kib %u)", kib);
    _FailAt = -1;

    /* the real files: the catalog's SHINY COLLECTION and the untrimmed build */
    if (argc >= 3) {
        uint32_t tn = 0, fn = 0;
        uint8_t *t = slurp(argv[1], &tn), *f = slurp(argv[2], &fn);
        CHECK(t && f, "the files read (%s, %s)", argv[1], argv[2]);
        if (t && f) {
            const uint16_t b = banks_of(t, tn);
            CHECK(tn == 1769472u && b == 128, "%s: %u bytes, plays as %u banks", argv[1], tn, b);
            load(t, tn, 1);
            CHECK(pd_rom_pad(tn, b, buf, &kib) == 0 && fn == (uint32_t)b * 0x4000u && !memcmp(_Ps, f, fn), "the catalog's file padded = %s byte for byte (%u KiB filled)", argv[2], kib);
        }
        free(t); free(f);
    }
    printf("cf_pad: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
