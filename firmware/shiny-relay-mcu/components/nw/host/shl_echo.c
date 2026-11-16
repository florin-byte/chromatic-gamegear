/* shl_echo.c: a SHLINK echo partner on a Mac, for the Chromatic's boot-ROM
 * SHLINK test (tools/nw/bootrom/shltest_boot.inc).
 *
 * The REAL ESP32 code (nw_link.c + shl_client.c via shl_dev.c), with this
 * program playing the GAME: it attaches with LINK_JOIN (the same game identity
 * and family as the boot ROM), takes a room (mine / host / join CODE) and then
 * exchanges every few ms like a game's lk_poll, sending each message it
 * receives straight back. Messages starting with 'R' are the boot ROM's final
 * report: per-message round trips (u16 LE, 1/4096 s) and its own counters;
 * printed as JSON with p50/p95.
 *
 *   ./shl_echo --relay relay.shinyrelay.com:5861 --ticket-cmd CMD --client-id HEX
 *              [--room mine|host|"join CODE"] [--ident S] [--fam F] [--seconds N]
 */
#include "shl_dev.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int xchg(uint8_t ack, const uint8_t *msgs, int nmsgs, int mlen, uint8_t *ans, uint16_t *alen)
{
    uint8_t req[600];
    req[0] = ack;
    req[1] = 1;   /* want */
    req[2] = 0;   /* ask */
    req[3] = 8;   /* in_free */
    req[4] = (uint8_t)nmsgs;
    int o = 5;
    for (int k = 0; k < nmsgs; k++) {
        req[o++] = (uint8_t)mlen;
        memcpy(req + o, msgs + k * mlen, (size_t)mlen);
        o += mlen;
    }
    return NwLink_Op(&DL, kNwLink_OpXchg, req, (uint16_t)o, ans, 1024, alen, shl_dev_wall_ms());
}

static int cmp16(const void *a, const void *b) { return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b; }

int main(int argc, char **argv)
{
    const char *relay = "relay.shinyrelay.com:5861", *room = "mine";
    const char *ident = "SHGID1:shlbt:1", *fam = "shlbt";   /* the boot ROM's */
    uint64_t cid = 0;
    int seconds = 300;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--relay") && i + 1 < argc) relay = argv[++i];
        else if (!strcmp(argv[i], "--ticket-cmd") && i + 1 < argc) snprintf(dev_ticket_cmd, sizeof dev_ticket_cmd, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--client-id") && i + 1 < argc) cid = strtoull(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--room") && i + 1 < argc) room = argv[++i];
        else if (!strcmp(argv[i], "--ident") && i + 1 < argc) ident = argv[++i];
        else if (!strcmp(argv[i], "--fam") && i + 1 < argc) fam = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else { fprintf(stderr, "shl_echo: bad arg %s\n", argv[i]); return 2; }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (shl_dev_init(relay, cid, 0) != 0) { fprintf(stderr, "shl_echo: relay %s?\n", relay); return 1; }
    /* attach as the game */
    uint8_t j[160];
    const size_t il = strlen(ident), fl = strlen(fam);
    j[0] = 16; j[1] = 2; j[2] = 0; j[3] = (uint8_t)il;
    memcpy(j + 4, ident, il);
    j[4 + il] = (uint8_t)fl;
    memcpy(j + 5 + il, fam, fl);
    uint8_t ans[1024];
    uint16_t alen = 0;
    if (NwLink_Op(&DL, kNwLink_OpJoin, j, (uint16_t)(5 + il + fl), ans, sizeof ans, &alen, shl_dev_wall_ms()) != 0) {
        fprintf(stderr, "shl_echo: LINK_JOIN refused\n");
        return 1;
    }
    if (shl_dev_room(room) != 0) { fprintf(stderr, "shl_echo: room %s?\n", room); return 1; }
    uint8_t session = 0, pend[8 * 64];
    int npend = 0, plen = 0;
    unsigned echoed = 0, last_peers = 255, reports = 0;
    char last_code[8] = "";
    const uint32_t t0 = shl_dev_wall_ms();
    uint32_t last_status = 0;
    while ((int)(shl_dev_wall_ms() - t0) < seconds * 1000) {
        shl_dev_net();
        /* echo what came in last time; one message length per batch (the boot ROM's are all one size) */
        const int st = xchg(session, pend, npend, plen, ans, &alen);
        npend = 0;
        if (st == 0 && alen >= 7) {
            session = ans[3];
            if (ans[1] != last_peers) {
                last_peers = ans[1];
                printf("{\"t_ms\":%u,\"event\":\"peers\",\"peers\":%u,\"link\":%u}\n", shl_dev_wall_ms() - t0, ans[1], ans[0]);
            }
            int o = 7;
            for (int k = 0; k < ans[6]; k++) {
                const int m = ans[o];
                const uint8_t *p = ans + o + 2;
                if (m > 0 && p[0] == 'R') {
                    /* the report: 'R', n u8, ok u8, bad u8, timeouts u8, then n x rtt u16 LE */
                    const int n = p[1];
                    uint16_t r[32];
                    int got = 0;
                    for (int i = 0; i < n && 5 + 2 * i + 1 < m && got < 32; i++) r[got++] = (uint16_t)(p[5 + 2 * i] | p[6 + 2 * i] << 8);
                    reports++;
                    printf("{\"t_ms\":%u,\"event\":\"report\",\"part\":%u,\"ok\":%u,\"bad\":%u,\"timeouts\":%u,\"rtt_ms\":[",
                           shl_dev_wall_ms() - t0, reports, p[2], p[3], p[4]);
                    for (int i = 0; i < got; i++) printf("%s%.1f", i ? "," : "", r[i] * 1000.0 / 4096.0);
                    if (got) {
                        qsort(r, (size_t)got, sizeof r[0], cmp16);
                        printf("],\"p50_ms\":%.1f,\"p95_ms\":%.1f,\"max_ms\":%.1f}\n", r[got / 2] * 1000.0 / 4096.0,
                               r[(got * 95 + 99) / 100 - 1] * 1000.0 / 4096.0, r[got - 1] * 1000.0 / 4096.0);
                    } else printf("]}\n");
                } else if (m > 0 && npend < 8 && (npend == 0 || m == plen)) {
                    memcpy(pend + npend * m, p, (size_t)m);
                    plen = m;
                    npend++;
                    echoed++;
                }
                o += 2 + m;
            }
        }
        if (strcmp(last_code, DC.room_code)) {
            snprintf(last_code, sizeof last_code, "%s", DC.room_code);
            printf("{\"t_ms\":%u,\"event\":\"room\",\"code\":\"%s\",\"host\":%d}\n", shl_dev_wall_ms() - t0, DC.room_code, DC.is_host);
        }
        if (shl_dev_wall_ms() - last_status > 5000) {
            last_status = shl_dev_wall_ms();
            char s[1024];
            shl_dev_status(s, sizeof s);
            printf("{\"t_ms\":%u,\"event\":\"status\",\"echoed\":%u,\"joins_sent\":%u,\"welcomes\":%u,\"refusals\":%u,\"last_refuse\":%u,\"dev\":%s}\n",
                   last_status - t0, echoed, DC.stats.joins_sent, DC.stats.welcomes, DC.stats.refusals, DC.stats.last_refuse, s);
        }
        usleep(4000);
    }
    return 0;
}
