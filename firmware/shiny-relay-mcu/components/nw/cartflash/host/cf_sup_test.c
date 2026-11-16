/* cf_sup_test.c: cf_sup.c (SAVE TO MY GAMES, lane savecloud1) against a mock flash and a mock server that applies the real
 * route's rules (the body must hash to scrc, saves full, ...). The decisions: what is read before anything is sent, that a
 * damaged or changed save never reaches the connection whole, every outcome and its words, the link-independent gate. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "../cf_sup.h"

uint32_t cf_crc32(uint32_t crc, const uint8_t *b, uint32_t n) { return (uint32_t)crc32(crc, b, n); }   /* zlib = the server's crc32 */

static int ok_n, bad_n;
static void check(int c, const char *what) { if (c) ok_n++; else { bad_n++; printf("  FAIL %s\n", what); } }

static uint8_t flash[0x20000 + 64];
static uint32_t fsize;
static int read_fail_at = -1, flip_after_check = 0, n_reads, n_open, open_heap_waits, open_fail, now_t, stop_at = -1;
static int connected, n_close, written_n, srv_code, srv_dup;
static const char *srv_tag = "";
static uint8_t wire[0x20000 + 64];
static uint32_t declared;
static char target[256];
static int stored_ok;   /* the server stored a save */

static int h_read(void *u, uint32_t off, uint8_t *b, uint32_t n)
{
    (void)u; n_reads++;
    if (read_fail_at >= 0 && (int)off >= read_fail_at) return -1;
    memcpy(b, flash + off, n);
    if (flip_after_check && n_reads > (int)((fsize + 1023) / 1024)) b[0] ^= 0x55;   /* the second pass reads different bytes */
    return 0;
}
static int h_open(void *u, const char *t, uint32_t len)
{
    (void)u; n_open++;
    if (open_heap_waits > 0) { open_heap_waits--; return 1; }
    if (open_fail) return -1;
    snprintf(target, sizeof target, "%s", t); declared = len; connected = 1; written_n = 0;
    return 0;
}
static int h_write(void *u, const uint8_t *b, uint32_t n) { (void)u; if (!connected) return -1; memcpy(wire + written_n, b, n); written_n += (int)n; return 0; }
static uint32_t qhex(const char *key)
{
    const char *p = strstr(target, key);
    return p ? (uint32_t)strtoul(p + strlen(key), NULL, 16) : 0;
}
static int h_answer(void *u, int *http, char *out, uint32_t cap)
{
    (void)u;
    /* the route: a short body or a crc that is not scrc stores nothing */
    if ((uint32_t)written_n != declared) { *http = 422; snprintf(out, cap, "{\"ok\":false}"); return 0; }
    if ((uint32_t)crc32(0, wire, (uInt)written_n) != qhex("scrc=")) { *http = 422; snprintf(out, cap, "{\"ok\":false}"); return 0; }
    *http = srv_code ? srv_code : 200;
    if (*http == 200) { stored_ok = 1; snprintf(out, cap, "{\"ok\":true,\"dup\":%s}", srv_dup ? "true" : "false"); }
    else snprintf(out, cap, "{\"ok\":false,\"code\":\"%s\"}", srv_tag);
    return 0;
}
static void h_close(void *u) { (void)u; if (connected) n_close++; connected = 0; }
static uint32_t h_now(void *u) { (void)u; return (uint32_t)(now_t += 100); }
static int h_stop(void *u) { (void)u; return stop_at >= 0 && n_reads >= stop_at; }
static const cf_sup_hooks_t H = { NULL, h_read, h_open, h_write, h_answer, h_close, h_now, h_stop };

static cf_sup_t S;
static void reset(uint32_t size)
{
    for (uint32_t i = 0; i < sizeof flash; i++) flash[i] = (uint8_t)(i * 31u + (i >> 7));
    fsize = size; read_fail_at = -1; flip_after_check = 0; n_reads = 0; n_open = 0; open_heap_waits = 0; open_fail = 0; now_t = 0; stop_at = -1;
    connected = 0; n_close = 0; written_n = 0; srv_code = 0; srv_dup = 0; srv_tag = ""; stored_ok = 0; target[0] = 0;
}
static int run(uint32_t want_crc, const char *title)
{
    cf_sup_begin(&S, &H, 0xDEADBEEFu, 0x100000u, fsize, want_crc, title);
    int steps = 0;
    while (cf_sup_step(&S) && ++steps < 100000) {}
    return S.res;
}
static uint32_t good_crc(void) { return (uint32_t)crc32(0, flash, fsize); }

int main(void)
{
    /* the request line: facts in the query, the title percent-encoded, nothing else */
    char t[200];
    check(!cf_sup_target(t, sizeof t, 0xAB12u, 524288, 0xCAFEF00Du, "POKEMON  CRYSTAL & CO:"), "target builds");
    check(!strcmp(t, "/link_device/my_games/save/upload?rcrc=0000ab12&rsz=524288&scrc=cafef00d&t=POKEMON%20%20CRYSTAL%20%26%20CO%3A"), t);
    check(cf_sup_target(t, 40, 1, 2, 3, "X") != 0, "a target that does not fit is refused");

    /* the good path: 32 KiB, every byte on the wire as stored, once, the connection let go */
    reset(32768);
    check(run(good_crc(), "SHINY  KART") == CF_SUP_OK, "ok");
    check(stored_ok && written_n == 32768 && !memcmp(wire, flash, 32768), "the server got the save byte for byte");
    check(declared == 32768 && strstr(target, "rsz=1048576") && strstr(target, "rcrc=deadbeef") && strstr(target, "t=SHINY%20%20KART"), "the facts it sent");
    check(n_reads == 64, "read twice (once to check, once to send): 2 x 32 pieces");
    check(n_close == 1 && !connected && S.pct == 100, "the connection let go, 100 percent");
    /* the largest save */
    reset(0x20000);
    check(run(good_crc(), "T") == CF_SUP_OK && written_n == 0x20000, "128 KiB goes");
    /* the largest save with a clock game's 48 bytes after it */
    reset(0x20000 + 48);
    check(run(good_crc(), "T") == CF_SUP_OK && written_n == 0x20000 + 48, "128 KiB and its clock go");
    /* a save that is not a whole number of pieces */
    reset(2048 + 700);
    check(run(good_crc(), "T") == CF_SUP_OK && written_n == 2748 && !memcmp(wire, flash, 2748), "an odd size goes whole");

    /* a stored save that no longer hashes as recorded: nothing is opened or sent */
    reset(32768);
    check(run(good_crc() ^ 1u, "T") == CF_SUP_E_READ, "damaged: E_READ");
    check(n_open == 0 && written_n == 0 && !stored_ok, "damaged: no connection was even opened");
    /* a read error in the check pass, in the send pass */
    reset(32768); read_fail_at = 16384;
    check(run(good_crc(), "T") == CF_SUP_E_READ && n_open == 0, "a read error before sending: nothing opened");
    reset(32768);
    cf_sup_begin(&S, &H, 1, 0x100000u, fsize, good_crc(), "T");
    while (S.st != CF_SUP_ST_SEND) (void)cf_sup_step(&S);
    read_fail_at = 20000;
    while (cf_sup_step(&S)) {}
    check(S.res == CF_SUP_E_READ && n_close == 1 && !stored_ok && (uint32_t)written_n < declared, "a read error mid-send: stops, the body is short, the server stores nothing");
    /* the bytes change between the check and the send */
    reset(32768); flip_after_check = 1;
    check(run(good_crc(), "T") == CF_SUP_E_READ && !stored_ok, "changed under the send: not stored (the connection ends short of a good save)");

    /* the heap does not come at once: asked again; then it does */
    reset(8192); open_heap_waits = 5;
    check(run(good_crc(), "T") == CF_SUP_OK && n_open == 6, "waits for the handshake's memory, then goes");
    reset(8192); open_heap_waits = 100000;
    check(run(good_crc(), "T") == CF_SUP_E_MEM && !stored_ok, "no memory in 25 s: E_MEM");
    reset(8192); open_fail = 1;
    check(run(good_crc(), "T") == CF_SUP_E_NET && n_open == 3, "three tries that cannot connect: E_NET");
    reset(8192); stop_at = 3;
    check(run(good_crc(), "T") == CF_SUP_E_STOP && n_close == 0 + (connected ? 0 : n_close) && !stored_ok, "asked to stop: E_STOP, nothing stored");

    /* the answers */
    struct { int code; const char *tag; int dup; int want; } A[] = {
        { 0, "", 0, CF_SUP_OK }, { 0, "", 1, CF_SUP_ALREADY }, { 401, "", 0, CF_SUP_E_AUTH }, { 409, "saves_full", 0, CF_SUP_E_FULL8 },
        { 409, "library_full", 0, CF_SUP_E_LIBFULL }, { 409, "changed", 0, CF_SUP_E_SERVER }, { 413, "", 0, CF_SUP_E_BIG }, { 429, "", 0, CF_SUP_E_RATE },
        { 503, "", 0, CF_SUP_E_SERVER }, { 404, "", 0, CF_SUP_E_OLD }, { 422, "", 0, CF_SUP_E_BAD }, { 500, "", 0, CF_SUP_E_SERVER } };
    for (unsigned i = 0; i < sizeof A / sizeof A[0]; i++) {
        reset(4096); srv_code = A[i].code; srv_tag = A[i].tag; srv_dup = A[i].dup;
        char w[64]; snprintf(w, sizeof w, "answer %d %s -> %d", A[i].code, A[i].tag, A[i].want);
        check(run(good_crc(), "T") == A[i].want, w);
    }
    check(cf_sup_outcome(200, NULL) == CF_SUP_OK, "no body on a 200 is still saved");

    /* the gate, before anything starts */
    check(cf_sup_gate(1, 1, 0, 1) == 0, "gate: go");
    check(cf_sup_gate(1, 1, 1, 1) == CF_SUP_E_PLAYING, "gate: a game is playing");
    check(cf_sup_gate(0, 1, 0, 1) == CF_SUP_E_PAIR, "gate: not paired");
    check(cf_sup_gate(1, 0, 0, 1) == CF_SUP_E_WIFI, "gate: no Wi-Fi saved");
    check(cf_sup_gate(1, 1, 0, 0) == CF_SUP_E_NOSAVE, "gate: no stored save");
    reset(0);
    check(run(0, "T") == CF_SUP_E_NOSAVE && n_open == 0, "a save of 0 bytes is no save");
    reset(0x20000); fsize = 0x20000u + 49u;
    check(run(0, "T") == CF_SUP_E_NOSAVE, "over 128 KiB and a clock is no save");

    /* the words: every outcome has them, each at most 47 characters (the card task's result), three rows at most, letters,
       digits, ".,!?'&:+-" and doubled spaces only, none of the words the player never sees, no em dash */
    static const char *const never[] = { "FPGA", "FLASH", "LIBRARY", "PSRAM", "STAGING", "SGZ", "COMPRESS", "KEPT  GAMES", "RAM  CART", "1.0.", "1.1." };
    for (int r = 0; r < CF_SUP_N; r++) {
        const char *w = cf_sup_words(r);
        char m[96]; snprintf(m, sizeof m, "words %d: |%s|", r, w);
        int rows = 1, bad = 0;
        for (const char *p = w; *p; p++) {
            if (*p == '|') rows++;
            else if (!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr(" .,!?'&:+->", *p))) bad = 1;
            if (*p == ' ' && p[1] != ' ' && (p == w || p[-1] != ' ') && p[1] && p[1] != '|') bad = 1;   /* a single space between words */
        }
        for (unsigned i = 0; i < sizeof never / sizeof never[0]; i++) if (strstr(w, never[i])) bad = 1;
        check(strlen(w) <= 47 && rows <= 3 && !bad && strstr(w, "\xE2\x80\x94") == NULL, m);
    }
    check(!strncmp(cf_sup_words(CF_SUP_OK), "SAVED|", 6), "SAVED says SAVED");
    for (int r = CF_SUP_E_NOSAVE; r < CF_SUP_N; r++) check(!strncmp(cf_sup_words(r), "NOT  SAVED|", 11), "every failure says NOT SAVED");

    printf("cf_sup_test: %d passed, %d failed\n", ok_n, bad_n);
    return bad_n ? 1 : 0;
}
