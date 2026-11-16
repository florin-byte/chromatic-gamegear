/* shl_dev.c: see shl_dev.h. */
#define _DEFAULT_SOURCE
#include "shl_dev.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

nw_fpga_t   DF;
nw_esp_t    DE;
NwLink_t    DL;
ShlClient_t DC;
int         dev_asked;
int         dev_need_ticket;
char        dev_ticket_cmd[512];

static int      sock = -1;
static ShlEp_t  relay_ep;
static uint64_t client_id;
static uint32_t gb_ms;
static uint32_t rng = 0x9E3779B9u;

uint32_t shl_dev_wall_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)((uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u);
}

/* ---- UART model ESP32 <-> FPGA: 11.52 bytes per ms, 4 bytes framing a packet */
#define QMAX 2048
typedef struct { uint8_t p[260]; int n; } qpkt_t;
static qpkt_t q_e2f[QMAX];
static int e2f_head, e2f_tail;
static double e2f_credit, f2e_credit;
static uint8_t f2e_cur[260];
static int f2e_n;

static void esp_send(void *u, const uint8_t *p, int n)
{
    (void)u;
    if (e2f_tail - e2f_head >= QMAX) return;
    qpkt_t *q = &q_e2f[e2f_tail % QMAX];
    memcpy(q->p, p, (size_t)n);
    q->n = n;
    e2f_tail++;
}

void shl_dev_step_ms(void)
{
    gb_ms++;
    e2f_credit += 11.52;
    while (e2f_head != e2f_tail && e2f_credit >= q_e2f[e2f_head % QMAX].n + 4) {
        qpkt_t *q = &q_e2f[e2f_head % QMAX];
        e2f_credit -= q->n + 4;
        e2f_head++;
        nw_fpga_from_esp(&DF, q->p, q->n);
    }
    if (e2f_head == e2f_tail && e2f_credit > 12) e2f_credit = 12;
    f2e_credit += 11.52;
    for (;;) {
        if (!f2e_n) {
            f2e_n = nw_fpga_to_esp(&DF, f2e_cur);
            if (!f2e_n) break;
        }
        if (f2e_credit < f2e_n + 4) break;
        f2e_credit -= f2e_n + 4;
        nw_esp_on_packet(&DE, f2e_cur, f2e_n);
        f2e_n = 0;
    }
    if (!f2e_n && f2e_credit > 12) f2e_credit = 12;
    nw_fpga_tick_ms(&DF, 1);
    nw_esp_tick(&DE, gb_ms);
}

/* ---- the gateway backend: nothing but the SHLINK ops is used here */
static void be_call(void *u, nw_esp_t *e, uint8_t id, const nw_call_t *c)
{
    (void)u;
    (void)c;
    nw_esp_complete(e, id, NW_S_OFFLINE, 0, 0, NULL, 0);
}
static void be_cancel(void *u, uint8_t id) { (void)u; (void)id; }

static uint8_t local_op(void *u, uint8_t op, const uint8_t *body, uint16_t blen, uint8_t *out, uint16_t cap,
                        uint16_t *out_len)
{
    (void)u;
    /* SHL_TRACE=1: every LINK_JOIN / LEAVE, and every XCHG whose want/ask moved
       (what the game says when it asks for a room; host only). */
    static int tr = -1, lw = -1, la = -1;
    if (tr < 0) tr = getenv("SHL_TRACE") != NULL;
    if (tr) {
        const unsigned long ms = (unsigned long)shl_dev_wall_ms();
        if (op == kNwLink_OpJoin && blen >= 5) {
            fprintf(stderr, "TRACE %lu JOIN slot %u maxp %u mode %u ident %.*s fam %.*s\n", ms, body[0], body[1], body[2],
                    body[3], (const char *)body + 4, body[4 + body[3]], (const char *)body + 5 + body[3]);
            lw = la = -1;
        } else if (op == kNwLink_OpLeave) {
            fprintf(stderr, "TRACE %lu LEAVE\n", ms);
        } else if (op == kNwLink_OpXchg && blen >= 5 && (body[1] != lw || body[2] != la)) {
            fprintf(stderr, "TRACE %lu XCHG want %u ask %u (was want %d ask %d; link baseline %d)\n", ms, body[1], body[2], lw, la, DL.last_ask);
            lw = body[1]; la = body[2];
        }
    }
    return NwLink_Op(&DL, op, body, blen, out, cap, out_len, shl_dev_wall_ms());
}

/* ---- the client's host */
static void net_send(void *ctx, ShlEp_t to, const uint8_t *b, size_t n)
{
    (void)ctx;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(to.ip);
    a.sin_port = htons(to.port);
    (void)sendto(sock, b, n, 0, (struct sockaddr *)&a, sizeof a);
}
static uint32_t rand32(void *ctx)
{
    (void)ctx;
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng ^ shl_dev_wall_ms();
}
static void need_ticket(void *ctx)
{
    (void)ctx;
    dev_need_ticket = 1;
}
static void on_ask(void *ctx)
{
    (void)ctx;
    dev_asked++;
    fprintf(stderr, "shl_dev: the game asked for the room menu (lk_ask)\n");
}

int shl_dev_init(const char *relay, uint64_t cid, int window_present)
{
    static const nw_esp_config_t cfg = { 0, 0x15, 4, 8, 10000 };
    static const nw_backend_t be = { NULL, be_call, be_cancel };
    nw_fpga_init(&DF);
    DF.present = window_present;
    nw_esp_init(&DE, &cfg, &be, esp_send, NULL);
    DE.local_op = local_op;
    nw_esp_set_online(&DE, 1);
    const ShlHost_t h = { NULL, net_send, rand32, need_ticket };
    Shl_Init(&DC, &h);
    NwLink_Init(&DL, &DC);
    DL.on_ask = on_ask;
    client_id = cid;
    rng ^= (uint32_t)cid;
    /* the relay */
    char host[256];
    snprintf(host, sizeof host, "%s", relay ? relay : "127.0.0.1:5861");
    char *colon = strrchr(host, ':');
    if (!colon) return -1;
    *colon = '\0';
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, colon + 1, &hints, &res) != 0 || !res) return -1;
    const struct sockaddr_in *sa = (const struct sockaddr_in *)res->ai_addr;
    relay_ep.ip = ntohl(sa->sin_addr.s_addr);
    relay_ep.port = ntohs(sa->sin_port);
    freeaddrinfo(res);
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;
    struct sockaddr_in me;
    memset(&me, 0, sizeof me);
    me.sin_family = AF_INET;
    if (bind(sock, (struct sockaddr *)&me, sizeof me) < 0) return -1;
    fcntl(sock, F_SETFL, O_NONBLOCK);
    return 0;
}

static void run_ticket_cmd(void)
{
    FILE *p = popen(dev_ticket_cmd, "r");
    if (!p) return;
    static char t[2048];
    size_t n = fread(t, 1, sizeof t - 1, p);
    pclose(p);
    while (n && (t[n - 1] == '\n' || t[n - 1] == '\r' || t[n - 1] == ' ')) n--;
    t[n] = '\0';
    if (n) shl_dev_ticket(t);
    memset(t, 0, sizeof t);
}

void shl_dev_ticket(const char *ticket)
{
    dev_need_ticket = 0;
    Shl_SetTicket(&DC, ticket, strlen(ticket), shl_dev_wall_ms());
}

void shl_dev_net(void)
{
    static uint8_t b[2048];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        const ssize_t n = recvfrom(sock, b, sizeof b, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) break;
        const ShlEp_t ep = { ntohl(from.sin_addr.s_addr), ntohs(from.sin_port) };
        (void)Shl_OnDatagram(&DC, ep, b, (size_t)n, shl_dev_wall_ms());
    }
    if (dev_need_ticket && dev_ticket_cmd[0]) run_ticket_cmd();
    Shl_Poll(&DC, shl_dev_wall_ms());
}

int shl_dev_room(const char *what)
{
    const uint32_t now = shl_dev_wall_ms();
    if (!strcmp(what, "leave")) {
        Shl_Leave(&DC);
        return 0;
    }
    if (!DL.attached) return -1;   /* the room menu belongs to a running SHLINK game */
    uint8_t mode;
    const char *arg = "";
    if (!strcmp(what, "host")) mode = kShl_ModeHost;
    else if (!strcmp(what, "mine")) mode = kShl_ModeMine;
    else if (!strncmp(what, "join ", 5)) { mode = kShl_ModeJoin; arg = what + 5; }
    else if (!strncmp(what, "room ", 5)) { mode = kShl_ModeRoom; arg = what + 5; }
    else return -1;
    Shl_Start(&DC, relay_ep, mode, mode == kShl_ModeJoin ? arg : "", mode == kShl_ModeRoom ? arg : "",
              DL.ident, DL.fam, DL.maxp, client_id, now);
    return 0;
}

int shl_dev_status(char *o, int cap)
{
    return snprintf(o, (size_t)cap,
                    "{\"window\":%d,\"unlocked\":%d,\"attached\":%d,\"ident\":\"%s\",\"fam\":\"%s\",\"state\":\"%s\","
                    "\"in_room\":%d,\"host\":%d,\"peers\":%u,\"seat\":%u,\"code\":\"%s\",\"room\":\"%s\",\"session\":%u,"
                    "\"ended\":\"%s\",\"asked\":%d,\"need_ticket\":%d,\"want\":%u,\"xchgs\":%u,\"joins\":%u,"
                    "\"up\":%u,\"down\":%u,\"dropped_offline\":%u,\"lost\":%u,\"pkts_out\":%u,\"pkts_in\":%u,"
                    "\"resends\":%u,\"held_back\":%u,\"rto_ms\":%u,\"srtt_ms\":%u,\"local_ops\":%u}",
                    DF.present, DF.unlocked, DL.attached, DL.ident, DL.fam, Shl_StateName(&DC), Shl_InRoom(&DC),
                    DC.is_host, DC.peers, DC.seat, DC.room_code, DC.room, DL.session,
                    DC.state == kShl_Ended ? Shl_ReasonName(DC.ended_reason) : "", dev_asked, dev_need_ticket, DL.want,
                    DL.xchgs, DL.joins, DL.msgs_up, DL.msgs_down, DL.dropped_offline, DL.lost, DC.stats.pkts_out,
                    DC.stats.pkts_in, DC.stats.resends, DC.stats.held_back, DC.rto_ms, DC.srtt_ms, DE.n_local);
}
