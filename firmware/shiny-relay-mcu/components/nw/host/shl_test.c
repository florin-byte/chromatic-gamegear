/* shl_test.c: the ESP32's SHLINK side on the Mac (lane chromshlink1).
 * Golden bytes shared with shiny_multiplayer shlink.rs, the client's stream
 * and flow control, and the window ops' session rules (CartridgeLink's). */
#include <stdio.h>
#include <string.h>

#include "nw_link.h"
#include "nw_proto.h"
#include "shl_client.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void hex(char *o, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) sprintf(o + 2 * i, "%02x", b[i]);
    o[2 * n] = '\0';
}

/* a fake network: what the client sent */
static uint8_t sent[16][1300];
static size_t sent_n[16];
static int nsent;
static int tickets_asked;
static void fsend(void *c, ShlEp_t to, const uint8_t *b, size_t n)
{
    (void)c; (void)to;
    if (nsent < 16) { memcpy(sent[nsent], b, n); sent_n[nsent] = n; }
    nsent++;
}
static uint32_t frand(void *c) { (void)c; return 0x01020304; }
static void fneed(void *c) { (void)c; tickets_asked++; }

static const ShlEp_t RELAY = { 0x7F000001, 5861 };

static size_t sdata(uint8_t *o, uint64_t tok, uint16_t ack, uint16_t seq, const uint8_t *const *items, const uint8_t *lens, int n)
{
    ShlItem_t it[32];
    for (int i = 0; i < n; i++) { it[i].len = lens[i]; memcpy(it[i].b, items[i], lens[i]); }
    return Shl_EncodeData(o, 1300, tok, ack, seq, it, (uint8_t)n);
}

static void golden(void)
{
    uint8_t b[1300];
    char h[2700];
    /* ⚠ the same bytes as shlink.rs golden_layouts */
    size_t n = Shl_EncodeJoin(b, sizeof b, 0x01020304, kShl_ModeJoin, 2, "BCD234", 0x1122334455667788ull, "",
                              "SHGID1:g1:0", "SHFAM1:fam", (const uint8_t *)"t.k.n", 5);
    hex(h, b, n);
    CHECK(!strcmp(h, "534c01600102030401024243443233341122334455667788000b5348474944313a67313a300a534846414d313a66616d0005742e6b2e6e"),
          "SJOIN golden: %s", h);
    ShlItem_t it[2] = { { 2, { 1, 42 } }, { 6, { 2, 1, 1, 0, 1, 2 } } };
    n = Shl_EncodeData(b, sizeof b, 5, 0x0102, 0xFFFF, it, 2);
    hex(h, b, n);
    const uint8_t want[] = { 'S', 'L', 1, 0x62, 0, 0, 0, 0, 0, 0, 0, 5, 1, 2, 0xFF, 0xFF, 2, 2, 1, 42, 6, 2, 1, 1, 0, 1, 2 };
    CHECK(n == sizeof want && !memcmp(b, want, n), "SDATA golden: %s", h);
}

static ShlClient_t C;

static void seat(void)
{
    const ShlHost_t host = { NULL, fsend, frand, fneed };
    Shl_Init(&C, &host);
    nsent = 0;
    tickets_asked = 0;
    Shl_Start(&C, RELAY, kShl_ModeHost, "", "", "SHGID1:g:0", "", 2, 0xAB, 0);
    CHECK(C.state == kShl_NeedTicket && tickets_asked == 1, "asks for a ticket");
    Shl_SetTicket(&C, "a.b.c", 5, 10);
    CHECK(C.state == kShl_Joining && nsent == 1 && sent[0][3] == kShl_TSJoin, "SJOIN sent");
    Shl_Poll(&C, 100);
    CHECK(nsent == 1, "no resend before 250 ms");
    Shl_Poll(&C, 270);
    CHECK(nsent == 2 && sent_n[1] == sent_n[0] && !memcmp(sent[1], sent[0], sent_n[0]), "the same SJOIN again");
    uint8_t w[16] = { 'S', 'L', 1, kShl_TSWelcome, 1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 9 };
    CHECK(Shl_OnDatagram(&C, RELAY, w, 16, 300) && C.state == kShl_Seated && C.token == 9, "seated");
}

static void stream(void)
{
    uint8_t b[1300];
    seat();
    /* the room: a STATE and two messages */
    const uint8_t st[] = { kShl_IState, kShl_FRoom | kShl_FHost, 1, 0, 1, 2, 'Q', 'R', 'S', '7', '8', '9', 3, 'a', 'b', 'c' };
    const uint8_t m1[] = { kShl_IMsg, 1, 7, 7 }, m2[] = { kShl_IMsg, 1, 8 };
    const uint8_t *items[] = { st, m1, m2 };
    const uint8_t lens[] = { sizeof st, sizeof m1, sizeof m2 };
    size_t n = sdata(b, 9, 0, 0, items, lens, 3);
    Shl_OnDatagram(&C, RELAY, b, n, 400);
    CHECK(Shl_InRoom(&C) && C.is_host && C.peers == 1 && !strcmp(C.room_code, "QRS789") && !strcmp(C.room, "abc"), "room view");
    CHECK(C.seat_gen == 1 && Shl_DownCount(&C) == 2, "two messages down, seat gen 1");
    uint8_t from, p[64];
    CHECK(Shl_Pop(&C, &from, p) == 2 && from == 1 && p[0] == 7, "first message");
    /* a duplicate delivers nothing twice */
    Shl_OnDatagram(&C, RELAY, b, n, 410);
    CHECK(Shl_DownCount(&C) == 1 && C.stats.dups_in == 3, "dups dropped");
    /* the ack goes out, with our messages */
    CHECK(Shl_Push(&C, (const uint8_t *)"\x05\x06", 2), "push");
    nsent = 0;
    Shl_Poll(&C, 420);
    CHECK(nsent == 1 && sent[0][3] == kShl_TSData && sent[0][12] == 0 && sent[0][13] == 3 && sent[0][16] == 1, "ack 3 + one item");
    /* the relay acks it: the stream is empty, the RTT measured */
    const uint8_t *none[1];
    n = sdata(b, 9, 1, 3, none, lens, 0);
    Shl_OnDatagram(&C, RELAY, b, n, 470);
    CHECK(C.tx_n == 0 && C.have_srtt && C.srtt_ms == 50, "acked, srtt %u", C.srtt_ms);
    /* flow control: more messages than the queue holds are not acked */
    const uint8_t *many[24];
    uint8_t ml[24], mm[24][3];
    for (int i = 0; i < 24; i++) { mm[i][0] = kShl_IMsg; mm[i][1] = 0; mm[i][2] = (uint8_t)i; many[i] = mm[i]; ml[i] = 3; }
    n = sdata(b, 9, 1, 3, many, ml, 24);
    Shl_OnDatagram(&C, RELAY, b, n, 500);
    CHECK(Shl_DownCount(&C) == kShl_DownItems && C.rx_expect == 3 + (kShl_DownItems - 1) && C.stats.held_back == 1, "held back at %u (expect %u)", Shl_DownCount(&C), C.rx_expect);
    /* the retransmit timer: unacked message goes again, then backs off */
    Shl_Push(&C, (const uint8_t *)"\x01", 1);
    nsent = 0;
    Shl_Poll(&C, 510);
    const uint32_t rto = C.rto_ms;
    Shl_Poll(&C, 510 + rto - 1);
    CHECK(nsent == 1, "no early resend");
    Shl_Poll(&C, 510 + rto);
    CHECK(nsent == 2 && C.stats.resends == 1 && C.rto_ms == rto * 2, "resent after the rto, backoff");
    /* the relay forgot us: rejoin the same room by id */
    const uint8_t rf[9] = { 'S', 'L', 1, kShl_TRefuse, 0, 0, 0, 0, kShl_RUnknownSession };
    tickets_asked = 0;
    Shl_OnDatagram(&C, RELAY, rf, 9, 2000);
    CHECK(C.state == kShl_NeedTicket && C.mode == kShl_ModeRoom && !strcmp(C.room, "abc") && tickets_asked == 1, "rejoin by room id");
    /* ENDED */
    seat();
    const uint8_t en[] = { kShl_IEnded, kShl_RNoRoom };
    const uint8_t *ei[] = { en };
    const uint8_t el[] = { 2 };
    n = sdata(b, 9, 0, 0, ei, el, 1);
    Shl_OnDatagram(&C, RELAY, b, n, 400);
    CHECK(C.state == kShl_Ended && C.ended_reason == kShl_RNoRoom, "ended no_room");
}

static uint8_t op(NwLink_t *l, uint8_t o, const uint8_t *body, uint16_t n, uint8_t *out, uint16_t *ol)
{
    return NwLink_Op(l, o, body, n, out, 1024, ol, 0);
}

static int asks;
static void on_ask(void *c) { (void)c; asks++; }

static void window_ops(void)
{
    uint8_t out[1024], b[1300];
    uint16_t ol;
    NwLink_t L;
    seat();
    NwLink_Init(&L, &C);
    L.on_ask = on_ask;
    const uint8_t x0[] = { 0, 1, 0, 8, 0 };
    CHECK(op(&L, kNwLink_OpXchg, x0, 5, out, &ol) == NW_S_NOT_IDENTIFIED, "XCHG before JOIN");
    const uint8_t j[] = { 16, 2, 0, 9, 'S', 'H', 'G', 'I', 'D', '1', ':', 'g', ':', 7, 'S', 'H', 'F', 'A', 'M', '1', ':' };
    CHECK(op(&L, kNwLink_OpJoin, j, sizeof j, out, &ol) == NW_S_OK && ol == 2 && L.attached && L.session == 1, "JOIN");
    seat();   /* the OSD menu's choice seated the client */
    CHECK(!strcmp(L.ident, "SHGID1:g:") && !strcmp(L.fam, "SHFAM1:"), "identity '%s' '%s'", L.ident, L.fam);
    /* no room yet: app bit only; what the game pushes is taken and dropped */
    const uint8_t x1[] = { 1, 1, 0, 8, 2, 1, 9, 2, 9, 9 };
    CHECK(op(&L, kNwLink_OpXchg, x1, sizeof x1, out, &ol) == NW_S_OK, "XCHG");
    CHECK(out[0] == kNwLink_App && out[3] == 1 && out[5] == kNwLink_OfflineFree && out[6] == 0 && L.dropped_offline == 2,
          "offline answer %u %u %u", out[0], out[3], out[5]);
    /* ask moved, no room: the menu */
    const uint8_t x2[] = { 1, 1, 1, 8, 0 };
    op(&L, kNwLink_OpXchg, x2, 5, out, &ol);
    CHECK(asks == 1, "lk_ask opens the room menu");
    /* the room arrives with a message already waiting */
    const uint8_t st[] = { kShl_IState, kShl_FRoom, 1, 1, 1, 2, 'A', 'B', 'C', 'D', 'E', 'F', 0 };
    const uint8_t m1[] = { kShl_IMsg, 0, 0x55, 0x66 };
    const uint8_t *items[] = { st, m1 };
    const uint8_t lens[] = { sizeof st, sizeof m1 };
    size_t n = sdata(b, 9, 0, 0, items, lens, 2);
    Shl_OnDatagram(&C, RELAY, b, n, 400);
    /* the game still acks session 1: the session moves, nothing is delivered, what it sent is stale */
    const uint8_t x3[] = { 1, 1, 1, 8, 1, 1, 7 };
    op(&L, kNwLink_OpXchg, x3, sizeof x3, out, &ol);
    CHECK(out[0] == (kNwLink_App | kNwLink_Room | kNwLink_Peer) && out[1] == 1 && out[2] == 1 && out[3] == 2 && out[6] == 0 && L.stale == 1,
          "new session %u, no delivery (%u)", out[3], out[6]);
    /* adopted: delivered, and its message goes up */
    const uint8_t x4[] = { 2, 1, 1, 8, 1, 3, 1, 2, 3 };
    op(&L, kNwLink_OpXchg, x4, sizeof x4, out, &ol);
    CHECK(out[6] == 1 && out[7] == 2 && out[8] == 0 && out[9] == 0x55 && L.msgs_up == 1 && C.tx_n == 1, "delivered + sent");
    CHECK(out[5] == kShl_TxItems - 1, "up_free = the stream's room (%u)", out[5]);
    /* in_free caps the deliveries */
    const uint8_t m2[] = { kShl_IMsg, 0, 1 }, m3[] = { kShl_IMsg, 0, 2 };
    const uint8_t *i2[] = { m2, m3 };
    const uint8_t l2[] = { 3, 3 };
    n = sdata(b, 9, 0, 2, i2, l2, 2);
    Shl_OnDatagram(&C, RELAY, b, n, 450);
    const uint8_t x5[] = { 2, 1, 1, 1, 0 };
    op(&L, kNwLink_OpXchg, x5, 5, out, &ol);
    CHECK(out[6] == 1 && Shl_DownCount(&C) == 1, "one slot free -> one message");
    /* a rebooted cartridge of the same game: same room, the session moves */
    const uint8_t s0 = L.session;
    op(&L, kNwLink_OpJoin, j, sizeof j, out, &ol);
    CHECK(Shl_InRoom(&C) && L.session == (uint8_t)(s0 + 1), "reboot keeps the room");
    /* LEAVE */
    op(&L, kNwLink_OpLeave, NULL, 0, out, &ol);
    CHECK(C.state == kShl_Idle, "left");
}

/* LINK_JOIN mode bit 7, "open the room menu now" (shiny_gen_3 notes/SHLINK_ON_CHROMATIC.md §2,
 * the GBC link library's net_menu_now): the ask that arrives with a fresh join is not swallowed. */
static void join_open_menu(void)
{
    uint8_t out[1024];
    uint16_t ol;
    NwLink_t L;
    seat();   /* a seated client, no room yet */
    NwLink_Init(&L, &C);
    L.on_ask = on_ask;
    uint8_t j[] = { 16, 2, 0, 9, 'S', 'H', 'G', 'I', 'D', '1', ':', 'g', ':', 7, 'S', 'H', 'F', 'A', 'M', '1', ':' };
    /* mode 0, the game wanting a room and its ask already moved: the first XCHG is the
     * baseline and the menu stays shut. This is the swallowed ask, unchanged for old games. */
    asks = 0;
    CHECK(op(&L, kNwLink_OpJoin, j, sizeof j, out, &ol) == NW_S_OK && L.join_mode == 0, "JOIN mode 0");
    const uint8_t x1[] = { 1, 1, 1, 8, 0 };
    op(&L, kNwLink_OpXchg, x1, 5, out, &ol);
    CHECK(asks == 0 && L.join_asks == 0, "mode 0: the ask that came with the join is the baseline (%d)", asks);
    /* bit 7: the menu opens at the JOIN, once; the baseline XCHG after it does not open it again */
    NwLink_Detach(&L);
    j[2] = kNwLink_JoinOpenMenu;
    CHECK(op(&L, kNwLink_OpJoin, j, sizeof j, out, &ol) == NW_S_OK && ol == 2, "JOIN mode 0x80");
    CHECK(asks == 1 && L.join_asks == 1 && L.join_mode == 0x80, "bit 7 opens the room menu (%d)", asks);
    op(&L, kNwLink_OpXchg, x1, 5, out, &ol);
    CHECK(asks == 1, "the baseline XCHG after it does not open it twice (%d)", asks);
    /* a later ask is still a move */
    const uint8_t x2[] = { 1, 1, 2, 8, 0 };
    op(&L, kNwLink_OpXchg, x2, 5, out, &ol);
    CHECK(asks == 2, "a later ask still opens it (%d)", asks);
    /* the low bits are the room mode: ignored, and never read as the flag */
    j[2] = 0x02;
    op(&L, kNwLink_OpJoin, j, sizeof j, out, &ol);
    CHECK(asks == 2 && L.join_mode == 0x02, "mode 2 is not the flag");
    /* a room live: the flag opens nothing (a rebooted cartridge takes its room back) */
    seat();
    const uint8_t st[] = { kShl_IState, kShl_FRoom, 1, 1, 1, 2, 'A', 'B', 'C', 'D', 'E', 'F', 0 };
    const uint8_t *items[] = { st };
    const uint8_t lens[] = { sizeof st };
    uint8_t b[1300];
    size_t n = sdata(b, 9, 0, 0, items, lens, 1);
    Shl_OnDatagram(&C, RELAY, b, n, 400);
    CHECK(Shl_InRoom(&C), "in a room");
    j[2] = kNwLink_JoinOpenMenu;
    op(&L, kNwLink_OpJoin, j, sizeof j, out, &ol);
    CHECK(asks == 2 && L.join_asks == 1, "bit 7 with a room live opens nothing (%d)", asks);
    /* no callback: nothing to call, no crash */
    L.on_ask = NULL;
    Shl_Leave(&C);
    op(&L, kNwLink_OpJoin, j, sizeof j, out, &ol);
    CHECK(L.attached, "bit 7 without an on_ask");
}

/* 1.0.2: the game's own online screens (LINK_ROOM / LINK_STATUS / LINK_UI). Additive: without the
 * callbacks every new op is UNSUPPORTED, and a game that never sends them sees 1.0.1. */
static int rooms_n, uis_n, last_mode = -1, last_ui = -1;
static char last_arg[80];
static void on_room(void *c, uint8_t m, const char *a) { (void)c; rooms_n++; last_mode = m; snprintf(last_arg, sizeof last_arg, "%s", a); }
static void on_ui(void *c, uint8_t w) { (void)c; uis_n++; last_ui = w; }
static void on_status(void *c, NwLinkDev_t *d) { (void)c; d->flags = kNwLink_SOnline | kNwLink_SPaired | 0x80; d->pair_state = 2; snprintf(d->pair_code, sizeof d->pair_code, "ABCD-EFGHIJKLMNOP"); }

static void game_screens(void)
{
    uint8_t out[1024];
    uint16_t ol;
    NwLink_t L;
    Shl_Leave(&C);
    NwLink_Init(&L, &C);
    L.on_ask = on_ask;
    const uint8_t room_host[] = { kShl_ModeHost, 0 };
    const uint8_t ui1[] = { kNwLink_UiWifi };
    /* no callbacks (a firmware build without them): UNSUPPORTED, status still answers */
    CHECK(op(&L, kNwLink_OpRoom, room_host, 2, out, &ol) == NW_S_UNSUPPORTED, "LINK_ROOM without on_room");
    CHECK(op(&L, kNwLink_OpUi, ui1, 1, out, &ol) == NW_S_UNSUPPORTED, "LINK_UI without on_ui");
    CHECK(op(&L, kNwLink_OpStatus, NULL, 0, out, &ol) == NW_S_OK && ol == 17 && out[0] == 1 && out[1] == 0 && out[14] == 0xFF,
          "LINK_STATUS without on_status: device fields zero (%u %u %u)", ol, out[1], out[14]);
    L.on_room = on_room;
    L.on_ui = on_ui;
    L.on_status = on_status;
    /* a room needs the game attached */
    CHECK(op(&L, kNwLink_OpRoom, room_host, 2, out, &ol) == NW_S_NOT_IDENTIFIED && rooms_n == 0, "LINK_ROOM before JOIN");
    const uint8_t j[] = { 16, 2, 0, 9, 'S', 'H', 'G', 'I', 'D', '1', ':', 'g', ':', 7, 'S', 'H', 'F', 'A', 'M', '1', ':' };
    CHECK(op(&L, kNwLink_OpJoin, j, sizeof j, out, &ol) == NW_S_OK, "JOIN");
    CHECK(op(&L, kNwLink_OpRoom, room_host, 2, out, &ol) == NW_S_OK && ol == 1 && out[0] == 1 && rooms_n == 1 && last_mode == kShl_ModeHost,
          "LINK_ROOM host");
    const uint8_t room_join[] = { kShl_ModeJoin, 6, 'B', 'C', 'D', '2', '3', '4' };
    CHECK(op(&L, kNwLink_OpRoom, room_join, sizeof room_join, out, &ol) == NW_S_OK && last_mode == kShl_ModeJoin && !strcmp(last_arg, "BCD234"),
          "LINK_ROOM join BCD234 ('%s')", last_arg);
    const uint8_t bad[] = { 7, 0 };
    CHECK(op(&L, kNwLink_OpRoom, bad, 2, out, &ol) == NW_S_BAD_REQUEST, "an unknown mode");
    const uint8_t shortb[] = { kShl_ModeJoin, 6, 'B' };
    CHECK(op(&L, kNwLink_OpRoom, shortb, 3, out, &ol) == NW_S_BAD_REQUEST, "a short arg");
    const uint8_t leave[] = { kNwLink_RoomLeave, 0 };
    CHECK(op(&L, kNwLink_OpRoom, leave, 2, out, &ol) == NW_S_OK && last_mode == kNwLink_RoomLeave, "LINK_ROOM leave");
    CHECK(op(&L, kNwLink_OpUi, ui1, 1, out, &ol) == NW_S_OK && last_ui == kNwLink_UiWifi && uis_n == 1, "LINK_UI wi-fi");
    /* status: the device's flags (unknown bits dropped), attached, the pairing code cut to 12 */
    CHECK(op(&L, kNwLink_OpStatus, NULL, 0, out, &ol) == NW_S_OK, "LINK_STATUS");
    CHECK(out[1] == (kNwLink_SOnline | kNwLink_SPaired | kNwLink_SAttached) && out[15] == 2 && out[16] == 12 && ol == 29 &&
          !memcmp(out + 17, "ABCD-EFGHIJK", 12), "status flags %02x pair %u len %u", out[1], out[15], out[16]);
    /* in a room: state, host, peers, code */
    seat();
    const uint8_t st[] = { kShl_IState, kShl_FRoom | kShl_FHost, 1, 0, 1, 2, 'B', 'C', 'D', '2', '3', '4', 0 };
    const uint8_t *items[] = { st };
    const uint8_t lens[] = { sizeof st };
    uint8_t b[1300];
    size_t n = sdata(b, 9, 0, 0, items, lens, 1);
    Shl_OnDatagram(&C, RELAY, b, n, 400);
    op(&L, kNwLink_OpStatus, NULL, 0, out, &ol);
    CHECK(out[2] == kShl_Seated && out[4] == 1 && out[5] == 1 && out[6] == 1 && out[7] == 2 && !memcmp(out + 8, "BCD234", 6),
          "status in a room: state %u room %u host %u peers %u code %.6s", out[2], out[4], out[5], out[6], out + 8);
    /* the game drives its room: an ask never opens the menu (the 1.0.2 OnAsk checks this) */
    CHECK(L.game_room_mode == kNwLink_RoomLeave, "the game's last choice is kept");
    /* another game attaches: it chooses its own */
    const uint8_t j2[] = { 16, 2, 0, 3, 'X', 'Y', 'Z', 7, 'S', 'H', 'F', 'A', 'M', '1', ':' };
    op(&L, kNwLink_OpJoin, j2, sizeof j2, out, &ol);
    CHECK(L.game_room_mode == 0xFF, "a new game resets it");
    /* an old game (never sends the new ops) sees the XCHG answer of 1.0.1, byte for byte */
    const uint8_t x0[] = { 1, 1, 0, 8, 0 };
    CHECK(op(&L, kNwLink_OpXchg, x0, 5, out, &ol) == NW_S_OK && ol == 7, "XCHG unchanged (%u)", ol);
}


/* 1.0.3: the window with no room client (receive mode's hold): XCHG and JOIN answered as an idle client, OK */
static void window_no_client(void)
{
    uint8_t out[1024];
    uint16_t ol;
    NwLink_t L;
    NwLink_Init(&L, NULL);
    const uint8_t x0[] = { 0, 1, 0, 8, 0 };
    CHECK(NwLink_OpNoClient(&L, kNwLink_OpXchg, x0, 5, out, 1024, &ol, 0) == NW_S_NOT_IDENTIFIED, "no client: XCHG before any JOIN -> NOT_IDENTIFIED (a fresh cart joins)");
    const uint8_t j[] = { 16, 2, 0, 9, 'S', 'H', 'G', 'I', 'D', '1', ':', 'g', ':', 7, 'S', 'H', 'F', 'A', 'M', '1', ':' };
    CHECK(NwLink_OpNoClient(&L, kNwLink_OpJoin, j, sizeof j, out, 1024, &ol, 0) == NW_S_OK && ol == 2 && out[0] == kShl_Idle && L.attached && L.session == 1,
          "no client: JOIN is OK, idle (PARTY v3-v11 latch any other answer)");
    const uint8_t x1[] = { 1, 1, 0, 8, 1, 3, 'a', 'b', 'c' };
    const uint32_t d0 = L.dropped_offline;
    CHECK(NwLink_OpNoClient(&L, kNwLink_OpXchg, x1, sizeof x1, out, 1024, &ol, 0) == NW_S_OK && ol == 7 && out[0] == kNwLink_App && out[1] == 0 &&
          out[3] == L.session && out[5] == kNwLink_OfflineFree && out[6] == 0 && L.dropped_offline == d0 + 1, "no client: XCHG is OK, no room, the pushed message dropped");
    CHECK(NwLink_OpNoClient(&L, kNwLink_OpRoom, x0, 5, out, 1024, &ol, 0) == NW_S_UNSUPPORTED, "no client: other ops are not served there");
}

int main(void)
{
    golden();
    stream();
    window_ops();
    join_open_menu();
    game_screens();
    window_no_client();
    printf("shl_test: %d/%d checks pass\n", checks - fails, checks);
    return fails ? 1 : 0;
}
