// Mac test of cwl_portal.c (0.17.2): the setup network's DNS answers.
#include "cwl_portal.h"

#include <stdio.h>
#include <string.h>

static int _Fails, _Checks;
#define CHECK(c) do { _Checks++; if (!(c)) { _Fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const uint8_t kIp[4] = {192, 168, 4, 1};

// A query for Name / Type, optionally with an EDNS0 OPT record (as Android's resolver sends).
static int Query(uint8_t *b, const char *Name, uint16_t Type, int Edns)
{
    int n = 0;
    const uint8_t h[12] = {0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, (uint8_t)(Edns ? 1 : 0)};
    memcpy(b, h, 12);
    n = 12;
    const char *p = Name;
    while (*p) {
        const char *d = strchr(p, '.');
        const int l = d ? (int)(d - p) : (int)strlen(p);
        b[n++] = (uint8_t)l;
        memcpy(b + n, p, (size_t)l);
        n += l;
        p += l + (d ? 1 : 0);
    }
    b[n++] = 0;
    b[n++] = (uint8_t)(Type >> 8); b[n++] = (uint8_t)Type;
    b[n++] = 0; b[n++] = 1;
    if (Edns) {
        const uint8_t opt[11] = {0, 0, 41, 0x10, 0x00, 0, 0, 0, 0, 0, 0};
        memcpy(b + n, opt, 11);
        n += 11;
    }
    return n;
}

int main(void)
{
    uint8_t b[300];
    // A: one answer, header right, no additional records even with EDNS
    for (int edns = 0; edns < 2; edns++) {
        int n = Query(b, "captive.apple.com", 1, edns);
        const int qend = n - (edns ? 11 : 0);
        const int r = CwlPortal_DnsReply(b, n, sizeof(b), kIp);
        CHECK(r == qend + 16);
        CHECK(b[0] == 0x12 && b[1] == 0x34);
        CHECK(b[2] & 0x80);                       // response
        CHECK(b[2] & 0x01);                       // RD kept
        CHECK((b[3] & 0x0F) == 0);                // NOERROR
        CHECK(b[6] == 0 && b[7] == 1);            // ANCOUNT 1
        CHECK(b[10] == 0 && b[11] == 0);          // ARCOUNT 0 (the OPT record is not echoed)
        CHECK(b[qend] == 0xC0 && b[qend + 1] == 0x0C);
        CHECK(b[qend + 3] == 1);                  // type A
        CHECK(memcmp(b + qend + 12, kIp, 4) == 0);
    }
    // AAAA, HTTPS (65), connectivitycheck: NODATA at once
    const uint16_t types[] = {28, 65, 16, 255};
    for (int i = 0; i < 4; i++) {
        int n = Query(b, "connectivitycheck.gstatic.com", types[i], 1);
        const int qend = n - 11;
        const int r = CwlPortal_DnsReply(b, n, sizeof(b), kIp);
        CHECK(r == qend);
        CHECK((b[3] & 0x0F) == 0 && b[7] == 0 && b[11] == 0);
    }
    // silent: responses, two questions, truncated names, short packets, compression pointers
    int n = Query(b, "www.msftconnecttest.com", 1, 0);
    b[2] |= 0x80;
    CHECK(CwlPortal_DnsReply(b, n, sizeof(b), kIp) == -1);
    n = Query(b, "x.com", 1, 0); b[5] = 2;
    CHECK(CwlPortal_DnsReply(b, n, sizeof(b), kIp) == -1);
    n = Query(b, "x.com", 1, 0);
    CHECK(CwlPortal_DnsReply(b, n - 3, sizeof(b), kIp) == -1);
    CHECK(CwlPortal_DnsReply(b, 11, sizeof(b), kIp) == -1);
    n = Query(b, "x.com", 1, 0); b[12] = 0xC0;
    CHECK(CwlPortal_DnsReply(b, n, sizeof(b), kIp) == -1);
    n = Query(b, "x.com", 1, 0); b[2] = 0x28;   // opcode 5 (update)
    CHECK(CwlPortal_DnsReply(b, n, sizeof(b), kIp) == -1);
    // no room for the answer: silent, never past Cap
    n = Query(b, "x.com", 1, 0);
    CHECK(CwlPortal_DnsReply(b, n, n + 15, kIp) == -1);
    CHECK(CwlPortal_DnsReply(b, n, n + 16, kIp) == n + 16);
    // a length byte running past the packet
    n = Query(b, "abc.com", 1, 0); b[12] = 60;
    CHECK(CwlPortal_DnsReply(b, n, sizeof(b), kIp) == -1);
    // random garbage never crashes or overruns
    unsigned seed = 1;
    for (int k = 0; k < 200000; k++) {
        const int len = (int)((seed = seed * 1103515245u + 12345u) >> 16) % 300;
        for (int i = 0; i < len; i++) b[i] = (uint8_t)((seed = seed * 1103515245u + 12345u) >> 16);
        const int r = CwlPortal_DnsReply(b, len, sizeof(b), kIp);
        if (r > (int)sizeof(b)) { CHECK(0); break; }
    }
    printf("portal_test: %d checks, %d failed\n", _Checks, _Fails);
    return _Fails ? 1 : 0;
}
