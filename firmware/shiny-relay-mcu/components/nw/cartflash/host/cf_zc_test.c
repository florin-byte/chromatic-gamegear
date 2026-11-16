/* cf_zc_test.c (lane m110t): Mac test of ../cf_zc_guard.inc (the SAME code cf_dev.c builds into GET GAMES -> CARD's unpack
 * onto the card): the job gives up when the card stops taking writes. m110s, unit 2: CRC errors on every write, 84 KiB in
 * 619 s on GETTING IT / STARTING, the card READ ONLY, and only then the end. A model of the unpack's writes (2 KiB each, as
 * the log's "write at 86016 ... 0 of 2048"), each taking a given time on a given card. */
#include <stdio.h>
#include <string.h>

static int pass, fail;
#define CHECK(c, ...) do { if (c) pass++; else { fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#include "../cf_zc_guard.inc"

/* a game of `bytes` written 2 KiB at a time, each write ms_of(i) long, the card read only from write ro_at on (-1 never):
   the write the guard stopped at (-1: all of it), its reason, the time it took */
static int run(unsigned bytes, int (*ms_of)(int), int ro_at, int *why, long *took)
{
    zc_guard_t g = { 1000 };
    int64_t now = 1000;
    const int n = (int)(bytes / 2048u);
    for (int i = 0; i < n; i++) {
        const int64_t t = now;
        now += ms_of(i);
        const int k = zc_guard(&g, now, now - t, !(ro_at >= 0 && i >= ro_at));
        if (k) { *why = k; *took = (long)(now - 1000); return i; }
    }
    *why = ZC_OK; *took = (long)(now - 1000);
    return -1;
}
static int fast(int i) { (void)i; return 5; }                         /* a card that works: 2 KiB in 5 ms */
static int m110s_u2(int i) { (void)i; return 619418 / 42; }            /* 42 writes (86016 bytes) in 619 s */
static int one_hang(int i) { return i == 30 ? 25000 : 5; }
static int slowish(int i) { (void)i; return 60; }                      /* 2 KiB in 60 ms: 4 MiB in 123 s */
static int retrying(int i) { return i % 4 == 0 ? 1500 : 8; }          /* a write retried now and then (try 2 after 1.5 s) */

int main(void)
{
    int why = 0; long took = 0;
    CHECK(run(4u << 20, fast, -1, &why, &took) == -1 && why == ZC_OK, "a card that works: a 4 MiB game all of it (%ld ms)", took);
    CHECK(run(262144, retrying, -1, &why, &took) == -1 && why == ZC_OK, "retries now and then (the card's own try 2): goes on (%ld ms)", took);
    int at = run(262144, m110s_u2, -1, &why, &took);
    CHECK(at == 8 && why == ZC_ALL_SLOW && took < 2 * ZC_ALL_MS, "m110s unit 2 (14.7 s a write): given up at write %d, %s, after %ld s (not 619 s)", at, zc_why(why), took / 1000);
    at = run(262144, fast, 20, &why, &took);
    CHECK(at == 20 && why == ZC_NOT_WRITABLE && took < 1000, "the card goes READ ONLY: given up at once (write %d, %s)", at, zc_why(why));
    at = run(262144, one_hang, -1, &why, &took);
    CHECK(at == 30 && why == ZC_ONE_SLOW, "one write that hangs 25 s: given up there (%d, %s)", at, zc_why(why));
    at = run(4u << 20, slowish, -1, &why, &took);
    CHECK(at > 1900 && why == ZC_ALL_SLOW && took <= ZC_ALL_MS + 60, "a card too slow for 4 MiB in 2 minutes: given up at the budget (write %d, %ld ms)", at, took);
    printf("cf_zc: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
