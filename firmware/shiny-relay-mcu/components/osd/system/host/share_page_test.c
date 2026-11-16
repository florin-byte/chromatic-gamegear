/* share_page_test.c (lane m110s): share.c's page side over a fake share session, on the Mac (the SAME share.c the unit
 * builds). m110f: a share's result nobody dismissed (a PLAY NOW: the game starts at once, the menu closes over it) stayed;
 * the next SYSTEM > DOWNLOAD showed the old result and B did not start a new receive until it was cleared. And a SHARE that
 * could not start (no memory for its task) did nothing at all: the page that asked is now told why. */
#include <stdio.h>
#include <string.h>
#include "osd_shared.h"
#include "../share.h"
#include "cf_share.h"
#include "cf_share_dev.h"

static int pass, fail;
#define CHECK(c, ...) do { if (c) pass++; else { fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* the fake session and the unit around it */
static uint32_t _Now = 5000;
static int _Vis = 1, _Active, _Closes, _StartRet;
static cf_share_view_t _V;
static fnOSDButtonCb_t _Cap;
uint32_t ShareHost_NowMs(void) { return _Now; }
bool ShareHost_Visible(void) { return _Vis != 0; }
void MenuMgr_SetCapture(fnOSDButtonCb_t f) { _Cap = f; }
int  CfShare_Active(void) { return _Active; }
void CfShare_View(cf_share_view_t *v) { *v = _V; }
void CfShare_Button(int b) { (void)b; }
void CfShare_Close(void) { _Closes++; _Active = 0; }
int  CfShare_SendStart(int s, uint32_t u, const char *p, const char *t) { (void)s; (void)u; (void)p; (void)t; if (!_StartRet) { _Active = 1; _V.kind = CF_SHV_A_CODE; } return _StartRet; }
int  CfShare_RecvStart(void) { if (!_StartRet) { _Active = 1; _V.kind = CF_SHV_B_START; } return _StartRet; }

static OSD_Result_t host_cap(const Button_t b, const ButtonState_t s, void *a) { (void)b; (void)s; (void)a; return kOSD_Result_Ok; }
/* the page drawn every 100 ms for ms (the menu shown or hidden); 1 = the share's rows were drawn the last time */
static int draws(int ms)
{
    ShareRows_t r;
    int shown = 0;
    for (int t = 0; t <= ms; t += 100) { shown = Share_Rows(&r); _Now += 100; }
    return shown;
}
static void result(int kind, int ok, int dest) { memset(&_V, 0, sizeof _V); _V.kind = kind; _V.ok = ok; _V.dest = dest; _Active = 1; }

int main(void)
{
    /* starting: why it did not start goes back to the page */
    _StartRet = -1; CHECK(Share_SendStart(kShareSrc_Card, 0, "/GAMES/MR.gbc", "MR.TETRIS", host_cap) == kShare_Busy && !_Active, "another share runs: busy");
    _StartRet = -2; CHECK(Share_SendStart(kShareSrc_Card, 0, "/GAMES/MR.gbc", "MR.TETRIS", host_cap) == kShare_NoMemory && _Cap == NULL, "no memory for its task: said, the buttons not taken");
    _StartRet = -2; CHECK(Share_RecvStart(NULL) == kShare_NoMemory, "a receive with no memory: said");
    _StartRet = 0;  CHECK(Share_SendStart(kShareSrc_Card, 0, "/GAMES/MR.gbc", "MR.TETRIS", host_cap) == kShare_Ok && _Cap != NULL && _Active, "started: its page takes the buttons");
    CHECK(draws(500), "the code page drawn");

    /* a result that comes while the page is up stays up (however long) until A: OK */
    _V.kind = CF_SHV_A_SEND; CHECK(draws(2000), "sending");
    result(CF_SHV_A_RESULT, 1, CF_SH_DEST_CARD); CHECK(draws(5000) && _Closes == 0, "a result on screen stays while the page is up");
    /* ... the page left with it unanswered, SYSTEM > DOWNLOAD opened: it is not shown again */
    Share_HostLeft(); _Now += 3000;
    CHECK(!draws(300) && _Closes == 1 && !_Active, "m110f: the old result is not on the next DOWNLOAD (closed %d)", _Closes);
    CHECK(_Cap == NULL, "the buttons are the page's own again (B: FROM A CHROMATIC receives)");

    /* a PLAY NOW that plays: the menu closed over the game at once; the next DOWNLOAD starts fresh, though it was never seen */
    _Closes = 0; Share_HostLeft();
    result(CF_SHV_B_RESULT, 1, CF_SH_DEST_PLAY); _Vis = 0; draws(3000); CHECK(_Closes == 0, "while the menu is hidden nothing is closed");
    _Vis = 1; CHECK(!draws(300) && _Closes == 1, "PLAY NOW's result gone when the menu opens again");

    /* a result that came while no page showed it (the menu closed during a share to the CARD): shown once */
    _Closes = 0;
    _V.kind = CF_SHV_B_RECV; _Active = 1; CHECK(draws(500), "receiving"); _Vis = 0; draws(2000);
    result(CF_SHV_B_RESULT, 1, CF_SH_DEST_CARD); draws(2000);
    _Vis = 1; CHECK(draws(400) && _Closes == 0, "a result not seen yet is shown when the menu opens");
    Share_HostLeft(); _Now += 2000; CHECK(draws(300) && _Closes == 0, "on screen less than a second: shown again");
    draws(1500); Share_HostLeft(); _Now += 2000; CHECK(!draws(300) && _Closes == 1, "then seen: not shown again");

    /* a failed share (NOT RECEIVED: IT DID NOT START), seen, the page not left: stays until A: OK */
    _Closes = 0; result(CF_SHV_B_RESULT, 0, CF_SH_DEST_PLAY); CHECK(draws(4000) && _Closes == 0, "a failed PLAY NOW stays on screen");
    _Vis = 0; draws(500); _Vis = 1; CHECK(!draws(300) && _Closes == 1, "the menu hidden and shown again counts as coming back");

    /* not drawn for a while (another page) counts as coming back, without Share_HostLeft */
    _Closes = 0; result(CF_SHV_A_RESULT, 0, 0); draws(2000); _Now += 3000; CHECK(draws(300) && _Closes == 0, "a 3 s gap (a busy moment) is not coming back");
    _Now += 9000; CHECK(!draws(300) && _Closes == 1, "a gap of 9 s in the draws (another page) counts as coming back");
    /* a share that still runs is always shown on coming back */
    _Closes = 0; _V.kind = CF_SHV_A_FIND; _Active = 1; Share_HostLeft(); _Now += 9000; CHECK(draws(300) && _Closes == 0, "a share that runs is shown again");

    printf("share_page: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
