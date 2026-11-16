/* cf_zc_guard.inc (lane m110t): a game unpacked onto the card (GET GAMES -> CARD, an SGZ1 container staged, then written as
 * the plain game file) gives up when the card stops taking it, instead of sitting on GETTING IT / STARTING. m110s, unit 2:
 * the card's writes got CRC errors and retried, 84 KiB went in 619 s, the card went READ ONLY, and only then did a write
 * fail and the job end. Now each write is judged: the card no longer writable, one write over ZC_ONE_MS, or the whole
 * unpack over ZC_ALL_MS (a 4 MiB game takes ~10 s on a card that works) ends it, and the install says THE CARD DID NOT
 * TAKE IT. Built into cf_dev.c and into the Mac test host/cf_zc_test.c (the same code). */
#include <stdint.h>

enum { ZC_ONE_MS = 20000, ZC_ALL_MS = 120000 };
enum { ZC_OK = 0, ZC_NOT_WRITABLE = 1, ZC_ONE_SLOW = 2, ZC_ALL_SLOW = 3 };
typedef struct { int64_t t0_ms; } zc_guard_t;
/* after one write: now (ms), what that write took (ms), whether the card still takes writes. ZC_OK goes on */
static int zc_guard(const zc_guard_t *g, int64_t now_ms, int64_t took_ms, int writable)
{
    if (!writable) return ZC_NOT_WRITABLE;
    if (took_ms > ZC_ONE_MS) return ZC_ONE_SLOW;
    if (now_ms - g->t0_ms > ZC_ALL_MS) return ZC_ALL_SLOW;
    return ZC_OK;
}
static const char *zc_why(int k) { return k == ZC_NOT_WRITABLE ? "the card no longer takes writes" : k == ZC_ONE_SLOW ? "one write took too long" : k == ZC_ALL_SLOW ? "the card is too slow" : "ok"; }
