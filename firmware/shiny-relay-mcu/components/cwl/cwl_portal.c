// See cwl_portal.h.
#include "cwl_portal.h"

#include <string.h>

int CwlPortal_DnsReply(uint8_t *Buf, int N, int Cap, const uint8_t Ip[4])
{
    if (N < 12 || Cap < N) return -1;
    if (Buf[2] & 0x80) return -1;                                   // a response, not a query
    if ((Buf[2] & 0x78) != 0) return -1;                            // opcode: standard queries only
    if (Buf[4] != 0 || Buf[5] != 1) return -1;                      // exactly one question
    int q = 12;
    while (q < N && Buf[q] != 0) {
        if ((Buf[q] & 0xC0) != 0) return -1;                        // no compression in a question
        q += Buf[q] + 1;
    }
    if (q >= N) return -1;                                          // the name runs off the end
    q += 1;                                                         // the zero byte
    if (q + 4 > N) return -1;
    const uint16_t type = (uint16_t)(Buf[q] << 8 | Buf[q + 1]);
    const uint16_t cls = (uint16_t)(Buf[q + 2] << 8 | Buf[q + 3]);
    q += 4;                                                         // end of the question
    const int answer = type == 1 && (cls & 0x7FFF) == 1;           // A, IN (mDNS-style top bit ignored)
    if (answer && q + 16 > Cap) return -1;
    Buf[2] = 0x80 | (Buf[2] & 0x01) | 0x04;                         // response, keep RD, AA
    Buf[3] = 0x80;                                                  // RA, NOERROR
    Buf[6] = 0; Buf[7] = (uint8_t)answer;                           // ANCOUNT
    Buf[8] = Buf[9] = 0;                                            // NSCOUNT
    Buf[10] = Buf[11] = 0;                                          // ARCOUNT (no EDNS echo)
    if (!answer) return q;
    static const uint8_t kAns[12] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4};
    memcpy(Buf + q, kAns, sizeof(kAns));
    memcpy(Buf + q + 12, Ip, 4);
    return q + 16;
}
