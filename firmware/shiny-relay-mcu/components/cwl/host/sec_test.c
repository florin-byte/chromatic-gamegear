// Mac test of cwl_sec.c (0.17.1): API URL rule and the phone-setup password.
//   make -C components/cwl/host test
#include "cwl_sec.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int _Fails, _Checks;
#define CHECK(c) do { _Checks++; if (!(c)) { _Fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint64_t _S = 0x9E3779B97F4A7C15ULL;
static uint32_t Rnd(void)   // xorshift64*
{
    _S ^= _S >> 12; _S ^= _S << 25; _S ^= _S >> 27;
    return (uint32_t)((_S * 0x2545F4914F6CDD1DULL) >> 32);
}
static uint32_t _Fixed[64];
static int _FixedN, _FixedAt;
static uint32_t RndFixed(void) { return _Fixed[_FixedAt++ % _FixedN]; }

static void TestUrls(void)
{
    // release (AllowHttp false)
    CHECK(CwlSec_UrlAllowed("https://api.shinyrelay.com", false));
    CHECK(CwlSec_UrlAllowed("https://beta.api.shinyrelay.com/gbnet/v1", false));
    CHECK(CwlSec_UrlAllowed("https://192.168.1.20:5050", false));
    CHECK(!CwlSec_UrlAllowed("http://192.168.1.20:5050", false));
    CHECK(!CwlSec_UrlAllowed("http://api.shinyrelay.com", false));
    CHECK(!CwlSec_UrlAllowed("HTTP://api.shinyrelay.com", false));
    CHECK(!CwlSec_UrlAllowed("HTTPS://api.shinyrelay.com", false));   // lower-case scheme only (strict)
    CHECK(!CwlSec_UrlAllowed("api.shinyrelay.com", false));
    CHECK(!CwlSec_UrlAllowed("https://", false));
    CHECK(!CwlSec_UrlAllowed("https:///x", false));
    CHECK(!CwlSec_UrlAllowed("https:api.shinyrelay.com", false));
    CHECK(!CwlSec_UrlAllowed(" https://api.shinyrelay.com", false));
    CHECK(!CwlSec_UrlAllowed("ftp://x", false));
    CHECK(!CwlSec_UrlAllowed("", false));
    CHECK(!CwlSec_UrlAllowed(NULL, false));
    // debug (AllowHttp true)
    CHECK(CwlSec_UrlAllowed("http://192.168.1.20:5050", true));
    CHECK(CwlSec_UrlAllowed("https://api.shinyrelay.com", true));
    CHECK(!CwlSec_UrlAllowed("http://", true));
    CHECK(!CwlSec_UrlAllowed("ftp://x", true));
    // this (host) build is a release build
    CHECK(!CwlSec_HttpAllowed());
    CHECK(CwlSec_ApiUrlOk("https://api.shinyrelay.com"));
    CHECK(!CwlSec_ApiUrlOk("http://api.shinyrelay.com"));
}

// The gateway base every release-build window call uses (0.17.2): the paired
// API host's /gbnet/v1 must be accepted in every form a unit can hold.
static void TestGateway(void)
{
    char b[160];
    CHECK(CwlSec_GatewayBase("", "https://api.shinyrelay.com", b, sizeof(b), false));
    CHECK(strcmp(b, "https://api.shinyrelay.com/gbnet/v1") == 0);
    CHECK(CwlSec_GatewayBase(NULL, "https://api.shinyrelay.com", b, sizeof(b), false));
    CHECK(strcmp(b, "https://api.shinyrelay.com/gbnet/v1") == 0);
    CHECK(CwlSec_GatewayBase("", "https://beta.api.shinyrelay.com", b, sizeof(b), false));
    CHECK(CwlSec_GatewayBase("", "https://192.168.1.20:5050", b, sizeof(b), false));
    CHECK(strcmp(b, "https://192.168.1.20:5050/gbnet/v1") == 0);
    CHECK(CwlSec_GatewayBase("https://api.shinyrelay.com/gbnet/v1", "https://x", b, sizeof(b), false));
    CHECK(strcmp(b, "https://api.shinyrelay.com/gbnet/v1") == 0);   // an override wins
    // what a release build refuses
    CHECK(!CwlSec_GatewayBase("http://192.168.1.20:5050/gbnet/v1", "https://api.shinyrelay.com", b, sizeof(b), false));
    CHECK(!CwlSec_GatewayBase("", "http://192.168.1.20:5050", b, sizeof(b), false));
    CHECK(!CwlSec_GatewayBase("", "", b, sizeof(b), false) && b[0] == '\0');
    // a debug build takes the LAN mock
    CHECK(CwlSec_GatewayBase("http://192.168.1.20:5050/gbnet/v1", "https://api.shinyrelay.com", b, sizeof(b), true));
    // never a truncated host
    char small[20];
    CHECK(!CwlSec_GatewayBase("", "https://api.shinyrelay.com", small, sizeof(small), false));
    CHECK(!CwlSec_GatewayBase("", "https://api.shinyrelay.com", b, 0, false));
    // the longest API the cwl_online settings hold (95 chars) still fits the 160-byte base
    char api[96];
    memset(api, 'a', sizeof(api) - 1);
    memcpy(api, "https://", 8);
    api[95] = '\0';
    CHECK(CwlSec_GatewayBase("", api, b, sizeof(b), false));
}

// 0.17.5: the relay follows the API stack (the beta invite asked of the prod relay)
static void TestRelayStack(void)
{
    char r[64];
    // the device report: beta API, the beta ticket naming the prod relay -> the beta relay
    CHECK(CwlSec_RelayFor("https://betaapi.shinyrelay.com", "relay.shinyrelay.com:5861", r, sizeof(r)));
    CHECK(strcmp(r, "betarelay.shinyrelay.com:6861") == 0);
    // prod: unchanged
    CHECK(!CwlSec_RelayFor("https://api.shinyrelay.com", "relay.shinyrelay.com:5861", r, sizeof(r)));
    CHECK(strcmp(r, "relay.shinyrelay.com:5861") == 0);
    // prod API, a ticket naming the beta relay -> prod relay
    CHECK(CwlSec_RelayFor("https://api.shinyrelay.com", "betarelay.shinyrelay.com:6861", r, sizeof(r)));
    CHECK(strcmp(r, "relay.shinyrelay.com:5861") == 0);
    // a ticket without a relay: the stack's own
    CHECK(CwlSec_RelayFor("https://betaapi.shinyrelay.com", "", r, sizeof(r)) && strcmp(r, "betarelay.shinyrelay.com:6861") == 0);
    CHECK(CwlSec_RelayFor("https://api.shinyrelay.com", NULL, r, sizeof(r)) && strcmp(r, "relay.shinyrelay.com:5861") == 0);
    // the right relay for the stack, or a new host the backend picked: kept
    CHECK(!CwlSec_RelayFor("https://betaapi.shinyrelay.com", "betarelay.shinyrelay.com:6861", r, sizeof(r)));
    CHECK(!CwlSec_RelayFor("https://api.shinyrelay.com", "relay2.shinyrelay.com:5861", r, sizeof(r)) && strcmp(r, "relay2.shinyrelay.com:5861") == 0);
    // URL forms: case, trailing slash, port, path
    CHECK(CwlSec_RelayFor("https://BetaAPI.ShinyRelay.com/", "relay.shinyrelay.com:5861", r, sizeof(r)) && strcmp(r, "betarelay.shinyrelay.com:6861") == 0);
    CHECK(CwlSec_RelayFor("https://betaapi.shinyrelay.com:443/x", "RELAY.shinyrelay.com:5861", r, sizeof(r)) && strcmp(r, "betarelay.shinyrelay.com:6861") == 0);
    // not a known stack (a LAN mock): the ticket's relay, whatever it is
    CHECK(!CwlSec_RelayFor("http://192.168.1.20:5050", "127.0.0.1:5861", r, sizeof(r)) && strcmp(r, "127.0.0.1:5861") == 0);
    CHECK(!CwlSec_RelayFor("http://192.168.1.20:5050", "relay.shinyrelay.com:5861", r, sizeof(r)));
    CHECK(!CwlSec_RelayFor("https://api.shinyrelay.com.evil.example", "relay.shinyrelay.com:5861", r, sizeof(r)));
    CHECK(!CwlSec_RelayFor("https://xapi.shinyrelay.com", "relay.shinyrelay.com:5861", r, sizeof(r)));
    CHECK(CwlSec_StackRelay("https://betaapi.shinyrelay.com") != NULL && CwlSec_StackRelay("https://x") == NULL);
    CHECK(CwlSec_StackRelay(NULL) == NULL);
}

static bool InAlpha(char c) { return c != '\0' && strchr(CWLSEC_SETUP_ALPHABET, c) != NULL; }

static void TestPass(void)
{
    CHECK(kCwlSec_SetupAlphabetLen == 18);
    CHECK(kCwlSec_SetupPassLen == 17);
    // the alphabet: no confusable pair in the OSD font, no 6 px glyph, no duplicates
    static const char *const kBanned = "0ODQ1IL2Z5S8BUV6YMW";
    for (const char *b = kBanned; *b; b++) {
        if (*b == 'L') continue;   // L is fine (no I/1 left to confuse it with)
        CHECK(strchr(CWLSEC_SETUP_ALPHABET, *b) == NULL);
    }
    for (int i = 0; i < kCwlSec_SetupAlphabetLen; i++)
        for (int j = i + 1; j < kCwlSec_SetupAlphabetLen; j++) CHECK(CWLSEC_SETUP_ALPHABET[i] != CWLSEC_SETUP_ALPHABET[j]);
    // entropy >= 60 bits
    CHECK(kCwlSec_SetupRandomChars * log2((double)kCwlSec_SetupAlphabetLen) >= 60.0);

    char p[32];
    // format
    for (int n = 0; n < 2000; n++) {
        memset(p, 'x', sizeof(p));
        CHECK(CwlSec_MakeSetupPass(p, sizeof(p), Rnd));
        CHECK(strlen(p) == 17);
        CHECK(p[5] == '-' && p[11] == '-');
        for (int i = 0; i < 17; i++) if (i != 5 && i != 11) CHECK(InAlpha(p[i]));
        // WPA2 PSK: 8..63 printable ASCII
        CHECK(strlen(p) >= 8 && strlen(p) <= 63);
    }
    // exact fit and too small
    char q[18];
    CHECK(CwlSec_MakeSetupPass(q, sizeof(q), Rnd) && strlen(q) == 17);
    char r[17] = "zzzz";
    CHECK(!CwlSec_MakeSetupPass(r, sizeof(r), Rnd) && r[0] == '\0');
    CHECK(!CwlSec_MakeSetupPass(p, sizeof(p), NULL) && p[0] == '\0');
    // rejection sampling: values >= 2^32 - (2^32 mod 18) are redrawn
    const uint32_t limit = (uint32_t)(0x100000000ULL - (0x100000000ULL % 18));
    _FixedN = 2; _FixedAt = 0;
    _Fixed[0] = 0xFFFFFFFFu;   // rejected
    _Fixed[1] = 1;              // 'C'
    CHECK(CwlSec_MakeSetupPass(p, sizeof(p), RndFixed));
    CHECK(strcmp(p, "CCCCC-CCCCC-CCCCC") == 0);
    _FixedN = 2; _FixedAt = 0;
    _Fixed[0] = limit;          // rejected (first rejected value)
    _Fixed[1] = limit - 1;      // accepted: (limit-1) % 18 = 17 -> '9'
    CHECK(CwlSec_MakeSetupPass(p, sizeof(p), RndFixed));
    CHECK(strcmp(p, "99999-99999-99999") == 0);
    // uniformity: chi-square over 18 symbols, 200k characters (df 17: 99.9% < 40.8)
    long cnt[18] = {0};
    for (int n = 0; n < 200000 / 15; n++) {
        (void)CwlSec_MakeSetupPass(p, sizeof(p), Rnd);
        for (int i = 0; i < 17; i++) if (p[i] != '-') cnt[strchr(CWLSEC_SETUP_ALPHABET, p[i]) - CWLSEC_SETUP_ALPHABET]++;
    }
    long tot = 0;
    for (int i = 0; i < 18; i++) tot += cnt[i];
    double chi = 0, e = (double)tot / 18;
    for (int i = 0; i < 18; i++) chi += (cnt[i] - e) * (cnt[i] - e) / e;
    printf("uniformity: %ld chars, chi2 = %.1f (df 17)\n", tot, chi);
    CHECK(chi < 40.8);
}

int main(void)
{
    TestUrls();
    TestGateway();
    TestRelayStack();
    TestPass();
    printf("sec_test: %d checks, %d failed\n", _Checks, _Fails);
    return _Fails ? 1 : 0;
}
