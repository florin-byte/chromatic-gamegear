// cart_xs.v: the express cart stream (lane cartspeed1, FPGA 18.26).
//
// Why: a cart install moved every byte over the 115,200-baud ESP32 -> FPGA UART into the network
// window, and the Game Boy CPU fed it to the cart bus engine one byte per register write, with one
// CF_NEXT round trip (stop-and-wait) per 2 KiB. About 10 KB/s. The cart's flash itself programs a
// byte in ~7 us (SST39VF168x tBP typ). This module takes the GB CPU and the UART out of the data
// path:
//
//   data:     ESP32 --QSPI (40 MHz, the OSD bus)--> ring (4 KiB BSRAM) --> engine --> cart flash
//   control:  ESP32 --UART address 0x12--> op queue / posts      (a few bytes per KiB)
//   status:   FPGA  --UART address 0x0C (system monitor channel 12)--> ESP32
//
// The ESP32 writes cart data into the ring with QSPI transactions whose address has bit 31 set
// (bits RING_AW-1:0 = the ring offset); mm_burst_write ignores those (no PSRAM write). It then
// POSTs the range (end position, length, crc32) over the UART. The FPGA checks the crc32 of the
// bytes in the ring before it accepts them (`valid` moves on), else it refuses the post (the ESP32
// writes the range again). A PROG op takes its bytes from the ring in stream order (`consumed`).
// The ESP32 only ever writes ring bytes that are not in [consumed, consumed + RING) of data still
// to be programmed, so a range is never changed while it is checked or read.
//
// Ops are queued (QDEPTH) and run back to back: no stop-and-wait between them. Every op runs
// through the cart bus engine's express port (cart_engine.v x_bus_*), one bus cycle at a time with
// the engine's timing; while an op runs the engine starts nothing of its own (x_hold), so no cycle
// ever lands between the cycles of a flash command. Ops (flash address F, 24 bits; MBC5 banking:
// bank = F[22:14] written to 0x2000 / 0x3000 when it changes, the byte at 0x4000 + F[13:0]):
//   1 ERASE  F, arg = 30 (64 KiB block) / 50 (4 KiB sector) / 10 (chip): AA 55 80 AA 55 arg, then
//            DQ6 toggle polling until it stops (timeout ~4 s)
//   2 PROG   F, n: n bytes from the ring. FF only steps F (an erased cell). Else AA 55 A0 byte,
//            DQ6 polling (timeout ~500 us), the last poll read must equal the byte (verify)
//   3 CRC    F, n: reads n bytes: crc32 (zlib) and "all FF" (the blank check)
//   4 CFG    bus timing (hclk counts: setup, pulse, hold, access, gap) and flags b0 = session
//   5 RAMW   cart RAM offset F (bank F[14:13]), n bytes from the ring (0A at 0000 first, the bank at 4000)
//   6 RAMCRC cart RAM offset F, n bytes: crc32 and "all FF"
//   Shiny Relay 1.1.0 (FPGA 18.30), the FPGA's own boot flash (4 MiB SPI NOR on the MSPI pins, x1 at
//   hclk/2) and the RAM cart (ramcart.v). Flash address F; nothing below 0x100000 is ever erased or
//   programmed but the jump headers (FHDR, inside 0x000-0x3FF, never erased but by FHDRERASE):
//  16 FID    the JEDEC id (9F) -> the crc field
//  17 FERASE F, arg = 20 (4 KiB) / D8 (64 KiB), F >= 0x100000: WREN, erase, status polling (~4 s max)
//  18 FPROG  F, n: n bytes from the ring, F >= 0x100000, F + n <= 0x400000: page programs (256 B)
//  19 FCRC   F, n: crc32 + "all FF" of n flash bytes (anywhere)
//  20 FHDR   F, n: n ring bytes into the header area (F + n <= 0x400), FF bytes left alone
//  21 FHDRERASE arg = 5A: erase sector 0 (all headers: the one unsafe step, used only when it is full; 18.31: refused)
//  22 FREAD  F, n = 2..1024 (even): n flash bytes into the flash read window (QSPI read, as RREAD's), crc32 + "all FF"
//            of them in the crc field (18.31). The window the QSPI read sees is the one the last FREAD / RREAD filled.
//   FREAD / RREAD arg bit 0 = TAGGED (18.31): the bytes go into the window from word 1 (n <= 1022) and the window's word 0
//   reads {seq, 00} of the op once its bytes are all there ({00, 00} meanwhile): the ESP32 can read the window without
//   waiting for the status (seqs are never 00)
//  23 FCOPY  F, n (even, F + n <= 4 MiB): n flash bytes into PSRAM from the address FCDST set (it moves on by n), one
//            16-bit word at a time through the RAM cart's PSRAM port (only while the RAM cart is off); crc32 of the bytes in
//            the crc field (18.31, the full design only: HAS_FCOPY)
//  24 FCDST  F = the PSRAM byte address FCOPY writes to next (even, 0x040000 and up: not the frame buffers)
//   18.31 guards (parameters): FERASE / FPROG at FLASH_W_BASE (0x05C000) and up, FHDR below FLASH_H_TOP (0x7C0).
//  32 RCFG   F = {ram_mask[19:16], rom_mask[15:8], b4 rtc, b3 ram, b2:0 mbc}, arg: b0 RAM cart on,
//            b1 hold the GB core in reset, b2 drop the cache, b3 clear the save RAM dirty flag, b4 MRESET (mapper reset)
//  33 RREAD  F = PSRAM byte address (even), n = 2..1024 (even, one 1 KiB row): into the read window
//            (QSPI read, qspi_slave.v)
//  34 RRTC   arg 0: the clock {DH,DL,H,M,S} -> crc field = bytes DL H M S... (crc[31:0] = rtc[31:0],
//            the flash address field = rtc[39:32]); arg 1: set it from {n[15:0], F[23:0]}
// Every error stops the op, empties the queue and is sticky until RESET; the status names the op
// that last finished, the error and the flash address it happened at.
//
// ESP32 -> FPGA, UART address 0x12, payload:
//   [1 OP, seq, op, F0, F1, F2, n0, n1, n2, arg]             (CFG: F0..F2 = setup pulse hold,
//                                                              n0 n1 = access gap, n2 = flags)
//   [2 POST, e0..e3 (end position u32, bits 23:0 used), l0 l1 (length), c0..c3 (crc32 of the bytes)]
//   [3 ABORT]   the running op stops at the next byte (an erase: when the chip is ready), queue cleared
//   [4 STATUS]  send a status now
//   [5 RESET]   (when idle) positions to 0, error cleared, queue cleared
// FPGA -> ESP32, address 0x0C, 21 bytes:
//   [0] 1  [1] flags: b0 session on, b1 running, b2 error, b3 allow, b4 starved, b5 post refused,
//          b6 checking a post, b7 x_session  [2] seq of the last op finished  [3] error code
//   [4..6] consumed  [7] RAM cart: b2:0 PSRAM density (MR2), b3 busy, b4 save RAM dirty, b5 on, b6 hold
//   [8..10] valid  [11] b0 a cartridge is in the slot (CART_DET), b1 RAM cart session allowed  [12..15] crc32 of the last CRC op  [16..18] flash address
//   [19] b3:0 ops queued, b6:4 posts refused (count mod 8: the ESP32 rewinds to `valid` once per
//        refusal, however many statuses repeat it), b7 the last CRC op read all FF
//   [20] seq of the op running / last started
//   [21..24] hclk spent waiting for programs (from each data cycle's end to the read that showed the
//            byte ready), all bytes since RESET  [25..28] hclk the last erase was busy
//   [29..31] bytes programmed since RESET (FF skipped not counted): the chip's own times, measured
// Posts need not wait for each other: a 1 KiB check takes ~0.1 ms, a post packet ~1.3 ms on the line.
// A status goes out whenever something changes (an op starts or ends, a post is refused,
// another KiB is consumed, an error) and on STATUS; changes that come while one waits are merged.
//
// Clock domains: QSPI clock (ring write port, inverted clock as mm_burst_write's FIFO), gclk (UART
// sniffer + TX channel), hclk (everything else, with the engine). gclk -> hclk: a payload register
// set written in the same clock as its toggle flips, held until the next packet of that type
// (>= 0.8 ms later); hclk -> gclk: status snapshot by request / ack toggles. Toggles are never
// reset, so a reset on one side cannot fake an edge.
module cart_xs #(
    parameter integer RING_AW     = 12,             // ring 4 KiB
    parameter [6:0]   RX_ADDR     = 7'h12,
    parameter integer QD_AW       = 1,              // 2 queued ops (+ the one running: always one waiting)
    parameter integer PROG_TMO    = 8400,           // ~500 us (hclk)
    parameter integer ERASE_TMO   = 67000000,       // ~4 s
    parameter integer STARVE_TMO  = 67000000,       // ~4 s without the next byte: error
    parameter integer STARVE_FLAG = 1048576,        // ~62 ms waiting: the starved flag (reported once)
    // the boot flash guards (18.31 layout, fpgacomp1): FERASE / FPROG only at FLASH_W_BASE and up (an erase by the start
    // of the 4 KiB sector / 64 KiB block it erases), FHDR only below FLASH_H_TOP, FHDRERASE only if HDRERASE_OK. The 18.30
    // values (0x100000 / 0x400 / 1) are the defaults; 18.31 and its recovery design: 0x05C000 / 0x7C0 / 0 = the recovery
    // design (0x000800-0x05BFFF) and its header (0x7C0) can never be changed from the ESP32 side, only by a USB install.
    parameter [23:0]  FLASH_W_BASE = 24'h100000,
    parameter [23:0]  FLASH_H_TOP  = 24'h000400,
    parameter         HDRERASE_OK  = 1'b1,
    parameter         HAS_FCOPY    = 1'b0          // FCOPY / FCDST (boot flash -> PSRAM through the RAM cart's port)
) (
    // ---------------- QSPI (ESP32 -> FPGA), from QSPI_Slave ----------------
    input                    qspi_clk_n,
    input                    q_addr_ready,    // one QSPI clock after the address is complete
    input            [31:0]  q_address,
    input                    q_byte_valid,    // one QSPI clock per data byte
    input             [7:0]  q_byte,
    // ---------------- gclk: UART ----------------
    input                    gclk,
    input                    g_reset,
    input             [7:0]  g_uart_rx_data,
    input                    g_uart_rx_val,
    output reg               g_tx_valid = 1'b0,
    output            [7:0]  g_tx_len,
    input             [7:0]  g_tx_bytepos,
    output            [7:0]  g_tx_byte,
    input                    g_tx_taken,
    // ---------------- hclk: the engine's express port ----------------
    input                    hclk,
    input                    h_reset,         // GB reset: the session ends
    input                    h_allow,         // link cmd 9 ENGINE_ALLOW
    input                    h_eng_busy,      // a CPU-side engine op runs / waits
    output reg               x_session = 1'b0,
    output                   x_hold,
    output reg               x_bus_req = 1'b0,
    output reg               x_bus_wr = 1'b0,
    output reg       [15:0]  x_bus_a = 16'h0000,
    output reg        [7:0]  x_bus_d = 8'h00,
    input                    x_bus_done,
    input             [7:0]  x_bus_q,
    output reg               x_tcfg = 1'b0,
    output reg        [4:0]  x_t_setup = 5'd0, x_t_pulse = 5'd0, x_t_hold = 5'd0, x_t_access = 5'd0, x_t_gap = 5'd0,
    // ---------------- hclk: the boot flash (SPI x1, mode 0) ----------------
    output reg               f_sck = 1'b0,
    output reg               f_cs_n = 1'b1,
    output reg               f_mosi = 1'b0,
    input                    f_miso,
    // ---------------- hclk: the RAM cart (ramcart.v) ----------------
    output reg               r_on = 1'b0,
    output reg               r_hold = 1'b0,
    output reg        [2:0]  r_mbc = 3'd0,
    output reg        [7:0]  r_rom_mask = 8'd0,
    output reg        [3:0]  r_ram_mask = 4'd0,
    output reg               r_ram_has = 1'b0,
    output reg               r_rtc_has = 1'b0,
    output reg               r_inval_tog = 1'b0,
    output reg               r_mreset_tog = 1'b0,
    output reg               r_dirty_clr = 1'b0,
    input                    r_dirty,
    input                    r_busy,
    output reg               r_wread_tog = 1'b0,
    output reg       [22:0]  r_wread_addr = 23'd0,
    output reg       [10:0]  r_wread_len = 11'd0,
    output reg               r_wread_off = 1'b0,   // 18.31: a tagged RREAD fills the window from word 1
    input                    r_wread_done_tog,
    output reg               r_rtc_set = 1'b0,
    output reg       [39:0]  r_rtc_in = 40'd0,
    input            [39:0]  r_rtc_out,
    input            [47:0]  r_dbg,
    input             [2:0]  r_density,
    input                    h_cart_in,
    // ---------------- 18.31: the flash read window (FREAD), read like the RAM cart's (QSPI read, qspi_slave.v) ----------------
    input             [8:0]  q_win_addr,      // 16-bit words, QSPI clock (inverted, as qspi_clk_n)
    output reg       [15:0]  q_fwin = 16'd0,
    output reg               h_fwin_sel = 1'b0, // 1: the last window op was FREAD (the top's window mux), 0: RREAD
    // 18.31 window tag (FREAD / RREAD with arg bit 0): the window's word 0 reads {the op's seq, 00} once the op is done
    output reg               h_wtag_on = 1'b0,
    output            [15:0] h_wtag,
    // ---------------- 18.31: FCOPY's PSRAM writes, one word at a time through the RAM cart (ramcart.v, hclk) ----------------
    input                    r_cw_ready,      // the RAM cart's PSRAM port is free (and the RAM cart is off)
    output reg               r_cw_go = 1'b0,  // pulse: write r_cw_data at r_cw_addr
    output reg       [22:0]  r_cw_addr = 23'd0,
    output reg       [15:0]  r_cw_data = 16'd0
);
    localparam RING = 1 << RING_AW;
    localparam QD   = 1 << QD_AW;

    localparam [7:0] E_NONE = 8'd0, E_PTMO = 8'd1, E_VERIFY = 8'd2, E_ETMO = 8'd3, E_STARVE = 8'd4,
                     E_QFULL = 8'd5, E_BADOP = 8'd6, E_NOSESS = 8'd7, E_ABORT = 8'd8;

    function [31:0] crc32_byte;   // zlib crc32 step (reflected, poly EDB88320)
        input [31:0] c; input [7:0] d;
        integer k; reg [31:0] x;
        begin
            x = c ^ {24'd0, d};
            for (k = 0; k < 8; k = k + 1) x = x[0] ? ((x >> 1) ^ 32'hEDB88320) : (x >> 1);
            crc32_byte = x;
        end
    endfunction
    function [7:0] crc8;          // CRC-8 SAE J1850 step (poly 0x1D), as uart_packet_wrapper_*
        input [7:0] c; input [7:0] d;
        integer k; reg [7:0] x;
        begin
            x = c ^ d;
            for (k = 0; k < 8; k = k + 1) x = x[7] ? ({x[6:0], 1'b0} ^ 8'h1D) : {x[6:0], 1'b0};
            crc8 = x;
        end
    endfunction

    // =====================================================================
    // QSPI side: the ring's write port
    // =====================================================================
    reg [RING_AW-1:0] q_wa = 0;
    reg               q_on = 1'b0;
    always @(posedge qspi_clk_n) begin
        if (q_addr_ready) begin
            q_wa <= q_address[RING_AW-1:0];
            q_on <= q_address[31];
        end else if (q_byte_valid && q_on)
            q_wa <= q_wa + 1'b1;
    end
    wire               ring_we = q_byte_valid & q_on & ~q_addr_ready;
    wire [RING_AW-1:0] ring_ra;            // combinational: the BSRAM registers it (data one clock later)
    wire         [7:0] ring_q;
    net_window_dpram #(.AW(RING_AW)) u_ring (
        .wclk(qspi_clk_n), .we(ring_we), .waddr(q_wa), .wdata(q_byte),
        .rclk(hclk), .raddr(ring_ra), .rdata(ring_q));

    // =====================================================================
    // gclk side: UART sniffer (address 0x12) and the status channel
    // =====================================================================
    // payload slots (written with their toggle, held until the next packet of the type)
    reg        g_op_tog = 1'b0, g_post_tog = 1'b0, g_abort_tog = 1'b0, g_reset_tog = 1'b0;
    reg  [7:0] g_op_seq = 8'd0, g_op_op = 8'd0, g_op_arg = 8'd0;
    reg [23:0] g_op_f = 24'd0, g_op_n = 24'd0;
    reg [23:0] g_post_end = 24'd0;
    reg [31:0] g_post_crc = 32'd0;
    reg [15:0] g_post_len = 16'd0;
    // status snapshot handshake
    reg        g_snap_req_tog = 1'b0;
    reg        h_snap_ack_tog = 1'b0;
    reg  [2:0] g_ack_sync = 3'b000;
    reg        g_ack_seen = 1'b0;
    reg        h_chg_tog = 1'b0;
    reg  [2:0] g_chg_sync = 3'b000;
    reg        g_chg_seen = 1'b0;
    reg        g_pend = 1'b0, g_wait_ack = 1'b0;
    // the snapshot (hclk writes it on a request; gclk reads it after the ack)
    reg  [7:0] s_flags = 8'd0, s_done_seq = 8'd0, s_err = 8'd0, s_q = 8'd0, s_run_seq = 8'd0;
    reg [23:0] s_cons = 24'd0, s_valid = 24'd0;
    reg [31:0] s_crc = 32'd0;
    reg [31:0] s_pbusy = 32'd0, s_ebusy = 32'd0;
    reg [23:0] s_nprog = 24'd0;
    reg [23:0] s_f = 24'd0;
    reg  [7:0] s_rc = 8'd0, s_slot = 8'd0;

    localparam RS_IDLE = 3'd0, RS_ADDR = 3'd1, RS_LEN = 3'd2, RS_DATA = 3'd3, RS_CRC = 3'd4;
    reg  [2:0] rs = RS_IDLE;
    reg  [7:0] rs_crc = 8'hFF, rs_len = 8'd0, rs_i = 8'd0;
    reg        rs_mine = 1'b0;
    reg  [7:0] p [0:11];
    integer pi;
    initial for (pi = 0; pi < 12; pi = pi + 1) p[pi] = 8'd0;

    assign g_tx_len = 8'd32;
    reg [7:0] txb;
    always @(*) begin
        case (g_tx_bytepos)
            8'd0:  txb = 8'd1;
            8'd1:  txb = s_flags;
            8'd2:  txb = s_done_seq;
            8'd3:  txb = s_err;
            8'd4:  txb = s_cons[7:0];
            8'd5:  txb = s_cons[15:8];
            8'd6:  txb = s_cons[23:16];
            8'd7:  txb = s_rc;
            8'd8:  txb = s_valid[7:0];
            8'd9:  txb = s_valid[15:8];
            8'd10: txb = s_valid[23:16];
            8'd11: txb = s_slot;
            8'd12: txb = s_crc[7:0];
            8'd13: txb = s_crc[15:8];
            8'd14: txb = s_crc[23:16];
            8'd15: txb = s_crc[31:24];
            8'd16: txb = s_f[7:0];
            8'd17: txb = s_f[15:8];
            8'd18: txb = s_f[23:16];
            8'd19: txb = s_q;
            8'd20: txb = s_run_seq;
            8'd21: txb = s_pbusy[7:0];
            8'd22: txb = s_pbusy[15:8];
            8'd23: txb = s_pbusy[23:16];
            8'd24: txb = s_pbusy[31:24];
            8'd25: txb = s_ebusy[7:0];
            8'd26: txb = s_ebusy[15:8];
            8'd27: txb = s_ebusy[23:16];
            8'd28: txb = s_ebusy[31:24];
            8'd29: txb = s_nprog[7:0];
            8'd30: txb = s_nprog[15:8];
            8'd31: txb = s_nprog[23:16];
            default: txb = 8'd0;
        endcase
    end
    assign g_tx_byte = txb;

    always @(posedge gclk) begin
        g_ack_sync <= {g_ack_sync[1:0], h_snap_ack_tog};
        g_chg_sync <= {g_chg_sync[1:0], h_chg_tog};
        if (g_reset) begin
            rs <= RS_IDLE; g_tx_valid <= 1'b0; g_pend <= 1'b0; g_wait_ack <= 1'b0;
            g_ack_seen <= g_ack_sync[2]; g_chg_seen <= g_chg_sync[2];
        end else begin
            if (g_chg_sync[2] != g_chg_seen) begin g_chg_seen <= g_chg_sync[2]; g_pend <= 1'b1; end
            // ---------------- UART RX sniffer ----------------
            if (g_uart_rx_val) begin
                case (rs)
                RS_IDLE: if (g_uart_rx_data == 8'h8F) begin rs <= RS_ADDR; rs_crc <= crc8(8'hFF, 8'h8F); end
                RS_ADDR: begin
                    rs <= RS_LEN; rs_crc <= crc8(rs_crc, g_uart_rx_data);
                    rs_mine <= (g_uart_rx_data[6:0] == RX_ADDR);
                end
                RS_LEN: begin
                    rs_crc <= crc8(rs_crc, g_uart_rx_data);
                    rs_len <= g_uart_rx_data; rs_i <= 8'd0;
                    rs <= (g_uart_rx_data == 8'd0) ? RS_CRC : RS_DATA;
                end
                RS_DATA: begin
                    rs_crc <= crc8(rs_crc, g_uart_rx_data);
                    rs_i <= rs_i + 8'd1;
                    if (rs_i + 8'd1 == rs_len) rs <= RS_CRC;
                    if (rs_mine && rs_i < 8'd12) p[rs_i[3:0]] <= g_uart_rx_data;
                end
                RS_CRC: begin
                    rs <= RS_IDLE;
                    if (rs_mine && rs_crc == g_uart_rx_data) begin
                        case (p[0])
                        8'd1: if (rs_len >= 8'd10) begin
                            g_op_seq <= p[1]; g_op_op <= p[2];
                            g_op_f <= {p[5], p[4], p[3]}; g_op_n <= {p[8], p[7], p[6]}; g_op_arg <= p[9];
                            g_op_tog <= ~g_op_tog;
                        end
                        8'd2: if (rs_len >= 8'd11) begin
                            g_post_end <= {p[3], p[2], p[1]}; g_post_len <= {p[6], p[5]};
                            g_post_crc <= {p[10], p[9], p[8], p[7]};
                            g_post_tog <= ~g_post_tog;
                        end
                        8'd3: g_abort_tog <= ~g_abort_tog;
                        8'd4: g_pend <= 1'b1;
                        8'd5: g_reset_tog <= ~g_reset_tog;
                        default: ;
                        endcase
                    end
                end
                default: rs <= RS_IDLE;
                endcase
            end
            // ---------------- status: snapshot, then the packet ----------------
            if (g_tx_valid) begin
                if (g_tx_taken) g_tx_valid <= 1'b0;
            end else if (g_wait_ack) begin
                if (g_ack_sync[2] != g_ack_seen) begin
                    g_ack_seen <= g_ack_sync[2];
                    g_wait_ack <= 1'b0;
                    g_tx_valid <= 1'b1;
                end
            end else if (g_pend) begin
                g_pend <= 1'b0;
                g_wait_ack <= 1'b1;
                g_snap_req_tog <= ~g_snap_req_tog;
            end
        end
    end

    // =====================================================================
    // hclk side
    // =====================================================================
    reg  [2:0] h_op_sync = 3'b000, h_post_sync = 3'b000, h_abort_sync = 3'b000, h_reset_sync = 3'b000, h_snap_sync = 3'b000;
    reg        h_op_seen = 1'b0, h_post_seen = 1'b0, h_abort_seen = 1'b0, h_reset_seen = 1'b0, h_snap_seen = 1'b0;
    reg        h_init = 1'b0;

    // op queue
    reg  [7:0] qseq [0:QD-1];
    reg  [7:0] qop  [0:QD-1];
    reg [23:0] qf   [0:QD-1];
    reg [23:0] qn   [0:QD-1];
    reg  [7:0] qarg [0:QD-1];
    reg  [QD_AW:0]  q_head = 0, q_tail = 0;   // with a wrap bit: ops queued = q_tail - q_head
    wire [QD_AW:0]  q_count = q_tail - q_head;
    wire            q_flush_now;              // ABORT / RESET arriving this clock

    // ring positions, fetch, post check
    reg [23:0] cons = 24'd0, valid = 24'd0;      // stream positions: 24 bits (a job's stream is < 16 MiB; RESET zeroes them)
    reg        fb_ok = 1'b0, f_pend = 1'b0;
    reg  [7:0] fb = 8'd0;
    reg        c_busy = 1'b0, c_pend = 1'b0, c_last = 1'b0;
    reg [23:0] c_pos = 24'd0, c_end = 24'd0;
    reg [31:0] c_crcw = 32'd0, c_crc = 32'hFFFFFFFF;
    reg        post_nak = 1'b0;
    reg  [2:0] post_naks = 3'd0;

    // the sequencer
    localparam S_IDLE = 5'd0, S_BANK = 5'd1, S_CMD = 5'd2, S_POLL = 5'd3, S_PNEXT = 5'd4, S_CNEXT = 5'd5,
               S_CRD = 5'd6, S_EDONE = 5'd7, S_ERR = 5'd8, S_RNEXT = 5'd9, S_RCYC = 5'd10,
               S_FWREN = 5'd11, S_FCMD = 5'd12, S_FWDATA = 5'd13, S_FRDATA = 5'd14, S_FPOLL = 5'd15,
               S_FEND = 5'd16, S_RWAIT = 5'd17, S_CWLAST = 5'd18;
    reg  [4:0] st = S_IDLE, st_after = S_IDLE;
    reg  [7:0] cur_seq = 8'd0, cur_op = 8'd0, cur_arg = 8'd0, run_seq = 8'd0, done_seq = 8'd0, err = 8'd0;
    reg [23:0] f = 24'd0, n = 24'd0;
    reg  [9:0] bank_cur = 10'h3FF;       // bit 9 = unknown
    reg  [1:0] rst_ram = 2'd0;           // cart RAM: 0 enable it next, 1 the bank next, 2 ready
    reg  [1:0] rbank = 2'd0;
    reg  [1:0] bstep = 2'd0;
    reg  [2:0] cstep = 3'd0;
    reg  [7:0] val = 8'd0, poll_v1 = 8'd0;
    reg        poll_second = 1'b0;
    reg [26:0] tmo = 27'd0;
    reg [26:0] starve = 27'd0;
    reg        starved = 1'b0;
    reg [31:0] crc = 32'hFFFFFFFF, crc_res = 32'd0;
    reg        all_ff = 1'b1, all_ff_res = 1'b0;
    reg        abort_pend = 1'b0, reset_pend = 1'b0;
    reg        cyc = 1'b0;               // a bus cycle of ours is in flight
    reg [31:0] m_pbusy = 32'd0, m_ebusy = 32'd0;   // measured: program wait (sum), last erase busy
    reg [23:0] m_nprog = 24'd0;

    // ---- the boot flash's SPI engine: one byte per 16 hclk (sck = hclk / 2) ----
    reg  [7:0] sp_out = 8'd0, sp_in = 8'd0;
    reg  [3:0] sp_bits = 4'd0;
    reg        sp_ph = 1'b0;
    wire       sp_busy = (sp_bits != 4'd0);
    reg  [2:0] fstep = 3'd0;
    reg  [7:0] fcmd = 8'd0;
    reg  [7:0] fgap = 8'd0;              // CS high time between commands
    reg  [2:0] r_wdone_sync = 3'b000;
    reg        r_wdone_seen = 1'b0;
    reg        r_dirty_r = 1'b0;
    wire       f_hdr = (cur_op == 8'd20);
    // the guards: nothing below 0x100000 but the header area through FHDR
    // FREAD's limits, worked out as the op is queued (one bit per queue entry; fpgacomp1: the same test made from the
    // queue at dispatch refused good FREADs on the unit, now and then, with 0 timing violations reported)
    reg  [QD-1:0] qrok = 0, qwok = 0, qhok = 0, qrrok = 0;
    reg  [7:0] wtag_seq = 8'd0;              // 00: no op's bytes (seqs are never 00)
    assign h_wtag = {wtag_seq, 8'h00};
    // (18.31: worked out as each op is queued, one bit per queue entry: see qrok)
    wire       f_ok_w = qwok[q_head[QD_AW-1:0]];
    wire       f_ok_h = qhok[q_head[QD_AW-1:0]];

    // FREAD's window: 512 x 16 (one BSRAM), written here a word per two bytes, read on the QSPI clock
    reg [15:0] fwin [0:511];
    reg        fw_we = 1'b0;
    reg  [8:0] fw_a = 9'd0;
    reg [15:0] fw_d = 16'd0;
    reg  [9:0] fw_i = 10'd0;
    reg  [7:0] fw_lo = 8'd0;
    // FCOPY: the PSRAM address (FCDST), the word waiting for the RAM cart's port
    reg [22:0] cdst = 23'd0;
    reg        cw_pend = 1'b0;
    reg [15:0] cw_word = 16'd0;
    always @(posedge hclk) if (fw_we) fwin[fw_a] <= fw_d;
    always @(posedge qspi_clk_n) q_fwin <= fwin[q_win_addr];
    task sp_send(input [7:0] b); begin sp_out <= b; sp_bits <= 4'd8; sp_ph <= 1'b0; end endtask

    wire running = (st != S_IDLE);
    assign q_flush_now = (h_abort_sync[2] != h_abort_seen) | (h_reset_sync[2] != h_reset_seen);
    assign x_hold = running;
    wire [8:0] bank_of_f = f[22:14];
    wire [15:0] win_a = {2'b01, f[13:0]};

    // bus cycle request (held until done)
    task bus(input wr, input [15:0] a, input [7:0] d);
        begin x_bus_req <= 1'b1; x_bus_wr <= wr; x_bus_a <= a; x_bus_d <= d; cyc <= 1'b1; end
    endtask
    task fail(input [7:0] code);
        begin err <= code; st <= S_IDLE; q_head <= q_tail; x_bus_req <= 1'b0; cyc <= 1'b0; h_chg_tog <= ~h_chg_tog; end
    endtask

    // ring read port: the fetch first, then the post check
    wire f_want = !fb_ok && !f_pend && (cons != valid);
    wire c_rd_want = c_busy && !f_want && (c_pos != c_end);
    assign ring_ra = f_want ? cons[RING_AW-1:0] : c_pos[RING_AW-1:0];
    wire [31:0] c_crc_next = crc32_byte(c_crc, ring_q);

    always @(posedge hclk) begin
        h_op_sync    <= {h_op_sync[1:0], g_op_tog};
        h_post_sync  <= {h_post_sync[1:0], g_post_tog};
        h_abort_sync <= {h_abort_sync[1:0], g_abort_tog};
        h_reset_sync <= {h_reset_sync[1:0], g_reset_tog};
        h_snap_sync  <= {h_snap_sync[1:0], g_snap_req_tog};
        x_tcfg <= 1'b0;
        r_rtc_set <= 1'b0; r_dirty_clr <= 1'b0;
        fw_we <= 1'b0;
        r_cw_go <= 1'b0;
        r_wdone_sync <= {r_wdone_sync[1:0], r_wread_done_tog};
        r_dirty_r <= r_dirty;
        if (r_dirty && !r_dirty_r) h_chg_tog <= ~h_chg_tog;     // a save RAM write: tell the ESP32
        if (sp_busy) begin
            if (!sp_ph) begin f_mosi <= sp_out[7]; f_sck <= 1'b0; sp_ph <= 1'b1; end
            else begin f_sck <= 1'b1; sp_in <= {sp_in[6:0], f_miso}; sp_out <= {sp_out[6:0], 1'b0}; sp_bits <= sp_bits - 4'd1; sp_ph <= 1'b0; end
        end else f_sck <= 1'b0;
        if (fgap != 8'd0) fgap <= fgap - 8'd1;

        if (!h_init) begin                // power-on: take the toggles as they are
            h_init <= 1'b1;
            h_op_seen <= h_op_sync[2]; h_post_seen <= h_post_sync[2]; h_abort_seen <= h_abort_sync[2];
            h_reset_seen <= h_reset_sync[2]; h_snap_seen <= h_snap_sync[2];
            r_wdone_seen <= r_wdone_sync[2];
        end else begin
            // ---------------- status snapshot ----------------
            if (h_snap_sync[2] != h_snap_seen) begin
                h_snap_seen <= h_snap_sync[2];
                s_flags <= {x_session, c_busy, post_nak, starved, h_allow, err != E_NONE, running, x_session & h_allow};
                s_done_seq <= done_seq; s_err <= err; s_run_seq <= run_seq;
                s_cons <= cons; s_valid <= valid; s_crc <= crc_res; s_f <= f;
                s_pbusy <= m_pbusy; s_ebusy <= m_ebusy; s_nprog <= m_nprog;
                s_q <= {all_ff_res, post_naks, {(4-QD_AW-1){1'b0}}, q_count};
                s_rc <= {1'b0, r_hold, r_on, r_dirty, r_busy, r_density};
                s_slot <= {6'd0, h_allow, h_cart_in};
                h_snap_ack_tog <= ~h_snap_ack_tog;
            end

            // ---------------- new op ----------------
            if (h_op_sync[2] != h_op_seen) begin
                h_op_seen <= h_op_sync[2];
                if (err != E_NONE && !reset_pend) ;                    // refused until RESET (the status says why)
                else if (q_count == QD) begin err <= E_QFULL; q_head <= q_tail; h_chg_tog <= ~h_chg_tog; end
                else begin
                    qseq[q_tail[QD_AW-1:0]] <= g_op_seq; qop[q_tail[QD_AW-1:0]] <= g_op_op; qf[q_tail[QD_AW-1:0]] <= g_op_f;
                    qn[q_tail[QD_AW-1:0]] <= g_op_n; qarg[q_tail[QD_AW-1:0]] <= g_op_arg;
                    // RREAD (and FREAD below): n even, 2..1024 (1022 when tagged: word 0 is the tag), the address even (RREAD)
                    qrrok[q_tail[QD_AW-1:0]] <= (g_op_n != 24'd0) && !g_op_n[0] && !g_op_f[0] && (g_op_n <= (g_op_arg[0] ? 24'd1022 : 24'd1024));
                    qrok[q_tail[QD_AW-1:0]] <= (g_op_op == 8'd23) ? ((g_op_n != 24'd0) && !g_op_n[0] && ({1'b0, g_op_f} + {1'b0, g_op_n} <= 25'h400000))
                                                                  : ((g_op_n != 24'd0) && (g_op_n <= (g_op_arg[0] ? 24'd1022 : 24'd1024)) && !g_op_n[0] && ({1'b0, g_op_f} + {1'b0, g_op_n} <= 25'h400000));
                    // (an erase is checked by the start of the block / sector it erases: FLASH_W_BASE need not be 64 KiB aligned)
                    qwok[q_tail[QD_AW-1:0]] <= ((g_op_op == 8'd17) ? ((g_op_arg == 8'hD8) ? {g_op_f[23:16], 16'h0000} : {g_op_f[23:12], 12'h000}) : g_op_f) >= FLASH_W_BASE &&
                                               ({1'b0, g_op_f} + {1'b0, g_op_n} <= 25'h400000);
                    qhok[q_tail[QD_AW-1:0]] <= ({1'b0, g_op_f} + {1'b0, g_op_n} <= {1'b0, FLASH_H_TOP});
                    q_tail <= q_tail + 1'b1;
                end
            end
            // ABORT / RESET empty the queue as they ARRIVE (ops sent after them stay queued); the running
            // op stops at its next safe point, RESET's positions and error take effect when idle
            if (h_abort_sync[2] != h_abort_seen) begin h_abort_seen <= h_abort_sync[2]; abort_pend <= 1'b1; q_head <= q_tail; end
            if (h_reset_sync[2] != h_reset_seen) begin h_reset_seen <= h_reset_sync[2]; reset_pend <= 1'b1; q_head <= q_tail; end

            // ---------------- ring read port ----------------
            if (f_pend) begin fb <= ring_q; fb_ok <= 1'b1; f_pend <= 1'b0; end
            if (c_pend) begin
                c_crc <= c_crc_next;
                if (c_last) begin
                    c_busy <= 1'b0;
                    if (~c_crc_next == c_crcw) begin valid <= c_end; post_nak <= 1'b0; end
                    else begin post_nak <= 1'b1; post_naks <= post_naks + 3'd1; h_chg_tog <= ~h_chg_tog; end   // only a refusal is reported
                end
            end
            c_pend <= 1'b0; c_last <= 1'b0;
            if (f_want) begin
                f_pend <= 1'b1;
            end else if (c_rd_want) begin
                c_pend <= 1'b1; c_last <= (c_pos + 24'd1 == c_end);
                c_pos <= c_pos + 24'd1;
            end

            // ---------------- a post ----------------
            if (h_post_sync[2] != h_post_seen) begin
                h_post_seen <= h_post_sync[2];
                if (!c_busy && g_post_len != 16'd0 && {8'd0, g_post_len} <= RING &&
                    g_post_end - {8'd0, g_post_len} == valid && g_post_end - cons <= RING) begin
                    c_busy <= 1'b1; c_pos <= valid; c_end <= g_post_end; c_crcw <= g_post_crc; c_crc <= 32'hFFFFFFFF;
                end else begin
                    post_nak <= 1'b1; post_naks <= post_naks + 3'd1; h_chg_tog <= ~h_chg_tog;
                end
            end

            // ---------------- the sequencer ----------------
            if (cyc && x_bus_done) begin x_bus_req <= 1'b0; cyc <= 1'b0; end

            case (st)
            S_IDLE: begin
                starved <= 1'b0;
                if (reset_pend) begin
                    reset_pend <= 1'b0; abort_pend <= 1'b0;
                    m_pbusy <= 32'd0; m_nprog <= 24'd0;
                    cons <= 24'd0; valid <= 24'd0; fb_ok <= 1'b0; f_pend <= 1'b0; c_busy <= 1'b0; c_pend <= 1'b0;
                    err <= E_NONE; post_nak <= 1'b0;
                    h_chg_tog <= ~h_chg_tog;
                end else if (abort_pend) begin
                    abort_pend <= 1'b0; h_chg_tog <= ~h_chg_tog;
                end else if (q_count != 0 && err == E_NONE && !h_eng_busy && !q_flush_now) begin
                    cur_seq <= qseq[q_head[QD_AW-1:0]]; cur_op <= qop[q_head[QD_AW-1:0]]; f <= qf[q_head[QD_AW-1:0]]; n <= qn[q_head[QD_AW-1:0]]; cur_arg <= qarg[q_head[QD_AW-1:0]];
                    run_seq <= qseq[q_head[QD_AW-1:0]];
                    q_head <= q_head + 1'b1;
                    bank_cur <= 10'h3FF;
                    h_chg_tog <= ~h_chg_tog;
                    case (qop[q_head[QD_AW-1:0]])
                    8'd1: begin
                        if (!(x_session && h_allow)) begin err <= E_NOSESS; q_head <= q_tail; end
                        else begin st <= S_BANK; st_after <= S_CMD; bstep <= 2'd0; cstep <= 3'd0; end
                    end
                    8'd2: begin
                        if (!(x_session && h_allow)) begin err <= E_NOSESS; q_head <= q_tail; end
                        else begin st <= S_PNEXT; starve <= 27'd0; end
                    end
                    8'd3: begin
                        if (!(x_session && h_allow)) begin err <= E_NOSESS; q_head <= q_tail; end
                        else begin st <= S_CNEXT; crc <= 32'hFFFFFFFF; all_ff <= 1'b1; end
                    end
                    8'd4: begin
                        x_t_setup <= qf[q_head[QD_AW-1:0]][4:0]; x_t_pulse <= qf[q_head[QD_AW-1:0]][12:8]; x_t_hold <= qf[q_head[QD_AW-1:0]][20:16];
                        x_t_access <= qn[q_head[QD_AW-1:0]][4:0]; x_t_gap <= qn[q_head[QD_AW-1:0]][12:8];
                        x_tcfg <= 1'b1;
                        x_session <= qn[q_head[QD_AW-1:0]][16] & h_allow;
                        done_seq <= qseq[q_head[QD_AW-1:0]];
                    end
                    8'd5, 8'd6: begin                                   // RAMW / RAMCRC (cart RAM, A000-BFFF)
                        if (!(x_session && h_allow)) begin err <= E_NOSESS; q_head <= q_tail; end
                        else begin st <= S_RNEXT; rst_ram <= 2'd0; crc <= 32'hFFFFFFFF; all_ff <= 1'b1; starve <= 27'd0; end
                    end
                    8'd16: begin st <= S_FCMD; fcmd <= 8'h9F; fstep <= 3'd0; crc <= 32'd0; end   // FID
                    8'd17: begin                                                                    // FERASE
                        if (!f_ok_w || (qarg[q_head[QD_AW-1:0]] != 8'h20 && qarg[q_head[QD_AW-1:0]] != 8'hD8)) begin err <= E_BADOP; q_head <= q_tail; end
                        else begin st <= S_FWREN; st_after <= S_FCMD; fcmd <= qarg[q_head[QD_AW-1:0]]; fstep <= 3'd0; tmo <= 27'd0; end
                    end
                    8'd18, 8'd20: begin                                                             // FPROG / FHDR
                        if ((qop[q_head[QD_AW-1:0]] == 8'd18) ? !f_ok_w : !f_ok_h) begin err <= E_BADOP; q_head <= q_tail; end
                        else begin st <= S_FWREN; st_after <= S_FCMD; fcmd <= 8'h02; fstep <= 3'd0; starve <= 27'd0; end
                    end
                    8'd19: begin                                                                    // FCRC
                        if (qn[q_head[QD_AW-1:0]] == 24'd0) begin err <= E_BADOP; q_head <= q_tail; end
                        else begin st <= S_FCMD; fcmd <= 8'h03; fstep <= 3'd0; crc <= 32'hFFFFFFFF; all_ff <= 1'b1; end
                    end
                    8'd21: begin                                                                    // FHDRERASE
                        if (qarg[q_head[QD_AW-1:0]] != 8'h5A || !HDRERASE_OK) begin err <= E_BADOP; q_head <= q_tail; end
                        else begin f <= 24'd0; st <= S_FWREN; st_after <= S_FCMD; fcmd <= 8'h20; fstep <= 3'd0; tmo <= 27'd0; end
                    end
                    8'd22: begin                                                                    // FREAD (18.31)
                        if (!qrok[q_head[QD_AW-1:0]]) begin err <= E_BADOP; q_head <= q_tail; end
                        else begin st <= S_FCMD; fcmd <= 8'h03; fstep <= 3'd0; crc <= 32'hFFFFFFFF; all_ff <= 1'b1; fw_i <= 10'd0;
                                   h_wtag_on <= qarg[q_head[QD_AW-1:0]][0]; wtag_seq <= 8'd0; end
                    end
                    8'd24: begin                                                                    // FCDST (18.31)
                        if (!HAS_FCOPY || qf[q_head[QD_AW-1:0]][0] || qf[q_head[QD_AW-1:0]] < 24'h040000 || qf[q_head[QD_AW-1:0]] >= 24'h800000) begin err <= E_BADOP; q_head <= q_tail; end
                        else begin cdst <= qf[q_head[QD_AW-1:0]][22:0]; done_seq <= qseq[q_head[QD_AW-1:0]]; end
                    end
                    8'd23: begin                                                                    // FCOPY (18.31)
                        if (!HAS_FCOPY || !qrok[q_head[QD_AW-1:0]] || r_on || ({1'b0, cdst} + {1'b0, qn[q_head[QD_AW-1:0]]} > 25'h800000)) begin err <= E_BADOP; q_head <= q_tail; end
                        else begin st <= S_FCMD; fcmd <= 8'h03; fstep <= 3'd0; crc <= 32'hFFFFFFFF; all_ff <= 1'b1; fw_i <= 10'd0; cw_pend <= 1'b0; starve <= 27'd0; end
                    end
                    8'd32: begin                                                                    // RCFG
                        r_mbc <= qf[q_head[QD_AW-1:0]][2:0]; r_ram_has <= qf[q_head[QD_AW-1:0]][3]; r_rtc_has <= qf[q_head[QD_AW-1:0]][4];
                        r_rom_mask <= qf[q_head[QD_AW-1:0]][15:8]; r_ram_mask <= qf[q_head[QD_AW-1:0]][19:16];
                        r_on <= qarg[q_head[QD_AW-1:0]][0]; r_hold <= qarg[q_head[QD_AW-1:0]][1];
                        if (qarg[q_head[QD_AW-1:0]][2]) r_inval_tog <= ~r_inval_tog;
                        if (qarg[q_head[QD_AW-1:0]][3]) r_dirty_clr <= 1'b1;
                        if (qarg[q_head[QD_AW-1:0]][4]) r_mreset_tog <= ~r_mreset_tog;
                        done_seq <= qseq[q_head[QD_AW-1:0]];
                    end
                    8'd33: begin                                                                    // RREAD
                        if (!qrrok[q_head[QD_AW-1:0]]) begin err <= E_BADOP; q_head <= q_tail; end
                        else begin
                            r_wread_addr <= qf[q_head[QD_AW-1:0]][22:0]; r_wread_len <= qn[q_head[QD_AW-1:0]][10:0];
                            r_wread_off <= qarg[q_head[QD_AW-1:0]][0];
                            h_wtag_on <= qarg[q_head[QD_AW-1:0]][0]; wtag_seq <= 8'd0;
                            r_wread_tog <= ~r_wread_tog; st <= S_RWAIT; h_fwin_sel <= 1'b0;
                        end
                    end
                    8'd35: begin crc_res <= r_dbg[31:0]; f <= {8'd0, r_dbg[47:32]}; done_seq <= qseq[q_head[QD_AW-1:0]]; end   // RDBG (bench)
                    8'd34: begin                                                                    // RRTC
                        if (qarg[q_head[QD_AW-1:0]] == 8'd1) begin r_rtc_in <= {qn[q_head[QD_AW-1:0]][15:0], qf[q_head[QD_AW-1:0]]}; r_rtc_set <= 1'b1; end
                        else begin crc_res <= r_rtc_out[31:0]; f <= {16'd0, r_rtc_out[39:32]}; end
                        done_seq <= qseq[q_head[QD_AW-1:0]];
                    end
                    default: begin err <= E_BADOP; q_head <= q_tail; end
                    endcase
                end
            end
            // ---- the boot flash: WREN, then the command ----
            S_FWREN: begin
                if (!sp_busy && fgap == 8'd0) begin
                    if (fstep == 3'd0) begin f_cs_n <= 1'b0; sp_send(8'h06); fstep <= 3'd1; end
                    else begin f_cs_n <= 1'b1; fgap <= 8'd4; fstep <= 3'd0; st <= st_after; end
                end
            end
            // the command byte and (but for FID) three address bytes, then what follows it
            S_FCMD: begin
                if (!sp_busy && fgap == 8'd0) begin
                    case (fstep)
                    3'd0: begin f_cs_n <= 1'b0; sp_send(fcmd); fstep <= (fcmd == 8'h9F) ? 3'd4 : 3'd1; end
                    3'd1: begin sp_send(f[23:16]); fstep <= 3'd2; end
                    3'd2: begin sp_send(f[15:8]); fstep <= 3'd3; end
                    3'd3: begin sp_send(f[7:0]); fstep <= 3'd4; end
                    default: begin
                        fstep <= 3'd0;
                        if (fcmd == 8'h02) st <= S_FWDATA;
                        else if (fcmd == 8'h03 || fcmd == 8'h9F) begin st <= S_FRDATA; if (fcmd == 8'h9F) n <= 24'd3; end
                        else begin f_cs_n <= 1'b1; fgap <= 8'd4; st <= S_FPOLL; tmo <= 27'd0; end   // an erase
                    end
                    endcase
                end
            end
            // a page program's bytes from the ring, to the page's end or the op's end
            S_FWDATA: begin
                if (!sp_busy) begin
                    if (n == 24'd0 || (fstep == 3'd1 && f[7:0] == 8'h00) || abort_pend) begin
                        f_cs_n <= 1'b1; fgap <= 8'd4; fstep <= 3'd0; st <= S_FPOLL; tmo <= 27'd0;
                    end else if (fb_ok) begin
                        fb_ok <= 1'b0; cons <= cons + 24'd1;
                        if (cons[9:0] == 10'h3FF) h_chg_tog <= ~h_chg_tog;
                        starved <= 1'b0; starve <= 27'd0;
                        // FHDR: an FF byte is sent as FF (programs nothing)
                        sp_send(fb); f <= f + 24'd1; n <= n - 24'd1; fstep <= 3'd1;
                    end else begin
                        starve <= starve + 27'd1;
                        if (starve == STARVE_FLAG) begin starved <= 1'b1; h_chg_tog <= ~h_chg_tog; end
                        if (starve > STARVE_TMO) begin f_cs_n <= 1'b1; fail(E_STARVE); end
                    end
                end
            end
            // read bytes (FCRC: crc32 + all FF; FID: three id bytes)
            S_FRDATA: begin
                // fstep 1: a byte has just come in (sp_in); 2: FCOPY handed a word over, nothing came in; 0: the first send
                if (cur_op == 8'd23 && cw_pend && !sp_busy) begin             // FCOPY: the word first, the next byte after it
                    if (r_cw_ready && !r_cw_go) begin
                        r_cw_go <= 1'b1; r_cw_addr <= cdst; r_cw_data <= cw_word; cdst <= cdst + 23'd2; cw_pend <= 1'b0; starve <= 27'd0;
                    end else begin
                        starve <= starve + 27'd1;
                        if (starve > STARVE_TMO) begin f_cs_n <= 1'b1; fail(E_STARVE); end
                    end
                end else if (!sp_busy) begin
                    if (fstep == 3'd1) begin
                        if (cur_op == 8'd16) crc <= {crc[23:0], sp_in};
                        else begin crc <= crc32_byte(crc, sp_in); if (sp_in != 8'hFF) all_ff <= 1'b0; end
                        n <= n - 24'd1;
                        if (cur_op == 8'd22) begin                          // FREAD: low byte first, then the word
                            if (!fw_i[0]) fw_lo <= sp_in;
                            else begin fw_we <= 1'b1; fw_a <= fw_i[9:1] + {8'd0, cur_arg[0]}; fw_d <= {sp_in, fw_lo}; end
                        end
                        if (cur_op == 8'd23) begin                          // FCOPY: low byte first (PSRAM words as the ESP32 writes them)
                            if (!fw_i[0]) fw_lo <= sp_in;
                            else begin cw_pend <= 1'b1; cw_word <= {sp_in, fw_lo}; end
                        end
                        fw_i <= fw_i + 10'd1;
                    end
                    if ((fstep == 3'd1 && n == 24'd1) || (fstep == 3'd2 && n == 24'd0) || abort_pend) begin
                        f_cs_n <= 1'b1; fgap <= 8'd4; st <= (cur_op == 8'd23 && !abort_pend) ? S_CWLAST : S_FEND;
                    end else if (cur_op == 8'd23 && fstep == 3'd1 && fw_i[0]) fstep <= 3'd2;   // a word: hand it over first
                    else begin sp_send(8'h00); fstep <= 3'd1; end
                end
            end
            // status register polling (RDSR, WIP = bit 0) after an erase or a page
            S_FPOLL: begin
                tmo <= tmo + 27'd1;
                if (cur_op == 8'd18 || cur_op == 8'd20) m_pbusy <= m_pbusy + 32'd1;
                if (!sp_busy && fgap == 8'd0) begin
                    case (fstep)
                    3'd0: begin f_cs_n <= 1'b0; sp_send(8'h05); fstep <= 3'd1; end
                    3'd1: begin sp_send(8'h00); fstep <= 3'd2; end
                    default: begin
                        if (!sp_in[0]) begin
                            f_cs_n <= 1'b1; fgap <= 8'd4; fstep <= 3'd0;
                            if (cur_op == 8'd17 || cur_op == 8'd21) m_ebusy <= {5'd0, tmo};
                            if ((cur_op == 8'd18 || cur_op == 8'd20) && n != 24'd0 && !abort_pend) begin
                                st <= S_FWREN; st_after <= S_FCMD; fcmd <= 8'h02;    // the next page
                            end else st <= S_FEND;
                        end else if (tmo > ERASE_TMO) begin f_cs_n <= 1'b1; fail(E_ETMO); end
                        else sp_send(8'h00);
                    end
                    endcase
                end
            end
            S_FEND: begin
                if (cur_op == 8'd16) crc_res <= crc;
                else if (cur_op == 8'd19 || cur_op == 8'd22 || cur_op == 8'd23) begin crc_res <= ~crc; all_ff_res <= all_ff; end
                if (cur_op == 8'd22) begin h_fwin_sel <= 1'b1; wtag_seq <= cur_seq; end
                done_seq <= cur_seq; st <= S_IDLE; h_chg_tog <= ~h_chg_tog;
                if (abort_pend) begin abort_pend <= 1'b0; err <= E_ABORT; end
            end
            // FCOPY: the last word handed over, then the end
            S_CWLAST: begin
                if (!cw_pend) st <= S_FEND;
                else if (r_cw_ready && !r_cw_go) begin r_cw_go <= 1'b1; r_cw_addr <= cdst; r_cw_data <= cw_word; cdst <= cdst + 23'd2; cw_pend <= 1'b0; end
                else begin starve <= starve + 27'd1; if (starve > STARVE_TMO) fail(E_STARVE); end
            end
            // RREAD: wait for the window
            S_RWAIT: begin
                if (r_wdone_sync[2] != r_wdone_seen) begin
                    r_wdone_seen <= r_wdone_sync[2];
                    done_seq <= cur_seq; st <= S_IDLE; h_chg_tog <= ~h_chg_tog; wtag_seq <= cur_seq;
                end
            end
            // ---- the bank register (MBC5): 0x2000 = bank[7:0], 0x3000 = bank[8], only when it changes
            S_BANK: begin
                if (!cyc && !x_bus_req) begin
                    if (bank_cur == {1'b0, bank_of_f}) st <= st_after;
                    else if (bstep == 2'd0) begin bus(1'b1, 16'h2000, bank_of_f[7:0]); bstep <= 2'd1; end
                    else if (bstep == 2'd1) begin bus(1'b1, 16'h3000, {7'd0, bank_of_f[8]}); bstep <= 2'd2; end
                    else begin bank_cur <= {1'b0, bank_of_f}; bstep <= 2'd0; st <= st_after; end
                end
            end
            // ---- the command cycles of an erase (6) or a program (4); each next cycle is asked for in
            //      the clock the last one ends (the engine starts it 2 clocks after the end, as its own)
            S_CMD: begin
                if ((!cyc && !x_bus_req && cstep == 3'd0) || (cyc && x_bus_done)) begin
                    if (cur_op == 8'd1) begin
                        case (cstep)
                        3'd0: bus(1'b1, 16'h0AAA, 8'hAA);
                        3'd1: bus(1'b1, 16'h0555, 8'h55);
                        3'd2: bus(1'b1, 16'h0AAA, 8'h80);
                        3'd3: bus(1'b1, 16'h0AAA, 8'hAA);
                        3'd4: bus(1'b1, 16'h0555, 8'h55);
                        3'd5: bus(1'b1, (cur_arg == 8'h10) ? 16'h0AAA : win_a, cur_arg);
                        default: bus(1'b0, win_a, 8'h00);                    // the first poll read
                        endcase
                        if (cstep == 3'd6) begin st <= S_POLL; poll_second <= 1'b0; tmo <= 27'd0; cstep <= 3'd0; end
                        else cstep <= cstep + 3'd1;
                    end else begin
                        case (cstep)
                        3'd0: bus(1'b1, 16'h0AAA, 8'hAA);
                        3'd1: bus(1'b1, 16'h0555, 8'h55);
                        3'd2: bus(1'b1, 16'h0AAA, 8'hA0);
                        3'd3: bus(1'b1, win_a, val);
                        default: bus(1'b0, win_a, 8'h00);                    // the first poll read
                        endcase
                        if (cstep == 3'd4) begin st <= S_POLL; poll_second <= 1'b0; tmo <= 27'd0; cstep <= 3'd0; end
                        else cstep <= cstep + 3'd1;
                    end
                end
            end
            // ---- DQ6 toggle polling at the byte / sector (a poll read is always in flight here)
            S_POLL: begin
                tmo <= tmo + 27'd1;
                if (cur_op == 8'd2) m_pbusy <= m_pbusy + 32'd1;
                if (cyc && x_bus_done) begin
                    if (!poll_second) begin poll_v1 <= x_bus_q; poll_second <= 1'b1; bus(1'b0, win_a, 8'h00); end
                    else if (((poll_v1 ^ x_bus_q) & 8'h40) == 8'h00) begin          // stopped toggling: done
                        if (cur_op == 8'd2) begin
                            m_nprog <= m_nprog + 24'd1;
                            if (x_bus_q != val) fail(E_VERIFY);
                            else begin f <= f + 24'd1; n <= n - 24'd1; st <= S_PNEXT; starve <= 27'd0; end
                        end else begin st <= S_EDONE; m_ebusy <= {5'd0, tmo}; end
                    end else begin
                        poll_v1 <= x_bus_q;
                        if (tmo > ((cur_op == 8'd2) ? PROG_TMO : ERASE_TMO)) fail(cur_op == 8'd2 ? E_PTMO : E_ETMO);
                        else bus(1'b0, win_a, 8'h00);
                    end
                end
            end
            S_EDONE: begin
                done_seq <= cur_seq; st <= S_IDLE; h_chg_tog <= ~h_chg_tog;
                if (abort_pend) begin abort_pend <= 1'b0; err <= E_ABORT; end
            end
            // ---- PROG: the next byte from the ring
            S_PNEXT: begin
                if (n == 24'd0) begin done_seq <= cur_seq; st <= S_IDLE; h_chg_tog <= ~h_chg_tog; end
                else if (abort_pend) begin abort_pend <= 1'b0; err <= E_ABORT; st <= S_IDLE; x_bus_req <= 1'b0; cyc <= 1'b0; h_chg_tog <= ~h_chg_tog; end
                else if (!h_allow || h_reset) fail(E_NOSESS);
                else if (bank_cur != {1'b0, bank_of_f}) begin st <= S_BANK; st_after <= S_PNEXT; bstep <= 2'd0; end
                else if (fb_ok) begin
                    fb_ok <= 1'b0; cons <= cons + 24'd1;
                    if (cons[9:0] == 10'h3FF) h_chg_tog <= ~h_chg_tog;          // another KiB consumed
                    starved <= 1'b0; starve <= 27'd0;
                    if (fb == 8'hFF) begin f <= f + 24'd1; n <= n - 24'd1; end   // erased already
                    else begin val <= fb; st <= S_CMD; bus(1'b1, 16'h0AAA, 8'hAA); cstep <= 3'd1; end
                end else begin
                    starve <= starve + 27'd1;
                    if (starve == STARVE_FLAG) begin starved <= 1'b1; h_chg_tog <= ~h_chg_tog; end
                    if (starve > STARVE_TMO) fail(E_STARVE);
                end
            end
            // ---- cart RAM (MBC5: 0A at 0000 enables it, the bank at 4000; the engine drives /CS for
            //      A000-BFFF): RAMW writes n bytes from the ring at F (F[14:13] bank, A000 + F[12:0]),
            //      RAMCRC reads them back for the crc32. No FF skip: RAM is not flash.
            S_RNEXT: begin
                if (n == 24'd0) begin
                    if (cur_op == 8'd6) begin crc_res <= ~crc; all_ff_res <= all_ff; end
                    done_seq <= cur_seq; st <= S_IDLE; h_chg_tog <= ~h_chg_tog;
                end
                else if (abort_pend) begin abort_pend <= 1'b0; err <= E_ABORT; st <= S_IDLE; h_chg_tog <= ~h_chg_tog; end
                else if (!h_allow || h_reset) fail(E_NOSESS);
                else if (!cyc && !x_bus_req) begin
                    if (rst_ram == 2'd0) begin bus(1'b1, 16'h0000, 8'h0A); rst_ram <= 2'd1; end          // RAM enable
                    else if (rst_ram == 2'd1 || rbank != f[14:13]) begin bus(1'b1, 16'h4000, {6'd0, f[14:13]}); rbank <= f[14:13]; rst_ram <= 2'd2; end
                    else if (cur_op == 8'd6) begin bus(1'b0, {3'b101, f[12:0]}, 8'h00); st <= S_RCYC; end
                    else if (fb_ok) begin
                        fb_ok <= 1'b0; cons <= cons + 24'd1;
                        if (cons[9:0] == 10'h3FF) h_chg_tog <= ~h_chg_tog;
                        starved <= 1'b0; starve <= 27'd0;
                        bus(1'b1, {3'b101, f[12:0]}, fb); st <= S_RCYC;
                    end else begin
                        starve <= starve + 27'd1;
                        if (starve == STARVE_FLAG) begin starved <= 1'b1; h_chg_tog <= ~h_chg_tog; end
                        if (starve > STARVE_TMO) fail(E_STARVE);
                    end
                end
            end
            S_RCYC: begin
                if (cyc && x_bus_done) begin
                    if (cur_op == 8'd6) begin crc <= crc32_byte(crc, x_bus_q); if (x_bus_q != 8'hFF) all_ff <= 1'b0; end
                    f <= f + 24'd1; n <= n - 24'd1; st <= S_RNEXT;
                end
            end
            // ---- CRC: read byte after byte (the next read asked for in the clock the last one ends)
            S_CNEXT: begin
                if (n == 24'd0) begin
                    crc_res <= ~crc; all_ff_res <= all_ff;
                    done_seq <= cur_seq; st <= S_IDLE; h_chg_tog <= ~h_chg_tog;
                end
                else if (abort_pend) begin abort_pend <= 1'b0; err <= E_ABORT; st <= S_IDLE; x_bus_req <= 1'b0; cyc <= 1'b0; h_chg_tog <= ~h_chg_tog; end
                else if (!h_allow || h_reset) fail(E_NOSESS);
                else if (bank_cur != {1'b0, bank_of_f}) begin st <= S_BANK; st_after <= S_CNEXT; bstep <= 2'd0; end
                else if (!cyc && !x_bus_req) begin bus(1'b0, win_a, 8'h00); st <= S_CRD; end
            end
            S_CRD: begin
                if (cyc && x_bus_done) begin
                    crc <= crc32_byte(crc, x_bus_q);
                    if (x_bus_q != 8'hFF) all_ff <= 1'b0;
                    f <= f + 24'd1; n <= n - 24'd1;
                    // same bank, more to read, nothing to stop for: the next read at once
                    if (n != 24'd1 && f[13:0] != 14'h3FFF && !abort_pend && h_allow && !h_reset)
                        bus(1'b0, {2'b01, f[13:0] + 14'd1}, 8'h00);
                    else st <= S_CNEXT;
                end
            end
            default: st <= S_IDLE;
            endcase

            // the session ends with `allow` or a GB reset
            if (!h_allow || h_reset) begin
                x_session <= 1'b0;
                // (the boot flash and RAM cart ops are not cart sessions: they run on)
                if (running && st < S_FWREN && st != S_PNEXT && st != S_CNEXT && st != S_RNEXT) begin
                    // mid-command: the engine has dropped the bus (it resets on the same condition)
                    err <= E_NOSESS; st <= S_IDLE; q_head <= q_tail; x_bus_req <= 1'b0; cyc <= 1'b0;
                    h_chg_tog <= ~h_chg_tog;
                end
            end
        end
    end
endmodule
