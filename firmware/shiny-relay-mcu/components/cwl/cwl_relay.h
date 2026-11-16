// cwl_relay.h: Shiny Link relay client (envelope v1), portable C.
//
// The ESP32 port of shiny_multiplayer's link_wire.rs + cwl_relay_shim.rs
// (shiny_gen_3 notes/CHROMATIC_RELAY.md §2, §7, §10). No OS calls: the host
// supplies time, randomness and datagram I/O through callbacks, so the same
// file runs on the Chromatic and in the desktop tests/shim
// (tools/cwl_relay_test.c, tools/cwl_relay_cshim.c).
//
// The CWL2 payload is opaque here: DATA carries whatever the protocol core
// (cwl_proto.c) sends, unchanged.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------- envelope v1

enum {
    kSl_Version = 1,
    kSl_Hdr = 4,

    kSl_Join = 0x01,
    kSl_Welcome = 0x02,
    kSl_Refuse = 0x03,
    kSl_Data = 0x10,
    kSl_Peer = 0x20,
    kSl_Ping = 0x30,
    kSl_Pong = 0x31,
    kSl_Punch = 0x40,
    kSl_PunchAck = 0x41,
    kSl_Bye = 0x50,

    kSl_ModeHost = 0,
    kSl_ModeJoin = 1,
    kSl_ModeMine = 2,
    kSl_KindCwl2 = 1,

    kSl_RBadTicket = 1,
    kSl_RNoRoom = 2,
    kSl_RRoomFull = 3,
    kSl_RRate = 4,
    kSl_RBadPacket = 5,
    kSl_RRelayFull = 6,
    kSl_RReplay = 7,
    kSl_RUnknownSession = 8,
    kSl_RKindMismatch = 9,

    kSl_PeerPresent = 0x01,
    kSl_PeerRelayOnly = 0x02,
    kSl_PathRelay = 0,
    kSl_PathDirect = 1,

    kSl_CodeLen = 6,
    kSl_JoinFixed = 33,
    kSl_WelcomeLen = 30,
    kSl_RefuseLen = 9,
    kSl_DataHdr = 12,
    kSl_PeerLen = 35,
    kSl_PingLen = 20,
    kSl_PunchLen = 17,
    kSl_ByeLen = 12,
    kSl_MaxTicket = 1024,
    kSl_CwlMin = 4,
    kSl_CwlMax = 64,
};

// IPv4 endpoint, host byte order (ip 0x0A000007 = 10.0.0.7).
typedef struct {
    uint32_t ip;
    uint16_t port;
} SlEp_t;

typedef struct {
    uint32_t nonce;
    uint64_t token;
    uint8_t slot, max_peers;
    char code[kSl_CodeLen + 1];
    SlEp_t observed;
} SlWelcome_t;

typedef struct {
    uint64_t token;
    uint8_t gen, slot, flags;
    uint64_t p2p_key;
    SlEp_t pub, local;
} SlPeer_t;

typedef struct {
    uint64_t token;
    uint32_t stamp;
    uint8_t gen, path;
    uint16_t rtt_ms;
} SlPing_t;

// -1 when b is not a v1 envelope, else its type.
int Sl_Type(const uint8_t *b, size_t n);
bool Sl_IsCwlPayload(const uint8_t *p, size_t n);
size_t Sl_EncodeJoin(uint8_t *out, size_t cap, uint32_t nonce, uint8_t mode, const char *code, uint64_t client_id,
                     SlEp_t local, const uint8_t *ticket, size_t ticket_len);
size_t Sl_EncodeData(uint8_t *out, size_t cap, uint64_t token, const uint8_t *payload, size_t n);
size_t Sl_EncodePing(uint8_t *out, uint8_t type, const SlPing_t *p);
size_t Sl_EncodePunch(uint8_t *out, uint8_t type, uint64_t key, uint8_t slot, uint32_t stamp);
size_t Sl_EncodeBye(uint8_t *out, uint64_t token);
bool Sl_DecodeWelcome(const uint8_t *b, size_t n, SlWelcome_t *w);
bool Sl_DecodeRefuse(const uint8_t *b, size_t n, uint32_t *nonce, uint8_t *reason);
bool Sl_DecodeData(const uint8_t *b, size_t n, uint64_t *token, const uint8_t **payload, size_t *plen);
bool Sl_DecodePeer(const uint8_t *b, size_t n, SlPeer_t *p);
bool Sl_DecodePing(const uint8_t *b, size_t n, SlPing_t *p);
bool Sl_DecodePunch(const uint8_t *b, size_t n, uint64_t *key, uint8_t *slot, uint32_t *stamp);
// Room codes: the relay's speakable alphabet (BCDFGHJKMNPQRSTVWXZ23456789).
extern const char kSl_CodeAlphabet[];
bool Sl_ValidCode(const char *code);

// ---------------------------------------------------------------- client

typedef enum {
    kRelay_Idle,         // not on the relay (no session wanted, or gave up)
    kRelay_NeedTicket,   // wants to JOIN, has no usable ticket yet
    kRelay_Joining,      // JOIN sent, waiting for WELCOME / REFUSE
    kRelay_Seated,       // has a seat (token)
} RelayState_t;

typedef enum {
    kRelayEv_Seated,       // arg: slot; code in RelayClient_t.code
    kRelayEv_Refused,      // arg: reason (a terminal refusal: no_room, room_full, kind_mismatch)
    kRelayEv_NeedTicket,   // ask the backend for a ticket (Relay_SetTicket)
    kRelayEv_PeerPresent,  // arg: 1 relay only
    kRelayEv_PeerAbsent,
    kRelayEv_DirectUp,     // arg: direct rtt ms
    kRelayEv_DirectDown,
    kRelayEv_Rejoin,       // the relay forgot our seat: rejoining
} RelayEvent_t;

typedef struct {
    void *ctx;
    void (*send)(void *ctx, SlEp_t to, const uint8_t *buf, size_t n);
    // A CWL payload from the partner (relay or direct path).
    void (*deliver)(void *ctx, const uint8_t *payload, size_t n, bool direct);
    void (*event)(void *ctx, RelayEvent_t ev, int arg);
    uint32_t (*rand32)(void *ctx);
} RelayHost_t;

typedef struct {
    uint32_t tx_relay, tx_direct, rx_relay, rx_direct;
    uint32_t joins_sent, welcomes, refusals, rejoins, direct_ups, direct_downs;
    uint32_t punches_sent, pings_sent, dropped_no_peer, unknown;
    uint8_t last_refuse;
} RelayStats_t;

enum {
    kRelay_JoinResend_ms = 250,
    kRelay_JoinFast_ms = 3000,     // then resend once a second
    kRelay_TicketMaxAge_ms = 100000, // tickets live 120 s
    kRelay_Punch_ms = 50,
    kRelay_PunchFor_ms = 5000,
    kRelay_DirectDead_ms = 3000,
    kRelay_DirectPing_ms = 500,
    kRelay_PingFull_ms = 2000,
    kRelay_PingLow_ms = 22000,
    kRelay_RateRetry_ms = 2000,
};

typedef struct {
    RelayHost_t host;
    RelayState_t state;
    // session wanted
    SlEp_t relay;
    uint8_t mode, orig_mode;      // mode of the next JOIN; the one asked for
    char code[kSl_CodeLen + 1];   // join code (asked), then the room's code once seated
    uint64_t client_id;
    SlEp_t local;
    bool p2p;                     // false: "force relay" (the shim's --no-p2p)
    uint32_t relay_ping_ms;       // 2 s at full power, ~22 s in low power
    // ticket (single use)
    uint8_t ticket[kSl_MaxTicket];
    size_t ticket_len;
    uint32_t ticket_ms;
    // joining
    uint32_t nonce;
    uint32_t join_start_ms, join_last_ms, retry_at_ms;
    bool retry_pending;
    // seat
    uint64_t token;
    uint8_t slot, gen;
    uint64_t p2p_key;
    bool peer_present, peer_relay_only;
    bool have_pub, have_local;
    SlEp_t peer_pub, peer_local;
    bool punching;
    uint32_t punch_until_ms, last_punch_ms;
    bool have_direct;
    SlEp_t direct;
    uint32_t direct_heard_ms;
    uint16_t direct_rtt_ms, relay_rtt_ms;
    uint32_t last_relay_ping_ms, last_direct_ping_ms, last_relay_heard_ms;
    uint32_t epoch_ms;            // stamps are ms since this
    RelayStats_t stats;
} RelayClient_t;

void Relay_Init(RelayClient_t *r, const RelayHost_t *host, uint32_t now_ms);
// Hand over a fresh ticket (from POST /link_device/ticket). Single use.
void Relay_SetTicket(RelayClient_t *r, const char *ticket, size_t len, uint32_t now_ms);
bool Relay_HasTicket(const RelayClient_t *r, uint32_t now_ms);
// Take (or retake: same client_id) a seat. mode kSl_Mode*, code for kSl_ModeJoin.
void Relay_Start(RelayClient_t *r, SlEp_t relay, uint8_t mode, const char *code, uint64_t client_id, SlEp_t local,
                 uint32_t now_ms);
// Our LAN endpoint changed (new DHCP lease): used by the next JOIN.
void Relay_SetLocal(RelayClient_t *r, SlEp_t local);
void Relay_SetP2p(RelayClient_t *r, bool on);
void Relay_SetLowPower(RelayClient_t *r, bool low);
// After a radio-off spell: ping the relay at once with the old token (the
// relay rebinds our new address and re-brokers), re-punch if we had a direct
// path. A forgotten seat comes back as REFUSE unknown_session -> rejoin.
void Relay_Resume(RelayClient_t *r, uint32_t now_ms);
// Leave cleanly (BYE) and go idle.
void Relay_Stop(RelayClient_t *r);
// Returns true when buf was a Shiny Link envelope (consumed, whatever it was).
bool Relay_OnDatagram(RelayClient_t *r, SlEp_t from, const uint8_t *buf, size_t n, uint32_t now_ms);
void Relay_Poll(RelayClient_t *r, uint32_t now_ms);
// Send one CWL payload to the partner. False when there is no seat or partner.
bool Relay_Send(RelayClient_t *r, const uint8_t *payload, size_t n, uint32_t now_ms);
bool Relay_DirectAlive(const RelayClient_t *r, uint32_t now_ms);
bool Relay_Seated(const RelayClient_t *r);
bool Relay_PeerPresent(const RelayClient_t *r);
const char *Relay_RefuseName(uint8_t reason);

#ifdef __cplusplus
}
#endif
