// Phone-setup portal helpers (0.17.2, lane fwux1). Pure C, host-tested.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The setup network's DNS: turns the query in Buf[0..N) into its reply in
// place (Buf has Cap bytes). An A/IN question gets one answer, Ip, TTL 30 s;
// any other type (AAAA, HTTPS/SVCB 65, ...) gets NOERROR with no answer
// (NODATA), so a phone's parallel AAAA / HTTPS lookups end at once instead
// of carrying an A record they must discard (before 0.17.2 every question,
// whatever its type, got an A answer). The reply has no authority or
// additional records (a query's EDNS OPT record is dropped, ARCOUNT 0). Returns the reply length, or -1 to stay
// silent (not a query, not exactly one question, truncated name).
int CwlPortal_DnsReply(uint8_t *Buf, int N, int Cap, const uint8_t Ip[4]);

#ifdef __cplusplus
}
#endif
