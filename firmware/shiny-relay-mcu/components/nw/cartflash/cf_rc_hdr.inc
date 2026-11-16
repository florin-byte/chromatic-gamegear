/* cf_rc_hdr.inc (lane m110s): the header of a game in PSRAM's ROM area (a share's PLAY NOW, a Wi-Fi Just play it), read
 * before it plays. A window read with the core held now and then comes a word late, the same way on both of its reads
 * (gamecomp1 10-22: ~5.6 KB per MiB wrong with the core held, none with it let go). m110f, unit 2: a share's PLAY NOW read a
 * header two bytes off (0x147 = the licensee code's '0' = CANNOT PLAY IT MAPPER) although every KiB had read back right as
 * it came. So: the core let go first (as scratch is read for an unpack), and the header read again while its own check byte
 * (0x14D, the one every Game Boy checks at power on) is wrong.
 *
 * Built into cf_rc_dev.c and into the Mac test host/cf_hdr_test.c (the same code). The includer gives:
 *   static int  hd_read(uint8_t *h, uint32_t n);   the first n bytes of the ROM area (0 = read)
 *   static void hd_let_go(void);                   the core let go (RAM cart config with no flags)
 *   static void hd_delay_ms(int ms);
 *   static void hd_say(const char *m); */
#include <stdint.h>
#include <stdio.h>

enum { HD_TRIES = 4 };

/* 1 = the header's check byte is right (0x134..0x14C, as the boot ROM sums them) */
static int hd_sum_ok(const uint8_t *h)
{
    uint8_t x = 0;
    for (int i = 0x134; i <= 0x14C; i++) x = (uint8_t)(x - h[i] - 1u);
    return x == h[0x14D];
}
/* 0 = h holds the header (checked, or as last read when it never checked: the header's parse decides, as before); else
   the read's error */
static int hd_ram_header(uint8_t *h, uint32_t n)
{
    char m[96];
    hd_let_go();
    int e = -1;
    for (int t = 0; t < HD_TRIES; t++) {
        e = hd_read(h, n);
        if (!e && hd_sum_ok(h)) {
            if (t) { snprintf(m, sizeof m, "the header checked on read %d", t + 1); hd_say(m); }
            return 0;
        }
        snprintf(m, sizeof m, "header read %d: %s (type %02x, check %02x)", t + 1, e ? "no answer" : "it does not check", h[0x147], h[0x14D]);
        hd_say(m);
        hd_delay_ms(20);
    }
    return e;
}
