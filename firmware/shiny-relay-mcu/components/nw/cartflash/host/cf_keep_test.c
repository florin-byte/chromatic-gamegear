/* cf_keep_test.c (lane m110x): Mac test of ../cf_rc_keep.inc (the SAME code cf_rc_dev.c builds into `lib imp`), the keep over
 * the USB cable while a game plays from memory (m110g1 finding 2: after a Wi-Fi Just play it the console answered -98 and the
 * page could only say busy). A model of the RAM cart task on a virtual clock:
 *   - a stop asked is taken by the task after TAKE_MS; a game playing then copies its save (SAVE_MS) and is gone;
 *   - a game still loading lets a stop go by (the task stops only a game that plays) and plays once its load ends;
 *   - a library op (the open `lib st` asked for) ends at a set time;
 *   - a game that never stops (the FPGA does not answer).
 * Each case: what kp_keep_ready() answers (0 = the keep goes on, -98 busy), how long it took, how many stops it asked. */
#include <stdio.h>
#include <string.h>

static int pass, fail;
#define CHECK(c, ...) do { if (c) pass++; else { fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

enum { M_NONE, M_LOADING, M_PLAYING, M_SAVING };
static struct {
    int now;                 /* ms */
    int st;                  /* M_* */
    int req;                 /* a stop asked, not yet taken */
    int req_at;              /* when it was asked */
    int take_ms, save_ms;    /* the task's pace */
    int load_end;            /* M_LOADING: when it plays */
    int save_end;            /* M_SAVING: when it is gone */
    int stuck;               /* the game never stops */
    int lib_end;             /* a library op runs until then (0 none) */
    int stops, says;
} S;
static void model(void)
{
    if (S.st == M_LOADING && S.now >= S.load_end) S.st = M_PLAYING;
    if (S.req && S.now - S.req_at >= S.take_ms) {
        S.req = 0;
        if (S.st == M_PLAYING && !S.stuck) { S.st = M_SAVING; S.save_end = S.now + S.save_ms; }   /* (a loading game: the stop is let go by) */
    }
    if (S.st == M_SAVING && S.now >= S.save_end) S.st = M_NONE;
    if (S.lib_end && S.now >= S.lib_end) S.lib_end = 0;
}
static int kp_playing(void) { return S.st != M_NONE; }
static int kp_loading(void) { return S.st == M_LOADING; }
static int kp_stop_pending(void) { return S.req; }
static void kp_stop(void) { S.stops++; S.req = 1; S.req_at = S.now; }
static int kp_lib_busy(void) { return S.lib_end != 0; }
static void kp_delay_ms(int ms) { for (int i = 0; i < ms; i++) { S.now++; model(); } }
static void kp_say(const char *m) { (void)m; S.says++; }
#include "cf_rc_keep.inc"

static void fresh(int st) { memset(&S, 0, sizeof S); S.st = st; S.take_ms = 100; S.save_ms = 800; }

int main(void)
{
    /* nothing plays: at once, no stop, no line */
    fresh(M_NONE);
    CHECK(kp_keep_ready() == 0 && S.now == 0 && S.stops == 0 && S.says == 0, "nothing plays: at once (%d ms, %d stops)", S.now, S.stops);

    /* a Just play it from the staging area plays: stopped (its save copied), then the keep goes on */
    fresh(M_PLAYING);
    CHECK(kp_keep_ready() == 0 && S.st == M_NONE && S.stops == 1 && S.says == 1, "playing: stopped once, then on (%d stops)", S.stops);
    CHECK(S.now >= 900 && S.now < 1100, "playing: about the task's pace and the save's copy (%d ms)", S.now);

    /* the open `lib st` asked for still runs after the stop: waited for */
    fresh(M_PLAYING); S.lib_end = 3000;
    CHECK(kp_keep_ready() == 0 && S.now >= 3000 && S.lib_end == 0, "the library's open waited for (%d ms)", S.now);

    /* a library op that never ends: busy after 30 s */
    fresh(M_NONE); S.lib_end = 1 << 30;
    CHECK(kp_keep_ready() == -98 && S.now >= 30000 && S.now < 30200, "a library op that never ends: busy (%d ms)", S.now);

    /* a stop already asked (STOP PLAYING pressed a moment before): not asked twice, waited for */
    fresh(M_PLAYING); kp_stop(); S.stops = 0;
    CHECK(kp_keep_ready() == 0 && S.stops == 0 && S.st == M_NONE, "a stop already asked: waited for, not asked again (%d stops)", S.stops);

    /* a game still loading: the task lets the first stop go by; asked again once it plays, then stopped */
    fresh(M_LOADING); S.load_end = 1500;
    CHECK(kp_keep_ready() == 0 && S.st == M_NONE && S.stops == 2 && S.now >= 1500, "loading: asked again once it plays (%d stops, %d ms)", S.stops, S.now);

    /* a slow save (a 128 KB save to the card): still within the wait */
    fresh(M_PLAYING); S.save_ms = 12000;
    CHECK(kp_keep_ready() == 0 && S.st == M_NONE, "a slow save: waited for (%d ms)", S.now);

    /* a game that never stops: busy (-98) after 30 s, no keep */
    fresh(M_PLAYING); S.stuck = 1;
    CHECK(kp_keep_ready() == -98 && S.now >= 30000 && S.now < 30200, "never stops: busy after 30 s (%d ms)", S.now);
    CHECK(S.stops == 1, "never stops: one stop asked, not one every step (%d)", S.stops);

    fprintf(stderr, "cf_keep_test: %d passed, %d failed\n", pass, fail);
    printf("cf_keep_test: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
