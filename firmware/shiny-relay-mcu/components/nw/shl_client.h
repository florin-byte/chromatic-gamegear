// shl_client.h: the Chromatic's SHLINK room client, portable C (no OS calls).
//
// A Shiny Gen GBC game's SHLINK mailbox reaches the ESP32 through the network
// window (nw_link.c); this client puts it in the game's multiplayer room on
// Shiny Relay as one more seat, over UDP (shiny_gen_3
// shiny_server_rs/services/shiny_multiplayer/src/shlink.rs, the byte layouts
// and golden bytes are pinned on both sides):
//
//   SJOIN 0x60    nonce u32, mode u8, max_players u8, code[6], client_id u64,
//                 room_len u8 + room, ident_len u8 + ident, fam_len u8 + fam,
//                 ticket_len u16 + ticket                          (c -> r)
//   SWELCOME 0x61 nonce u32, token u64                             (r -> c)
//   SDATA 0x62    token u64, ack u16, seq u16, n u8, n x [len u8, item]
//   REFUSE 0x03   nonce u32, reason u8 (nonce 0 + 8: the seat is gone)
//   BYE 0x50      token u64 (leave cleanly)
//
// SDATA is a reliable in-order item stream each way (go-back-N: every packet
// carries every unacked item; ack = the next item expected). Items up:
// [1, payload] a message. Items down: [1, from_seat, payload] a message,
// [2, flags, peers, seat, epoch, max, code[6], room_len, room] the room,
// [3, reason] the seat is over. The client acks only the messages it has room
// for, so a full queue holds the relay back instead of dropping anything.
//
// No TLS anywhere here: the ticket (from /link_device/ticket over HTTPS, the
// spare-ticket mechanism in cwl_online.c) is the only credential, single use.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    kShl_TSJoin = 0x60,
    kShl_TSWelcome = 0x61,
    kShl_TSData = 0x62,
    kShl_TRefuse = 0x03,
    kShl_TBye = 0x50,

    kShl_ModeHost = 0,
    kShl_ModeJoin = 1,
    kShl_ModeMine = 2,
    kShl_ModeRoom = 3,

    kShl_IMsg = 1,
    kShl_IState = 2,
    kShl_IEnded = 3,
    kShl_FRoom = 1,
    kShl_FHost = 2,

    kShl_RNoRoom = 2,
    kShl_RRoomFull = 3,
    kShl_RRate = 4,
    kShl_RUnknownSession = 8,
    kShl_RRoomClosed = 11,
    kShl_RKicked = 12,
    kShl_RTaken = 13,
    kShl_RPartyConsent = 14,    // a public party seat without the account's OK (party_ask.h)

    kShl_MsgMax = 64,
    kShl_TxItems = 12,          // messages up, unacked
    kShl_DownItems = 12,        // messages down, waiting for the game (the game's own ring is 8)
    kShl_JoinMax = 768,         // the whole SJOIN (a ticket is ~450 B), kept for the resends;
                                // once seated the same buffer builds each SDATA
    kShl_RoomIdMax = 64,
    kShl_IdentMax = 64,
    kShl_FamMax = 32,
    kShl_Budget = 740,          // item bytes per SDATA (12 x 66 fits; the buffer is the SJOIN one)

    kShl_JoinResend_ms = 250,
    kShl_JoinGiveUp_ms = 8000,
    kShl_RtoInit_ms = 200,
    kShl_RtoMin_ms = 60,
    kShl_RtoMax_ms = 2000,
    kShl_Keepalive_ms = 2000,
    kShl_TicketMaxAge_ms = 100000,
};

typedef struct {
    uint32_t ip;     // host order
    uint16_t port;
} ShlEp_t;

typedef enum {
    kShl_Idle,
    kShl_NeedTicket,
    kShl_Joining,
    kShl_Seated,
    kShl_Ended,
} ShlState_t;

typedef struct {
    void *ctx;
    void (*send)(void *ctx, ShlEp_t to, const uint8_t *buf, size_t n);
    uint32_t (*rand32)(void *ctx);
    // A ticket is needed (Shl_SetTicket when it arrives).
    void (*need_ticket)(void *ctx);
} ShlHost_t;

typedef struct {
    uint8_t len;
    uint8_t b[2 + kShl_MsgMax];
} ShlItem_t;

typedef struct {
    uint32_t joins_sent, welcomes, refusals, rejoins;
    uint32_t pkts_out, pkts_in, items_out, items_in, resends, dups_in, held_back;
    uint32_t msgs_up, msgs_down, states;
    uint8_t last_refuse;
} ShlStats_t;

typedef struct {
    ShlHost_t host;
    ShlState_t state;
    ShlEp_t relay;
    // what was asked
    uint8_t mode, asked_mode;
    char code[7];
    char room[kShl_RoomIdMax + 1];
    char ident[kShl_IdentMax + 1];
    char fam[kShl_FamMax + 1];
    uint8_t maxp;
    uint64_t client_id;
    // joining
    uint8_t join[kShl_JoinMax];
    uint16_t join_len;
    uint32_t nonce, join_start_ms, join_last_ms;
    // seat
    uint64_t token;
    uint32_t seat_gen;           // bumped by the first STATE of every seat (a new room)
    bool seat_has_state;
    bool in_room, is_host;
    uint8_t peers, seat, max_peers;
    char room_code[7];
    uint8_t ended_reason;
    // the stream, up
    ShlItem_t tx[kShl_TxItems];
    uint8_t tx_n;
    uint16_t tx_base;
    bool dirty, ack_owed;
    uint32_t last_send_ms, front_sent_ms;
    bool front_sent, front_resent;
    uint32_t rto_ms, srtt_ms;
    bool have_srtt;
    // the stream, down
    uint16_t rx_expect;
    ShlItem_t down[kShl_DownItems];   // [from_seat, payload]
    uint8_t down_head, down_n;
    uint32_t last_heard_ms;
    ShlStats_t stats;
} ShlClient_t;

void Shl_Init(ShlClient_t *c, const ShlHost_t *host);
// Take a seat: mode kShl_Mode*, `code` for join, `room` (id) for room.
// ident = the cartridge's SHGID1 string, fam = its LKFAM ("" for none).
void Shl_Start(ShlClient_t *c, ShlEp_t relay, uint8_t mode, const char *code, const char *room,
               const char *ident, const char *fam, uint8_t maxp, uint64_t client_id, uint32_t now_ms);
// A fresh relay ticket (single use) for the pending JOIN.
void Shl_SetTicket(ShlClient_t *c, const char *ticket, size_t len, uint32_t now_ms);
// Leave the room (BYE) and go idle.
void Shl_Leave(ShlClient_t *c);
// True when buf was SHLINK traffic for this client (consumed).
bool Shl_OnDatagram(ShlClient_t *c, ShlEp_t from, const uint8_t *buf, size_t n, uint32_t now_ms);
void Shl_Poll(ShlClient_t *c, uint32_t now_ms);

// Messages up: room left in the stream, and push one (false = full / not seated).
uint8_t Shl_TxFree(const ShlClient_t *c);
bool Shl_Push(ShlClient_t *c, const uint8_t *payload, uint8_t n);
// Messages down, in order.
uint8_t Shl_DownCount(const ShlClient_t *c);
// The next one: *from = sender's seat, payload into out (>= 64), returns its length (0: none).
uint8_t Shl_Pop(ShlClient_t *c, uint8_t *from, uint8_t *out);

bool Shl_InRoom(const ShlClient_t *c);
const char *Shl_StateName(const ShlClient_t *c);
const char *Shl_ReasonName(uint8_t reason);

// Wire helpers (exported for the golden-byte tests).
size_t Shl_EncodeJoin(uint8_t *out, size_t cap, uint32_t nonce, uint8_t mode, uint8_t maxp, const char *code,
                      uint64_t client_id, const char *room, const char *ident, const char *fam,
                      const uint8_t *ticket, size_t ticket_len);
size_t Shl_EncodeData(uint8_t *out, size_t cap, uint64_t token, uint16_t ack, uint16_t seq,
                      const ShlItem_t *items, uint8_t n);

#ifdef __cplusplus
}
#endif
