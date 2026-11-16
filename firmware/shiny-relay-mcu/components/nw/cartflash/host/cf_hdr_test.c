/* cf_hdr_test.c (lane m110s): Mac test of ../cf_rc_hdr.inc (the SAME code cf_rc_dev.c builds into the play of a game in
 * PSRAM), the header read before a share's PLAY NOW / a Wi-Fi Just play it plays. m110f, unit 2: every KiB of MR.TETRIS read
 * back right as it came, then the play read a header a word late (0x147 = '0') and said CANNOT PLAY IT MAPPER. A model of
 * the window: each read comes right, or a word late (every byte two places on, as gamecomp1 saw with the core held), or not
 * at all. argv: ROM files whose headers are checked as the unit reads them (the catalog's games: none may need a re-read). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pass, fail;
#define CHECK(c, ...) do { if (c) pass++; else { fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static uint8_t _Rom[0x150];
static const char *_Plan;            /* one letter per read: r right, l a word late, x no answer (then right) */
static int _Reads, _LetGo, _Says, _Delay;
static int hd_read(uint8_t *h, uint32_t n)
{
    const char k = _Plan[_Reads] ? _Plan[_Reads] : 'r';
    _Reads++;
    if (k == 'x') return -2;
    if (k == 'l') { memset(h, 0, n); memcpy(h + 2, _Rom, n - 2); return 0; }
    memcpy(h, _Rom, n);
    return 0;
}
static void hd_let_go(void) { _LetGo++; }
static void hd_delay_ms(int ms) { _Delay += ms; }
static void hd_say(const char *m) { _Says++; printf("  rc: play: %s\n", m); }
#include "../cf_rc_hdr.inc"

static int run(const char *plan, uint8_t *h)
{
    _Plan = plan; _Reads = _LetGo = _Says = _Delay = 0;
    memset(h, 0xEE, 0x150);
    return hd_ram_header(h, 0x150);
}
int main(int argc, char **argv)
{
    /* MR.TETRIS's header as the unit has it (m110f's game): MBC5 + RAM + battery, 256 KB, 32 KB of RAM */
    static const uint8_t kHdr[] = { 'M','R','.','T','E','T','R','I','S',0,0,0,0,0,0,0xC0, '0','0', 0x00, 0x1B, 0x03, 0x03, 0x00, 0x33, 0x01 };
    memset(_Rom, 0, sizeof _Rom);
    memcpy(_Rom + 0x134, kHdr, sizeof kHdr);
    { uint8_t x = 0; for (int i = 0x134; i <= 0x14C; i++) x = (uint8_t)(x - _Rom[i] - 1u); _Rom[0x14D] = x; }
    uint8_t h[0x150];
    CHECK(run("r", h) == 0 && _Reads == 1 && _LetGo == 1 && !memcmp(h, _Rom, sizeof h) && _Says == 0, "a right read: once, the core let go first (reads %d)", _Reads);
    CHECK(run("lr", h) == 0 && _Reads == 2 && h[0x147] == 0x1B && h[0x149] == 0x03, "a word late, then right: read again (reads %d, type %02x)", _Reads, h[0x147]);
    {
        uint8_t l[0x150]; memset(l, 0, sizeof l); memcpy(l + 2, _Rom, sizeof l - 2);
        CHECK(l[0x147] == '0' && !hd_sum_ok(l), "the word-late header is m110f's (type %02x = the licensee's '0': MAPPER) and its check byte says so", l[0x147]);
    }
    CHECK(run("lllr", h) == 0 && _Reads == 4 && h[0x147] == 0x1B, "late three times, right on the fourth (reads %d)", _Reads);
    CHECK(run("llll", h) == 0 && _Reads == 4 && h[0x147] == '0', "never right: the last read is given (the header's parse decides, as before; reads %d)", _Reads);
    CHECK(run("xr", h) == 0 && _Reads == 2 && h[0x147] == 0x1B, "no answer, then right");
    CHECK(run("xxxx", h) != 0 && _Reads == 4, "no answer at all: an error (CANNOT READ THE GAME)");
    CHECK(_Delay <= 4 * 20, "at most 80 ms of waits (%d)", _Delay);
    /* the catalog's games: their check bytes are right, so a right read is never read again */
    int roms = 0;
    for (int a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        if (!f) { CHECK(0, "cannot open %s", argv[a]); continue; }
        const size_t n = fread(_Rom, 1, sizeof _Rom, f);
        fclose(f);
        if (n < sizeof _Rom) { CHECK(0, "%s: short", argv[a]); continue; }
        CHECK(run("r", h) == 0 && _Reads == 1, "%s: its header checks (reads %d)", argv[a], _Reads);
        roms++;
    }
    printf("cf_hdr: %d passed, %d failed (%d ROM headers)\n", pass, fail, roms);
    return fail ? 1 : 0;
}
