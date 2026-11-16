/* cf_fetch_test.c: cf_fetch.c against a fake gateway and a fake NOR region (lane cartflash1).
 * The region behaves like flash: a write can only clear bits, so a missing erase shows. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../cf_fetch.h"
#include "../cf_esp.h"

#define CAP (1024u * 1024u)
static uint8_t region[CAP], *srv;
static uint32_t srv_size, srv_total_lie;
static cf_fetch_jr_t jr; static int jr_valid;
static unsigned n_req, n_erase, n_write, n_sleep, n_jr;
static int fail_every, fail_from, fail_count, short_reads, refuse, die_at_req, flip_at = -1;
static int ok_n, bad_n;

static int f_range(void *u, const char *rel, uint32_t off, uint16_t max, const uint8_t **d, uint16_t *len, uint32_t *total)
{
    (void)u; (void)rel;
    static uint8_t out[2048];
    n_req++;
    if (refuse) return 2;
    if (die_at_req && (int)n_req >= die_at_req) return 1;                   /* the link is gone for good */
    if (fail_every && n_req % (unsigned)fail_every == 0) return 1;
    if (fail_from && (int)n_req >= fail_from && fail_count > 0) { fail_count--; return 1; }
    if (off >= srv_size) return 1;
    uint32_t k = srv_size - off < max ? srv_size - off : max;
    if (short_reads && k > 300) { const uint32_t s = 300 + n_req % 200; if (s < k) k = s; }   /* a server that answers less than asked */
    memcpy(out, srv + off, k);
    if (flip_at >= 0 && (uint32_t)flip_at >= off && (uint32_t)flip_at < off + k) out[(uint32_t)flip_at - off] ^= 0x04;
    *d = out; *len = (uint16_t)k; *total = srv_total_lie ? srv_total_lie : srv_size;
    return 0;
}
static int f_erase(void *u, uint32_t off, uint32_t n) { (void)u; if (off + n > CAP || off % 4096 || n % 4096) return -1; memset(region + off, 0xFF, n); n_erase++; return 0; }
static int f_write(void *u, uint32_t off, const uint8_t *b, uint32_t n) { (void)u; if (off + n > CAP) return -1; for (uint32_t i = 0; i < n; i++) region[off + i] &= b[i]; n_write++; return 0; }
static int f_read(void *u, uint32_t off, uint8_t *b, uint32_t n) { (void)u; if (off + n > CAP) return -1; memcpy(b, region + off, n); return 0; }
static int f_jl(void *u, void *j, uint32_t n) { (void)u; if (!jr_valid || n != sizeof jr) return -1; memcpy(j, &jr, n); return 0; }
static int f_js(void *u, const void *j, uint32_t n) { (void)u; if (n != sizeof jr) return -1; memcpy(&jr, j, n); jr_valid = 1; n_jr++; return 0; }
static void f_sleep(void *u, uint32_t ms) { (void)u; (void)ms; n_sleep++; }
static void f_log(void *u, const char *m) { (void)u; if (getenv("V")) printf("    %s\n", m); }
static const cf_fetch_host_t H = { NULL, CAP, f_range, f_erase, f_write, f_read, f_jl, f_js, f_sleep, f_log };

static void check(int c, const char *what) { if (c) ok_n++; else { bad_n++; printf("  FAIL %s\n", what); } }

static const uint8_t logo[48] = {
    0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
    0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E, 0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
    0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E };
static void make(uint32_t size, uint8_t type)
{
    free(srv);
    srv = malloc(size); srv_size = size;
    for (uint32_t i = 0; i < size; i++) srv[i] = (uint8_t)(i * 31 + (i >> 9) * 7 + 1);
    memcpy(srv + 0x104, logo, 48);
    memcpy(srv + 0x134, "FETCHTEST", 9); memset(srv + 0x13D, 0, 6);
    srv[0x143] = 0x80; srv[0x147] = type; srv[0x149] = 3;
    uint8_t sz = 0; while ((0x8000u << sz) < size) sz++;
    srv[0x148] = sz;
    uint8_t c = 0; for (int i = 0x134; i < 0x14D; i++) c = (uint8_t)(c - srv[i] - 1);
    srv[0x14D] = c;
}
static void reset(void)
{
    memset(region, 0x00, sizeof region);          /* old data everywhere: an erase is needed */
    jr_valid = 0; n_req = n_erase = n_write = n_sleep = n_jr = 0;
    fail_every = fail_from = fail_count = short_reads = refuse = die_at_req = 0; flip_at = -1; srv_total_lie = 0;
}

int main(void)
{
    uint32_t c;
    make(0x40000, 0x1B);                                                    /* 256 KiB MBC5+RAM+BATT */
    const uint32_t crc = cf_crc32(0, srv, srv_size);

    printf("clean\n"); reset();
    check(cf_fetch(&H, "cart/image/1", srv_size, crc, 5) == CF_FE_OK, "clean fetch OK");
    check(!memcmp(region, srv, srv_size) && n_erase == srv_size / 4096, "region == image; one erase per block");
    check(cf_fetch_staged(&H, &c) == srv_size && c == crc, "staged and verified");
    check(region[srv_size] == 0x00, "nothing past the image touched");
    n_req = 0;
    check(cf_fetch(&H, "cart/image/1", srv_size, crc, 5) == CF_FE_OK && n_req == 0, "the same image again: no download");

    printf("flaky link + short reads\n"); reset(); fail_every = 7; short_reads = 1;
    check(cf_fetch(&H, "cart/image/1", srv_size, crc, 5) == CF_FE_OK && !memcmp(region, srv, srv_size), "every 7th request fails, short answers: still the exact image");
    check(n_sleep > 0, "it backed off between tries");

    printf("link dies, later resume\n"); reset(); die_at_req = (int)(150u * 1024u / CF_FETCH_CHUNK);
    check(cf_fetch(&H, "cart/image/1", srv_size, crc, 3) == CF_FE_NET, "the link dies: NET after the retries");
    check(cf_fetch_staged(&H, NULL) == 0, "a partial image is never staged");
    check(jr_valid && jr.done == 0x20000 && !jr.ok, "journal at the last 64 KiB step");
    die_at_req = 0; n_req = 0;
    check(cf_fetch(&H, "cart/image/1", srv_size, crc, 3) == CF_FE_OK, "run again: OK");
    check(n_req <= (srv_size - 0x20000) / CF_FETCH_CHUNK + 2 && !memcmp(region, srv, srv_size), "resumed at 128 KiB, the exact image");

    printf("resume with a damaged prefix\n"); reset(); die_at_req = (int)(150u * 1024u / CF_FETCH_CHUNK);
    (void)cf_fetch(&H, "cart/image/1", srv_size, crc, 3);
    region[0x1234] ^= 0xFF; die_at_req = 0;
    check(cf_fetch(&H, "cart/image/1", srv_size, crc, 3) == CF_FE_CRC && cf_fetch_staged(&H, NULL) == 0, "a damaged prefix: CRC at the end, not staged");
    check(cf_fetch(&H, "cart/image/1", srv_size, crc, 3) == CF_FE_OK && !memcmp(region, srv, srv_size), "the next run starts from 0 and is right");

    printf("a different image asked after a partial one\n"); reset(); die_at_req = (int)(150u * 1024u / CF_FETCH_CHUNK);
    (void)cf_fetch(&H, "cart/image/1", srv_size, crc, 3);
    die_at_req = 0; n_req = 0;
    check(cf_fetch(&H, "cart/image/2", srv_size, crc, 3) == CF_FE_OK && n_req >= srv_size / CF_FETCH_CHUNK, "another path: from 0");

    printf("a corrupted byte in transit\n"); reset(); flip_at = 0x15555;
    check(cf_fetch(&H, "cart/image/1", srv_size, crc, 3) == CF_FE_CRC && cf_fetch_staged(&H, NULL) == 0, "bit flip: CRC, not staged");

    printf("server answers\n"); reset(); refuse = 1;
    check(cf_fetch(&H, "cart/image/1", srv_size, crc, 3) == CF_FE_GONE && n_req == 1 && n_erase <= 1, "refused: no retry");
    reset(); srv_total_lie = srv_size * 2;
    check(cf_fetch(&H, "cart/image/1", srv_size, crc, 3) == CF_FE_SIZE, "the server's size differs: stop");

    printf("arguments + policy\n"); reset();
    check(cf_fetch(&H, "cart/image/1", 0x4000, crc, 3) == CF_FE_ARG, "16 KiB: below a cart");
    check(cf_fetch(&H, "cart/image/1", CAP * 2, crc, 3) == CF_FE_ARG, "larger than the region");
    check(cf_fetch(&H, "", srv_size, crc, 3) == CF_FE_ARG, "empty path");
    /* any ROM (owner 2026-10-11): the device judges only the length and the checksums */
    make(0x40000, 0x13);                                                    /* MBC3 */
    reset();
    check(cf_fetch(&H, "cart/image/3", srv_size, cf_crc32(0, srv, srv_size), 3) == CF_FE_OK && cf_fetch_staged(&H, NULL) == srv_size, "an MBC3 image: staged");
    { uint8_t hd[80]; memcpy(hd, srv + 0x100, 80);
      check(cf_image_compat(hd) == CF_W_MAPPER, "and flagged: not MBC5");
      hd[0x47] = 0x10; check(cf_image_compat(hd) & CF_W_RTC, "type 10: a clock cartridge");
      hd[0x47] = 0x1B; hd[0x49] = 0x04; check(cf_image_compat(hd) & CF_W_RAM, "128 KiB of save RAM: more than the cart has");
      hd[0x49] = 0x03; check(cf_image_compat(hd) & CF_W_HEADER, "an edited header: the checksum is wrong");
      memcpy(hd, srv + 0x100, 80); hd[0x47] = 0x1B; hd[0x49] = 0x03;
      uint8_t x = 0; for (int i = 0x34; i <= 0x4C; i++) x = (uint8_t)(x - hd[i] - 1); hd[0x4D] = x;
      check(cf_image_compat(hd) == 0 && !cf_compat_text(0)[0], "MBC5 + 32 KiB + valid header: no warning");
      hd[0x47] = 0x00; x = 0; for (int i = 0x34; i <= 0x4C; i++) x = (uint8_t)(x - hd[i] - 1); hd[0x4D] = x;
      check(cf_image_compat(hd) == 0, "ROM only: no warning");
      char t[17];
      cf_title_from_header(hd, t); check(!strcmp(t, "FETCHTEST"), "title from the header");
      memcpy(hd + 0x34, "my_game v1.2\x01\x02\x80", 15); cf_title_from_header(hd, t); check(!strcmp(t, "MY GAME V1.2"), "sanitized: upper case, _ to space, control bytes dropped");
      memset(hd + 0x34, 0, 16); cf_title_from_header(hd, t); check(!strcmp(t, "UNKNOWN GAME"), "no title: UNKNOWN GAME");
      memset(hd + 0x34, 0xFF, 16); cf_title_from_header(hd, t); check(strlen(t) <= 15 && !strcmp(t, "UNKNOWN GAME"), "garbage: UNKNOWN GAME"); }
    for (uint32_t i = 0; i < srv_size; i++) srv[i] = (uint8_t)(i * 7 + 3);  /* no header at all */
    reset();
    check(cf_fetch(&H, "cart/image/5", srv_size, cf_crc32(0, srv, srv_size), 3) == CF_FE_OK, "an image with no valid header: staged (the question warns)");
    srv_size = 200003;                                                       /* not a power of two, not a block multiple */
    reset();
    check(cf_fetch(&H, "cart/image/6", srv_size, cf_crc32(0, srv, srv_size), 3) == CF_FE_OK && !memcmp(region, srv, srv_size) && cf_fetch_staged(&H, &c) == 200003, "200,003 bytes: staged exactly");
    make(CAP, 0x1B);                                                        /* 1 MiB: the whole region */
    reset();
    check(cf_fetch(&H, "cart/image/4", srv_size, cf_crc32(0, srv, srv_size), 3) == CF_FE_OK && !memcmp(region, srv, CAP), "1 MiB fills the region exactly");

    printf("cf_fetch_test: %d ok, %d failed\n", ok_n, bad_n);
    return bad_n ? 1 : 0;
}
