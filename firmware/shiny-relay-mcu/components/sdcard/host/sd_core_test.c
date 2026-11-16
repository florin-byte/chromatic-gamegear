/* sd_core_test.c (lane sdcard1): the card library's portable rules on the Mac. make -C components/sdcard/host test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../sd_core.h"

static int fails, passes;
static void check(int ok, const char *what) { if (ok) passes++; else { fails++; printf("FAIL: %s\n", what); } }

static void hdr(uint8_t *h, const char *title, uint8_t cgb, uint8_t type, uint8_t rs, uint8_t ra)
{
    memset(h, 0, 0x150);
    memcpy(h + 0x134, title, strlen(title));
    h[0x143] = cgb; h[0x147] = type; h[0x148] = rs; h[0x149] = ra;
}

static void t_header(void)
{
    uint8_t h[0x150];
    sdc_hdr_t o;
    hdr(h, "TETRIS", 0, 0x00, 0, 0);
    check(sdc_hdr_parse(h, 0x8000, &o) == 0 && !strcmp(o.title, "TETRIS") && o.mbc == 0 && o.sav_size == 0, "a 32 KiB ROM only game plays, title TETRIS");
    hdr(h, "POKEMON RED", 0, 0x13, 5, 3);
    check(sdc_hdr_parse(h, 0x100000, &o) == 0 && o.mbc == 3 && o.battery && !o.rtc && o.sav_size == 0x8000 && o.rom_banks == 64, "MBC3+RAM+BATTERY 1 MiB: 32 KiB save");
    hdr(h, "POKEMON_CRYSTALX", 0xC0, 0x10, 6, 3);
    check(sdc_hdr_parse(h, 0x200000, &o) == 0 && o.mbc == 3 && o.rtc && o.battery && o.sav_size == 0x8000, "MBC3+TIMER+RAM+BATTERY: the clock");
    check(!strcmp(o.title, "POKEMON_CRYSTAL"), "a CGB-only header: 15 title bytes at most");
    hdr(h, "BIGGAME", 0x80, 0x1B, 7, 4);
    check(sdc_hdr_parse(h, 0x400000, &o) == 0 && o.mbc == 5 && o.rom_banks == 256 && o.sav_size == 0x20000, "MBC5 4 MiB with 128 KiB of save plays");
    hdr(h, "HUGE", 0x80, 0x1B, 8, 4);
    check(sdc_hdr_parse(h, 0x800000, &o) != 0 && !strcmp(o.why, "TOO  BIG"), "an 8 MiB game is too big for the RAM cart");
    hdr(h, "HUC", 0x80, 0xFF, 5, 3);
    check(sdc_hdr_parse(h, 0x100000, &o) != 0 && !strcmp(o.why, "MAPPER"), "HuC1 is not a mapper the RAM cart plays");
    hdr(h, "SHORT", 0x80, 0x19, 6, 0);
    check(sdc_hdr_parse(h, 0x100000, &o) == 0 && o.rom_banks == 128, "m110t: a file shorter than its header says (2 MiB in 1 MiB) plays: trimmed, the rest FF");
    hdr(h, "SHINY COLLECTION", 0x80, 0x1B, 6, 3);
    check(sdc_hdr_parse(h, 1769472u, &o) == 0 && o.rom_banks == 128 && o.sav_size == 0x8000, "m110t: SHINY COLLECTION as served (1,769,472 bytes, a 2 MiB header) plays");
    hdr(h, "TINY", 0, 0x00, 0, 0);
    check(sdc_hdr_parse(h, 0x150, &o) == 0 && o.rom_banks == 2, "m110t: a trimmed ROM only game of just its header plays (the rest FF)");
    check(sdc_hdr_parse(h, 0x14F, &o) != 0 && !strcmp(o.why, "SIZE"), "m110t: shorter than a header: SIZE");
    hdr(h, "LONG", 0x80, 0x19, 6, 0);
    check(sdc_hdr_parse(h, 0x400001, &o) != 0 && !strcmp(o.why, "TOO  BIG"), "m110t: a file over 4 MiB: TOO BIG (not SIZE)");
    {
        uint16_t rb = 9; const char *w = "";
        check(sdc_rom_fit(6, 1769472u, 0x400000u, &rb, &w) == 0 && rb == 128, "m110t: sdc_rom_fit: trimmed fits, the header's banks");
        check(sdc_rom_fit(8, 0x8000u, 0x400000u, &rb, &w) != 0 && !strcmp(w, "TOO  BIG") && rb == 0, "m110t: sdc_rom_fit: a header over 4 MiB");
        check(sdc_rom_fit(5, 0x100001u, 0x100000u, &rb, &w) != 0 && !strcmp(w, "TOO  BIG"), "m110t: sdc_rom_fit: longer than the place's max");
    }
    hdr(h, "MBC2", 0, 0x06, 1, 0);
    check(sdc_hdr_parse(h, 0x10000, &o) == 0 && o.sav_size == 0x200, "MBC2: 512 bytes of save");
    hdr(h, "lower case", 0, 0, 0, 0);
    check(sdc_hdr_parse(h, 0x8000, &o) == 0 && !strcmp(o.title, "LOWER CASE"), "titles upper case");
}

static void t_names(void)
{
    char b[128];
    check(sdc_is_game_name("Tetris.gb") && sdc_is_game_name("A.GBC") && sdc_is_game_name("x.Gb"), ".gb / .gbc any case");
    check(!sdc_is_game_name(".hidden.gb") && !sdc_is_game_name("a.gba") && !sdc_is_game_name("gb") && !sdc_is_game_name("a.gb.txt"), "not games: hidden, .gba, no extension, .txt");
    check(sdc_skip_dir("System Volume Information") && sdc_skip_dir(".Trashes") && sdc_skip_dir("SAVES") && sdc_skip_dir("shiny") && !sdc_skip_dir("GAMES") && !sdc_skip_dir("Pokemon"), "skipped folders");
    check(sdc_save_path("/GAMES/Pokemon Red (USA).gb", b, sizeof b) == 0 && !strcmp(b, "/SAVES/Pokemon Red (USA).sav"), "the save of /GAMES/Pokemon Red (USA).gb");
    check(sdc_save_path("/a/b/c.d.gbc", b, sizeof b) == 0 && !strcmp(b, "/SAVES/c.d.sav"), "only the last extension goes");
    check(sdc_save_path("/x.gb", b, 10) != 0, "a save path that does not fit is refused");
    sdc_display_name("TETRIS", "/GAMES/t.gb", b, 24);
    check(!strcmp(b, "TETRIS"), "a file name of fewer than 3 letters: the header title");
    sdc_display_name("POKEMON_RED", "/GAMES/Pokemon - Red Version (USA, Europe) [!].gb", b, 24);
    check(!strcmp(b, "POKEMON - RED VERSION"), "the file name the user chose, its region / dump tags dropped");
    sdc_display_name("X", "/GAMES/Shiny_Crosser.gbc", b, 24);
    check(!strcmp(b, "SHINY CROSSER"), "_ shown as a space");
    sdc_display_name("X", "/GAMES/The Legend of Zelda - Link's Awakening DX.gbc", b, 24);
    check(strlen(b) == 23 && !strncmp(b, "THE LEGEND OF ZELDA - L", 23), "a long name: 23 characters");
    sdc_display_name("X", "/GAMES/(USA).gb", b, 24);
    check(!strcmp(b, "(USA)"), "a name that is only a tag stays as it is");
    sdc_display_name("SHINY_CROSS", "/GAMES/x.gbc", b, 24);
    check(!strcmp(b, "SHINY CROSS"), "a header's _ shown as a space");
    sdc_display_name("", "/GAMES/\xc3\xa9t\xc3\xa9 Kart.gb", b, 24);
    check(!strcmp(b, "?T? KART"), "non-ASCII: one ? per character");
    check(sdc_clean_file("../../etc/Pok\xc3\xa9mon Red.gb", "", b, 64) == 0 && !strcmp(b, "Pok_mon Red.gb"), "a sent name: basename, a safe set");
    check(sdc_clean_file("game", ".gbc", b, 64) == 0 && !strcmp(b, "game.gbc"), "no extension: the one asked for");
    check(sdc_clean_file("Game.GBC", ".gbc", b, 64) == 0 && !strcmp(b, "Game.gbc"), "the extension lower case, not doubled");
    check(sdc_clean_file("..", "", b, 64) != 0 && sdc_clean_file("  ", ".gb", b, 64) != 0, "no usable name");
    char lng[200]; memset(lng, 'a', 150); lng[150] = 0; strcat(lng, ".sav");
    check(sdc_clean_file(lng, ".sav", b, 64) == 0 && strlen(b) == 60 && !strcmp(b + 56, ".sav"), "at most 60 characters, the extension kept");
    check(sdc_clean_file("a<b>:c|d?.gb", "", b, 64) == 0 && !strcmp(b, "a_b__c_d_.gb"), "characters FAT does not take become _");
}

/* the sort: a scrambled list with duplicate titles, small caps (many passes) */
static char T[3000][17];
static uint32_t out[3000], nout;
static int g(void *u, uint32_t i, char t[17]) { (void)u; memcpy(t, T[i], 17); return 0; }
static int e(void *u, uint32_t i) { (void)u; out[nout++] = i; return 0; }
static void t_sort(void)
{
    const uint32_t n = 3000;
    uint32_t s = 12345;
    for (uint32_t i = 0; i < n; i++) {
        s = s * 1103515245u + 12345u;
        const int len = 1 + (int)((s >> 16) % 12);
        for (int k = 0; k < len; k++) { s = s * 1103515245u + 12345u; T[i][k] = (char)('A' + (s >> 16) % 4); }   /* few letters: many equal titles */
        T[i][len] = 0;
    }
    static sdc_key_t w[256];
    for (uint32_t cap = 1; cap <= 256; cap = cap * 4 + 3) {
        nout = 0;
        uint32_t passes = 0;
        const int r = sdc_sort(n, g, e, NULL, w, cap, &passes);
        int ok = r == 0 && nout == n;
        static uint8_t seen[3000];
        memset(seen, 0, sizeof seen);
        for (uint32_t j = 0; ok && j < n; j++) { if (seen[out[j]]++) ok = 0; }
        for (uint32_t j = 1; ok && j < n; j++) {
            const int c = strcmp(T[out[j - 1]], T[out[j]]);
            if (c > 0 || (c == 0 && out[j - 1] > out[j])) ok = 0;
        }
        char what[96];
        snprintf(what, sizeof what, "sort of %u titles, %u at a time: every one once, in order (%u passes)", n, cap, passes);
        check(ok, what);
    }
    nout = 0;
    check(sdc_sort(0, g, e, NULL, w, 8, NULL) == 0 && nout == 0, "an empty library sorts");
}

static void t_rtc(void)
{
    uint8_t f[48], r[5] = { 59, 59, 23, 255, 0 }, q[5];
    uint64_t t;
    sdc_rtc_put(f, r, 1761000000ull);
    check(sdc_rtc_get(f, 48, q, &t) == 0 && !memcmp(q, r, 5) && t == 1761000000ull, "the 48-byte footer round trip");
    check(sdc_rtc_get(f, 44, q, &t) == 0 && t == 1761000000ull, "the 44-byte footer (32-bit time)");
    check(sdc_rtc_get(f, 40, q, &t) != 0, "40 bytes is not a footer");
    sdc_rtc_advance(r, 1);
    check(r[0] == 0 && r[1] == 0 && r[2] == 0 && r[3] == 0 && r[4] == 1, "23:59:59 day 255 + 1 s = day 256 (DH bit 0)");
    uint8_t z[5] = { 0, 0, 0, 255, 1 };
    sdc_rtc_advance(z, 86400);
    check(z[3] == 0 && (z[4] & 1) == 0 && (z[4] & 0x80), "day 511 + 1 day: the carry");
    uint8_t hlt[5] = { 1, 2, 3, 4, 0x40 };
    sdc_rtc_advance(hlt, 1000);
    check(hlt[0] == 1 && hlt[1] == 2, "a halted clock does not move");
}

static void t_rec(void)
{
    uint8_t h[0x150];
    sdc_hdr_t o;
    hdr(h, "ZELDA", 0x80, 0x1B, 5, 3);
    const int ok = sdc_hdr_parse(h, 0x100000, &o);
    sdc_rec_t r;
    sdc_rec_make(&r, "/GAMES/zelda.gbc", 0x100000, 0x12345678, &o, ok);
    check(!strcmp(r.title, "ZELDA") && (r.flags & SDC_F_PLAYABLE) && (r.flags & SDC_F_BATTERY) && r.size == 0x100000 && !strcmp(r.path, "/GAMES/zelda.gbc"), "an index record");
    const uint32_t a = sdc_fp_add(sdc_fp_start(), "/GAMES/zelda.gbc", 1, 2), b = sdc_fp_add(sdc_fp_start(), "/GAMES/zelda.gbc", 1, 3);
    check(a != b && a == sdc_fp_add(sdc_fp_start(), "/GAMES/zelda.gbc", 1, 2), "the fingerprint sees a changed date");
    check(sdc_crc32(0, "123456789", 9) == 0xCBF43926u, "crc32 check value");
    /* fr1: the folder order's keys sort by id first (as numbers), then by name, case not counted */
    char k1[17], k2[17], k3[17], k4[17];
    sdc_key_folder(k1, 5, "zelda"); sdc_key_folder(k2, 70, "Abc"); sdc_key_folder(k3, 70, "abd"); sdc_key_folder(k4, 4095, "");
    check(strncmp(k1, k2, 16) < 0 && strncmp(k2, k3, 16) < 0 && strncmp(k3, k4, 16) < 0 && strlen(k1) == 8, "folder keys: id, then name");
    char lng[SDC_NAME_LEN];
    sdc_display_name("", "/GAMES/Pokemon - Crystal Version - Special Edition Long Name.gbc", lng, sizeof lng);
    check(!strcmp(lng, "POKEMON - CRYSTAL VERSION - SPECIAL EDITION LONG NAME"), "fr1: a name longer than 23 characters is kept whole");
}

int main(void)
{
    t_header(); t_names(); t_sort(); t_rtc(); t_rec();
    printf("sd_core: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
