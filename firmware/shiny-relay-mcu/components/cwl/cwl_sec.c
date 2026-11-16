// See cwl_sec.h.
#include "cwl_sec.h"

#include <stdio.h>
#include <string.h>

#ifdef CWLSEC_HOST
#define CWLSEC_HTTP_DEFAULT false
#else
#include "sdkconfig.h"
#ifdef CONFIG_SHINY_RELAY_DEBUG_ALLOW_HTTP_API
#define CWLSEC_HTTP_DEFAULT true
#else
#define CWLSEC_HTTP_DEFAULT false
#endif
#endif

static bool HasHostAfter(const char *Url, size_t Prefix)
{
    const char c = Url[Prefix];
    return c != '\0' && c != '/' && c != ':' && c != '?' && c != '#' && c != ' ';
}

bool CwlSec_UrlAllowed(const char *Url, bool AllowHttp)
{
    if (Url == NULL) return false;
    if (strncmp(Url, "https://", 8) == 0) return HasHostAfter(Url, 8);
    if (AllowHttp && strncmp(Url, "http://", 7) == 0) return HasHostAfter(Url, 7);
    return false;
}

typedef struct { const char *ApiHost, *Relay; } Stack_t;
static const Stack_t kStacks[] = {
    {"api.shinyrelay.com", "relay.shinyrelay.com:5861"},
    {"betaapi.shinyrelay.com", "betarelay.shinyrelay.com:6861"},
};
enum { kNumStacks = sizeof(kStacks) / sizeof(kStacks[0]) };

static int LowerEq(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
        if (x == '\0') return 1;
    }
    return 1;
}

// The stack of an API URL: its host (between "://" and the first ':' '/' or
// end) matched case-insensitively. -1: not a known stack.
static int StackOfApi(const char *Api)
{
    if (Api == NULL) return -1;
    const char *h = strstr(Api, "://");
    h = h ? h + 3 : Api;
    size_t n = 0;
    while (h[n] && h[n] != ':' && h[n] != '/') n++;
    for (int i = 0; i < kNumStacks; i++)
        if (strlen(kStacks[i].ApiHost) == n && LowerEq(h, kStacks[i].ApiHost, n)) return i;
    return -1;
}

static int StackOfRelay(const char *Relay)
{
    if (Relay == NULL) return -1;
    for (int i = 0; i < kNumStacks; i++) {
        const size_t n = strlen(kStacks[i].Relay);
        if (strlen(Relay) == n && LowerEq(Relay, kStacks[i].Relay, n)) return i;
    }
    return -1;
}

const char *CwlSec_StackRelay(const char *Api)
{
    const int s = StackOfApi(Api);
    return s < 0 ? NULL : kStacks[s].Relay;
}

bool CwlSec_RelayFor(const char *Api, const char *TicketRelay, char *Out, size_t Cap)
{
    if (Out == NULL || Cap == 0) return false;
    const char *tr = TicketRelay ? TicketRelay : "";
    const int api = StackOfApi(Api);
    const char *use = tr;
    if (api >= 0) {
        const int rel = StackOfRelay(tr);
        if (tr[0] == '\0' || (rel >= 0 && rel != api)) use = kStacks[api].Relay;
    }
    snprintf(Out, Cap, "%s", use);
    return strcmp(use, tr) != 0;
}

bool CwlSec_GatewayBase(const char *Override, const char *Api, char *Out, size_t Cap, bool AllowHttp)
{
    if (Out == NULL || Cap == 0) return false;
    Out[0] = '\0';
    int n;
    if (Override != NULL && Override[0]) n = snprintf(Out, Cap, "%s", Override);
    else if (Api != NULL && Api[0]) n = snprintf(Out, Cap, "%s/gbnet/v1", Api);
    else return false;
    if (n < 0 || (size_t)n >= Cap) return false;   // cut short: never call a truncated host
    return CwlSec_UrlAllowed(Out, AllowHttp);
}

bool CwlSec_HttpAllowed(void) { return CWLSEC_HTTP_DEFAULT; }

bool CwlSec_ApiUrlOk(const char *Url) { return CwlSec_UrlAllowed(Url, CWLSEC_HTTP_DEFAULT); }

bool CwlSec_MakeSetupPass(char *Out, size_t Cap, uint32_t (*Rnd)(void))
{
    if (Out == NULL) return false;
    if (Cap < (size_t)kCwlSec_SetupPassLen + 1 || Rnd == NULL) {
        if (Cap > 0) Out[0] = '\0';
        return false;
    }
    static const char kAlpha[] = CWLSEC_SETUP_ALPHABET;
    // Largest multiple of the alphabet size below 2^32: values at or above it
    // are drawn again, so each character is exactly uniform.
    const uint32_t Limit = (uint32_t)(0x100000000ULL - (0x100000000ULL % kCwlSec_SetupAlphabetLen));
    int o = 0;
    for (int i = 0; i < kCwlSec_SetupRandomChars; i++) {
        if (i > 0 && i % kCwlSec_SetupGroupLen == 0) Out[o++] = '-';
        uint32_t r;
        do { r = Rnd(); } while (r >= Limit);
        Out[o++] = kAlpha[r % kCwlSec_SetupAlphabetLen];
    }
    Out[o] = '\0';
    return true;
}
