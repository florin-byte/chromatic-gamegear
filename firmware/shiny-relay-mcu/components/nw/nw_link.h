// nw_link.h: the network window's SHLINK ops, answered by the ESP32 itself
// (never forwarded to the HTTPS gateway). Portable C.
//
// A Shiny Gen GBC game with the SHLINK helper v1.1 (shiny_gen_3
// tests/fixtures/cartlink/shlink_v11_gbc.sgba) moves its mailbox through the
// window once a frame. This file plays the part CartridgeLink plays in the app
// (notes/CARTRIDGE_LINK_V1.md §3, §6): the session counter and its ack, the
// link bits, delivering only into a session the game has adopted, dropping
// what a game pushes with nobody to hear it. The room itself is shl_client.c.
//
//   0x0B LINK_JOIN  req: S u8, max_players u8, mode u8, ident_len u8 + ident,
//                        fam_len u8 + fam
//                   ans: state u8, up_free u8
//                   mode bit 7 (kNwLink_JoinOpenMenu): "open the room menu now".
//                   The game asked for a room (lk_ask / net_menu_now) and this
//                   JOIN is the first the window hears of it: every JOIN resets
//                   the ask baseline (the next XCHG's ask is taken as-is), so
//                   without the bit that ask was swallowed. Treated exactly like
//                   an ask move: the room menu opens when no room is live.
//                   Bits 0-6 are the room mode (reserved, ignored here). Every
//                   game before this bit sends 0, so nothing existing changes.
//   0x0C LINK_XCHG  req: ack u8, want u8, ask u8, in_free u8, n u8,
//                        n x [len u8, payload]
//                   ans: link u8, peers u8, seat u8, session u8, lost u8,
//                        up_free u8, n u8, n x [len u8, from u8, payload]
//                   NOT_IDENTIFIED (2) before a JOIN: the helper joins again.
//   0x0D LINK_LEAVE leave the room; ans: -
//
// 1.0.2 (lane tetrispatch1, PARTY BATTLE v7: the game's own online screens). Additive: a game
// that never sends these sees 1.0.1 exactly; firmware before 1.0.2 answers them NOT_IDENTIFIED
// or UNSUPPORTED (the game reads the version with NW_OP_INFO first).
//   0x0E LINK_ROOM   req: mode u8, len u8 + arg. The game chose its room on its own screen:
//                    kShl_Mode* (0 host, 1 join: arg = the code, 2 mine / the quick match,
//                    3 a room by id: arg = the id) or kNwLink_RoomLeave (leave). No room menu.
//                    Needs a LINK_JOIN first. ans: accepted u8 (1)
//   0x0F LINK_STATUS req: -. ans (version 1, later versions only append):
//                    [0] version 1
//                    [1] flags: bit0 online (Wi-Fi + gateway), bit1 a Wi-Fi network is saved,
//                        bit2 paired with an account, bit3 a game is attached
//                    [2] room state (kShl_Idle .. kShl_Ended)   [3] its ended reason (kShl_R*)
//                    [4] in a room   [5] its host   [6] peers   [7] max peers
//                    [8..13] the room's code (ASCII, zeros: none)
//                    [14] the mode asked for (kShl_Mode*, 0xFF none yet)
//                    [15] pairing (kNwLinkPair*)   [16] code len n   [17..] the pairing code
//   0x10 LINK_UI     req: what u8: 1 open SYSTEM > WI-FI on the screen (the Wi-Fi setup the
//                    game cannot do itself), 2 start pairing (the game shows the code from
//                    LINK_STATUS), 3 cancel pairing, 4 open the room menu. ans: -
#pragma once

#include "shl_client.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    kNwLink_OpJoin = 0x0B,
    kNwLink_OpXchg = 0x0C,
    kNwLink_OpLeave = 0x0D,

    kNwLink_App = 1,
    kNwLink_Room = 2,
    kNwLink_Host = 4,
    kNwLink_Peer = 8,
    kNwLink_OfflineFree = 16,   // up_free with nobody to hear: taken and dropped

    kNwLink_JoinOpenMenu = 0x80, // LINK_JOIN mode bit 7: open the room menu now

    // 1.0.2
    kNwLink_OpRoom = 0x0E,
    kNwLink_OpStatus = 0x0F,
    kNwLink_OpUi = 0x10,
    kNwLink_RoomLeave = 100,
    kNwLink_StatusVer = 1,
    kNwLink_SOnline = 1,
    kNwLink_SWifiSaved = 2,
    kNwLink_SPaired = 4,
    kNwLink_SAttached = 8,
    kNwLink_UiWifi = 1,
    kNwLink_UiPairStart = 2,
    kNwLink_UiPairCancel = 3,
    kNwLink_UiRoomMenu = 4,
    kNwLink_PairCodeMax = 12,
};

// What the device itself says in LINK_STATUS (nw_dev.c fills it; a test sets it).
typedef struct {
    uint8_t flags;             // kNwLink_SOnline | kNwLink_SWifiSaved | kNwLink_SPaired
    uint8_t pair_state;        // 0 idle, 1 starting, 2 waiting (code shown), 3 done, 4 failed
    char pair_code[kNwLink_PairCodeMax + 1];
} NwLinkDev_t;

typedef struct {
    ShlClient_t *c;
    // the attached game (LINK_JOIN)
    bool attached;
    uint8_t slot_size, maxp;
    uint8_t join_mode;         // the last LINK_JOIN's mode byte
    char ident[kShl_IdentMax + 1];
    char fam[kShl_FamMax + 1];
    // the app bytes as this side writes them
    uint8_t session, lost;
    uint32_t seen_gen;
    // what the game said
    int last_ask;              // -1 until the first exchange
    uint8_t want;
    // callbacks: the game asked for the room menu (lk_ask, or a LINK_JOIN with
    // mode bit 7; no room live),
    // and stopped wanting a room (lk_want(0))
    void *ctx;
    void (*on_ask)(void *ctx);
    void (*on_unwant)(void *ctx);
    // 1.0.2: the game chose its room (LINK_ROOM), wants a system screen (LINK_UI), and the
    // device's own state for LINK_STATUS. NULL: the op answers UNSUPPORTED (on_status NULL:
    // the device fields are zero).
    void (*on_room)(void *ctx, uint8_t mode, const char *arg);
    void (*on_ui)(void *ctx, uint8_t what);
    void (*on_status)(void *ctx, NwLinkDev_t *dev);
    uint8_t game_room_mode;    // the last LINK_ROOM mode (0xFF none): the game drives its room
    // counters
    uint32_t joins, xchgs, msgs_up, msgs_down, dropped_offline, stale;
    uint32_t join_asks;        // JOINs that opened the room menu (mode bit 7)
    uint32_t rooms, statuses, uis;   // 1.0.2 ops
    uint32_t last_xchg_ms;
} NwLink_t;

void NwLink_Init(NwLink_t *l, ShlClient_t *c);
// One window op. Writes the answer body to out (cap bytes), its length to
// *out_len, and returns the NW status (NW_S_*).
uint8_t NwLink_Op(NwLink_t *l, uint8_t op, const uint8_t *body, uint16_t blen, uint8_t *out, uint16_t cap,
                  uint16_t *out_len, uint32_t now_ms);
// 1.0.3: XCHG / JOIN with no room client (receive mode's hold): answered as an idle client, never failed.
uint8_t NwLink_OpNoClient(NwLink_t *l, uint8_t op, const uint8_t *body, uint16_t blen, uint8_t *out, uint16_t cap,
                          uint16_t *out_len, uint32_t now_ms);
// The window was locked / the GB reset: the game is gone (the room stays, a
// rebooted cartridge of the same game takes it back on its LINK_JOIN).
void NwLink_Detach(NwLink_t *l);
// The game wants a room right now (it is on its multiplayer screen).
bool NwLink_Wants(const NwLink_t *l);

#ifdef __cplusplus
}
#endif
