/* nw_proto.h: network window v1 constants (NETWORK_WINDOW.md).
 *
 * Plain #defines only, so the same header serves the ESP32 service, the Mac
 * reference model / tests, an emulator hook and GBDK game code.
 */
#ifndef NW_PROTO_H
#define NW_PROTO_H

/* ---- GB registers (CPU addresses) ---- */
#define NW_REG_ID     0xFF58u  /* R: 0x4E once unlocked; W: key 'S','G','N','W', 0x00 = lock */
#define NW_REG_VER    0xFF59u  /* R: 0x10 */
#define NW_REG_CMD    0xFF5Au  /* W: command; R: NW_ERR */
#define NW_REG_STAT   0xFF5Bu
#define NW_REG_PTRL   0xFF5Cu
#define NW_REG_PTRH   0xFF5Du
#define NW_REG_DATA   0xFF5Eu
#define NW_REG_RSVD   0xFF5Fu  /* reserved, reads 0xFF */

#define NW_ID_MAGIC   0x4Eu
#define NW_VERSION    0x10u    /* hi nibble major, lo nibble minor */
#define NW_KEY0 0x53u
#define NW_KEY1 0x47u
#define NW_KEY2 0x4Eu
#define NW_KEY3 0x57u

#define NW_TX_SIZE    2048u
#define NW_RX_SIZE    4096u

/* NW_CMD */
#define NW_CMD_CLR_ERR 0x00u
#define NW_CMD_SEND    0x01u
#define NW_CMD_RX_ACK  0x02u
#define NW_CMD_ABORT   0x03u

/* NW_STAT bits */
#define NW_ST_BUSY     0x01u
#define NW_ST_RX_READY 0x02u
#define NW_ST_ERR      0x04u
#define NW_ST_SVC      0x08u
#define NW_ST_ONLINE   0x10u
#define NW_ST_NOTIFY   0x20u

/* NW_ERR (sticky: first error wins until CLR_ERR) */
#define NW_E_NONE        0u
#define NW_E_BUSY        1u
#define NW_E_NOSVC       2u
#define NW_E_TX_OVERRUN  3u
#define NW_E_RX_UNDERRUN 4u
#define NW_E_BADLEN      5u
#define NW_E_BADCMD      6u
#define NW_E_NOT_READY   7u
#define NW_E_SVC_LOST    8u

/* ---- FPGA <-> ESP32 transport (system monitor UART, 0x8F packets) ---- */
#define NW_UART_ADDR_TO_FPGA 0x11u  /* ESP32 -> FPGA */
#define NW_UART_ADDR_TO_ESP  0x0Bu  /* FPGA -> ESP32 (TX channel 11) */

/* ESP32 -> FPGA payload[0] */
#define NW_R_CONFIG 1u  /* [1, flags]                         */
#define NW_R_RXDATA 2u  /* [2, id, off16, data...]            */
#define NW_R_RXPOST 3u  /* [3, id, len16]                     */
#define NW_R_TXREAD 4u  /* [4, id, off16, cnt16]              */
#define NW_R_STATUS 5u  /* [5]                                */
/* CONFIG flags */
#define NW_CFG_SVC    0x01u
#define NW_CFG_ONLINE 0x02u
#define NW_CFG_NOTIFY 0x04u
#define NW_CFG_AWAKE  0x08u

/* FPGA -> ESP32 payload: [type, id, a16, ext...] */
#define NW_T_COMMIT 1u  /* a = request length                 */
#define NW_T_TXDATA 2u  /* a = offset, data follows           */
#define NW_T_ABORT  3u
#define NW_T_RXACK  4u
#define NW_T_POSTED 5u  /* a = posted length                  */
#define NW_T_RXNAK  6u  /* a = bytes the FPGA holds (resend from here) */
#define NW_T_STATE  7u  /* a = flags | version << 8, ext = act_len16, rx_have16 */
/* STATE flags */
#define NW_SF_UNLOCKED  0x01u
#define NW_SF_ACTIVE    0x02u
#define NW_SF_POSTED    0x04u
#define NW_SF_STREAMING 0x08u

#define NW_RXDATA_MAX 240u   /* data bytes per RXDATA packet (packet length <= 255) */

/* ---- messages inside the buffers ---- */
#define NW_MSG_VER 1u
/* request: [ver, op, flags, 0, body...] */
#define NW_REQ_HDR 4u
#define NW_OP_INFO         0x01u
#define NW_OP_IDENTIFY     0x02u  /* body: game_id[16], version u16, title (NUL) */
#define NW_OP_GET          0x03u  /* body: path (NUL)                            */
#define NW_OP_POST         0x04u  /* body: path (NUL), content                   */
#define NW_OP_SERVICE      0x05u  /* body: service u8, opaque                    */
#define NW_OP_UPDATE_CHECK 0x06u  /* body: (none; uses the IDENTIFY game id/ver) */
#define NW_OP_BLOB_READ    0x07u  /* body: handle u8, offset u32, max u16        */
#define NW_OP_POLL         0x08u  /* body: none -> pending notifications         */
#define NW_OP_LB_SUBMIT    0x09u  /* body: board u8, score u32, name[8]          */
#define NW_OP_LB_TOP       0x0Au  /* body: board u8, start u16, count u8         */

/* response: [ver, status, handle, flags, body_len u16, total_len u32, body...] */
#define NW_RESP_HDR 10u
#define NW_RF_MORE 0x01u   /* total_len > body_len: read the rest with BLOB_READ(handle) */

#define NW_S_OK              0u
#define NW_S_BAD_REQUEST     1u
#define NW_S_NOT_IDENTIFIED  2u
#define NW_S_DENIED          3u
#define NW_S_NOT_FOUND       4u
#define NW_S_OFFLINE         5u
#define NW_S_TIMEOUT         6u
#define NW_S_SERVER_ERROR    7u
#define NW_S_TOO_LARGE       8u
#define NW_S_RATE_LIMITED    9u
#define NW_S_UNSUPPORTED    10u

#endif
