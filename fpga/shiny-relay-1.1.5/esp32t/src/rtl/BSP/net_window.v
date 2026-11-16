// net_window.v
//
// Network window v1: a memory-mapped mailbox that a GB/GBC game reads and
// writes directly at FF58-FF5E, with the ESP32 (Wi-Fi) on the other side.
// Spec: NETWORK_WINDOW.md at the top of the chromatic_wifi workspace.
//
//   FF58 NW_ID    R: 0x4E ('N') once unlocked         W: unlock key 'S','G','N','W' / 0x00 = lock
//   FF59 NW_VER   R: 0x10 (v1.0)
//   FF5A NW_CMD   R: NW_ERR (first error, sticky)     W: 00 CLR_ERR, 01 SEND, 02 RX_ACK, 03 ABORT
//   FF5B NW_STAT  R: {0,0,NOTIFY,ONLINE,SVC,ERR,RX_READY,BUSY}
//   FF5C NW_PTRL  R/W pointer bits 7:0
//   FF5D NW_PTRH  R/W pointer bits 12:8
//   FF5E NW_DATA  R: RX[ptr++]                        W: TX[ptr++]
//   FF5F          reserved (reads 0xFF)
//
// While locked (power-on, GB reset, after writing 0x00 to FF58) the window does
// not drive the bus at all: every address reads 0xFF exactly as on a stock
// Chromatic or a real CGB, and only FF58 listens for the key.
//
// Two clock domains:
//   hclk (~16.78 MHz): GB CPU bus side. TX buffer write port, RX buffer read port.
//   gclk (~8.39 MHz):  system monitor side. Sniffs the ESP32 -> FPGA UART byte
//                      stream for packets to address 0x11, and offers packets
//                      for system monitor TX channel 11 (address 0x0B).
// Buffers are dual-clock BSRAMs. Ownership passes with toggle handshakes:
//   TX: game owns it until SEND (commit toggle), then the gclk side owns it
//       until the response is posted or the request is aborted.
//   RX: gclk side owns it from a commit until it posts (post toggle), then the
//       game owns it until its next SEND / RX_ACK / ABORT / lock.
// Each toggle carries a payload that is written in the same clock as the
// toggle flips and stays stable until the next flip of that toggle, which is
// always many destination clocks later (a CPU instruction, or a UART packet).
// Toggles are never reset (only power-on initial values), so a reset on one
// side cannot fake an edge on the other.

module net_window #(
    parameter TX_AW       = 11,     // TX buffer 2 KiB
    parameter RX_AW       = 12,     // RX buffer 4 KiB
    parameter CHUNK       = 128,    // TXDATA payload per UART packet
    parameter CHUNK_SMALL = 32,     // ... while the link cable is in use (keeps link events prompt)
    parameter WD_BITS     = 25,     // service watchdog: 2^25 gclk = 4.0 s at 8.388 MHz
    parameter [6:0] RX_ADDR = 7'h11 // ESP32 -> FPGA UART address
) (
    // ---------------- hclk: GB CPU bus ----------------
    input             hclk,
    input             h_reset,       // GB reset (active high): locks the window, aborts a request
    input      [15:0] h_cpu_addr,
    input       [7:0] h_cpu_do,
    input             h_cpu_rd_n,
    input             h_cpu_wr_stb,  // one hclk pulse per CPU write (gb.v: ce_cpu & ~cpu_wr_n_edge)
    output            h_sel,         // the window answers this read
    output      [7:0] h_do,

    // ---------------- gclk: system monitor / UART ----------------
    input             gclk,
    input             g_reset,
    input       [7:0] g_uart_rx_data,
    input             g_uart_rx_val,
    output reg        g_tx_valid = 1'b0, // a packet for TX channel 11 is ready (held until taken)
    output      [7:0] g_tx_len,          // payload byte count
    input       [7:0] g_tx_bytepos,      // from uart_packet_wrapper_tx
    output      [7:0] g_tx_byte,         // payload byte at g_tx_bytepos (1 gclk after it changes)
    input             g_tx_taken,        // packet sent (write_done on channel 11)
    input             g_link_busy,       // CWL link in use: use small chunks
    output            g_esp_awake,       // CONFIG bit 3: no wake preamble needed
    output            g_unlocked,        // a game has the window open (power manager hint)
    output reg [15:0] g_crc_errors = 16'd0
);
    localparam TX_SIZE = 1 << TX_AW;
    localparam RX_SIZE = 1 << RX_AW;
    localparam PW = ((TX_AW > RX_AW) ? TX_AW : RX_AW) + 1;

    localparam [7:0] NW_ID_MAGIC = 8'h4E;   // 'N'
    localparam [7:0] NW_VERSION  = 8'h10;   // v1.0

    // NW_ERR codes
    localparam [7:0] E_NONE        = 8'd0;
    localparam [7:0] E_BUSY        = 8'd1;  // SEND or data write while a request is in flight
    localparam [7:0] E_NOSVC       = 8'd2;  // SEND while no ESP32 service is attached
    localparam [7:0] E_TX_OVERRUN  = 8'd3;  // data write past the TX buffer
    localparam [7:0] E_RX_UNDERRUN = 8'd4;  // data read past the response length
    localparam [7:0] E_BADLEN      = 8'd5;  // SEND with ptr 0 or ptr > TX size
    localparam [7:0] E_BADCMD      = 8'd6;  // unknown command
    localparam [7:0] E_NOT_READY   = 8'd7;  // data read with no response ready
    localparam [7:0] E_SVC_LOST    = 8'd8;  // service went away while a request was in flight

    // ESP32 -> FPGA packet types (address 0x11)
    localparam [7:0] R_CONFIG = 8'd1, R_RXDATA = 8'd2, R_RXPOST = 8'd3, R_TXREAD = 8'd4, R_STATUS = 8'd5;
    // FPGA -> ESP32 packet types (TX channel 11 / address 0x0B)
    localparam [7:0] T_COMMIT = 8'd1, T_TXDATA = 8'd2, T_ABORT = 8'd3, T_RXACK = 8'd4,
                     T_POSTED = 8'd5, T_RXNAK  = 8'd6, T_STATE = 8'd7;

    // =====================================================================
    // Cross-domain signals
    // =====================================================================
    // h -> g
    reg            h_unlocked = 1'b0;
    reg            h_commit_tog = 1'b0;
    reg      [7:0] h_commit_id = 8'd0;
    reg  [TX_AW:0] h_commit_len = 0;
    reg            h_abort_tog = 1'b0;
    reg      [7:0] h_abort_id = 8'd0;
    reg            h_rxack_tog = 1'b0;
    reg      [7:0] h_rxack_id = 8'd0;
    // g -> h
    reg            g_post_tog = 1'b0;
    reg      [7:0] g_post_id = 8'd0;
    reg  [RX_AW:0] g_post_len = 0;
    reg            g_svc = 1'b0, g_online = 1'b0, g_notify = 1'b0, g_awake = 1'b0;

    // =====================================================================
    // Buffers
    // =====================================================================
    wire           tx_we;
    wire [TX_AW-1:0] tx_waddr;
    wire     [7:0] tx_q;
    reg  [TX_AW-1:0] tx_raddr;
    wire     [7:0] rx_q;
    reg            rx_we = 1'b0;
    reg  [RX_AW-1:0] rx_waddr = 0;
    reg      [7:0] rx_wdata = 8'd0;
    reg     [PW-1:0] ptr = 0;

    net_window_dpram #(.AW(TX_AW)) u_txbuf (
        .wclk(hclk), .we(tx_we), .waddr(tx_waddr), .wdata(h_cpu_do),
        .rclk(gclk), .raddr(tx_raddr), .rdata(tx_q));

    net_window_dpram #(.AW(RX_AW)) u_rxbuf (
        .wclk(gclk), .we(rx_we), .waddr(rx_waddr), .wdata(rx_wdata),
        .rclk(hclk), .raddr(ptr[RX_AW-1:0]), .rdata(rx_q));

    // =====================================================================
    // hclk side: CPU registers
    // =====================================================================
    reg      [1:0] key_idx = 2'd0;
    reg      [7:0] err = 8'd0;
    reg            busy = 1'b0;
    reg            rx_ready = 1'b0;
    reg      [7:0] cur_id = 8'd0;      // id of the last committed request (never reset)
    reg  [RX_AW:0] rx_len = 0;
    reg            rd_data_q = 1'b0;

    reg  [2:0] h_post_sync = 3'd0;
    reg        h_post_seen = 1'b0;
    reg  [1:0] h_svc_sync = 2'd0, h_online_sync = 2'd0, h_notify_sync = 2'd0;
    wire h_svc = h_svc_sync[1];

    wire       in_page = (h_cpu_addr[15:3] == 13'h1FEB);    // FF58-FF5F
    wire [2:0] r       = h_cpu_addr[2:0];
    wire       wr      = h_cpu_wr_stb & in_page;
    assign     h_sel   = in_page & h_unlocked & (r != 3'd7);
    wire       rd_data_act = in_page & h_unlocked & (r == 3'd6) & ~h_cpu_rd_n;
    wire       rd_ok   = rx_ready & (rx_len > ptr);

    wire [7:0] stat = {2'b00, h_notify_sync[1], h_online_sync[1], h_svc, (err != 8'd0), rx_ready, busy};
    assign h_do = (r == 3'd0) ? NW_ID_MAGIC :
                  (r == 3'd1) ? NW_VERSION :
                  (r == 3'd2) ? err :
                  (r == 3'd3) ? stat :
                  (r == 3'd4) ? ptr[7:0] :
                  (r == 3'd5) ? {{(16-PW){1'b0}}, ptr[PW-1:8]} :
                  (r == 3'd6) ? (rd_ok ? rx_q : 8'hFF) :
                                8'hFF;

    wire tx_wr_ok = wr & h_unlocked & (r == 3'd6) & ~busy & (ptr < TX_SIZE);
    assign tx_we    = tx_wr_ok;
    assign tx_waddr = ptr[TX_AW-1:0];

    // A post is applied only between data-port reads, so the byte a read
    // returns and the pointer step at its end always agree.
    wire post_pending = (h_post_sync[2] != h_post_seen) & ~rd_data_act & ~rd_data_q;

    // key sequence 'S' 'G' 'N' 'W'
    wire [7:0] key_want = (key_idx == 2'd0) ? 8'h53 : (key_idx == 2'd1) ? 8'h47 :
                          (key_idx == 2'd2) ? 8'h4E : 8'h57;

    task automatic set_err(input [7:0] e);
        if (err == E_NONE) err <= e;
    endtask

    always @(posedge hclk) begin
        h_post_sync   <= {h_post_sync[1:0], g_post_tog};
        h_svc_sync    <= {h_svc_sync[0], g_svc};
        h_online_sync <= {h_online_sync[0], g_online};
        h_notify_sync <= {h_notify_sync[0], g_notify};

        if (h_reset) begin
            h_unlocked <= 1'b0;
            key_idx    <= 2'd0;
            ptr        <= 0;
            err        <= E_NONE;
            busy       <= 1'b0;       // the gclk side sees the lock and aborts the request
            rx_ready   <= 1'b0;
            rd_data_q  <= 1'b0;
            h_post_seen <= h_post_sync[2];
        end else begin
            rd_data_q <= rd_data_act;

            // end of a data-port read: step the pointer
            if (rd_data_q & ~rd_data_act) begin
                if (rd_ok) ptr <= ptr + 1'b1;
                else set_err(rx_ready ? E_RX_UNDERRUN : E_NOT_READY);
            end

            if (post_pending) begin
                h_post_seen <= h_post_sync[2];
                if (busy && g_post_id == cur_id) begin
                    busy     <= 1'b0;
                    rx_ready <= 1'b1;
                    rx_len   <= g_post_len;
                end
            end else if (busy && !h_svc) begin
                busy        <= 1'b0;
                set_err(E_SVC_LOST);
                h_abort_id  <= cur_id;
                h_abort_tog <= ~h_abort_tog;
            end

            if (wr) begin
                if (r == 3'd0) begin
                    if (!h_unlocked) begin
                        if (h_cpu_do == key_want) begin
                            key_idx <= key_idx + 2'd1;
                            if (key_idx == 2'd3) h_unlocked <= 1'b1;
                        end else
                            key_idx <= (h_cpu_do == 8'h53) ? 2'd1 : 2'd0;
                    end else if (h_cpu_do == 8'h00) begin
                        h_unlocked <= 1'b0;   // lock: the gclk side aborts any request
                        key_idx    <= 2'd0;
                        busy       <= 1'b0;
                        rx_ready   <= 1'b0;
                        err        <= E_NONE;
                        ptr        <= 0;
                    end
                end else if (h_unlocked) begin
                    case (r)
                        3'd2: case (h_cpu_do)
                            8'h00: err <= E_NONE;
                            8'h01: begin                                  // SEND
                                if (!h_svc)                     set_err(E_NOSVC);
                                else if (busy)                  set_err(E_BUSY);
                                else if (ptr == 0 || ptr > TX_SIZE) set_err(E_BADLEN);
                                else begin
                                    cur_id       <= cur_id + 8'd1;
                                    h_commit_id  <= cur_id + 8'd1;
                                    h_commit_len <= ptr[TX_AW:0];
                                    h_commit_tog <= ~h_commit_tog;
                                    busy         <= 1'b1;
                                    rx_ready     <= 1'b0;             // implicit RX_ACK
                                    ptr          <= 0;
                                end
                            end
                            8'h02: if (rx_ready) begin                    // RX_ACK
                                rx_ready    <= 1'b0;
                                h_rxack_id  <= cur_id;
                                h_rxack_tog <= ~h_rxack_tog;
                            end
                            8'h03: begin                                  // ABORT
                                rx_ready <= 1'b0;
                                if (busy) begin
                                    busy        <= 1'b0;
                                    h_abort_id  <= cur_id;
                                    h_abort_tog <= ~h_abort_tog;
                                end
                            end
                            default: set_err(E_BADCMD);
                        endcase
                        3'd4: ptr[7:0] <= h_cpu_do;
                        3'd5: ptr[PW-1:8] <= h_cpu_do[PW-9:0];
                        3'd6: begin
                            if (busy)                 set_err(E_BUSY);
                            else if (ptr >= TX_SIZE)  set_err(E_TX_OVERRUN);
                            else                      ptr <= ptr + 1'b1;
                        end
                        default: ;
                    endcase
                end
            end
        end
    end

    // =====================================================================
    // gclk side: UART packets
    // =====================================================================
    reg  [2:0] g_commit_sync = 3'd0, g_abort_sync = 3'd0, g_rxack_sync = 3'd0;
    reg        g_commit_seen = 1'b0, g_abort_seen = 1'b0, g_rxack_seen = 1'b0;
    reg  [1:0] g_unl_sync = 2'd0;
    reg        g_unl_prev = 1'b0;
    assign g_unlocked  = g_unl_sync[1];
    assign g_esp_awake = g_svc & g_awake;

    // request state
    reg            active = 1'b0;
    reg      [7:0] act_id = 8'd0;
    reg  [TX_AW:0] act_len = 0;
    reg            posted = 1'b0;
    reg  [RX_AW:0] rx_have = 0;
    reg            stream_on = 1'b0;
    reg  [TX_AW:0] stream_off = 0, stream_end = 0;

    // pending outgoing packets
    reg            p_state = 1'b0, p_abort = 1'b0, p_reply = 1'b0, p_commit = 1'b0, p_rxack = 1'b0;
    reg      [7:0] p_abort_id = 8'd0, p_rxack_id = 8'd0;
    reg      [7:0] p_reply_type = 8'd0, p_reply_id = 8'd0;
    reg     [15:0] p_reply_val = 16'd0;

    // the packet in the TX slot
    reg      [7:0] pkt_type = 8'd0, pkt_id = 8'd0;
    reg     [15:0] pkt_a = 16'd0;
    reg     [31:0] pkt_ext = 32'd0;
    reg            pkt_ram = 1'b0;
    reg  [TX_AW:0] pkt_off = 0;
    reg      [7:0] pkt_n = 8'd0;

    assign g_tx_len = 8'd4 + pkt_n;
    wire [7:0] ext_byte = (g_tx_bytepos == 8'd4) ? pkt_ext[7:0]   :
                          (g_tx_bytepos == 8'd5) ? pkt_ext[15:8]  :
                          (g_tx_bytepos == 8'd6) ? pkt_ext[23:16] : pkt_ext[31:24];
    assign g_tx_byte = (g_tx_bytepos == 8'd0) ? pkt_type :
                       (g_tx_bytepos == 8'd1) ? pkt_id :
                       (g_tx_bytepos == 8'd2) ? pkt_a[7:0] :
                       (g_tx_bytepos == 8'd3) ? pkt_a[15:8] :
                       pkt_ram ? tx_q : ext_byte;
    always @(*) tx_raddr = pkt_off[TX_AW-1:0] + {{(TX_AW-8){1'b0}}, g_tx_bytepos} - 4'd4;

    // UART RX sniffer
    localparam RS_IDLE = 3'd0, RS_ADDR = 3'd1, RS_LEN = 3'd2, RS_DATA = 3'd3, RS_CRC = 3'd4;
    reg      [2:0] rs = RS_IDLE;
    reg      [7:0] rs_crc = 8'hFF;
    reg            rs_mine = 1'b0;
    reg      [7:0] rs_len = 8'd0, rs_i = 8'd0;
    reg      [7:0] f_type = 8'd0, f_id = 8'd0, f_alo = 8'd0;
    reg     [15:0] f_a = 16'd0, f_b = 16'd0;
    reg            wr_ok = 1'b0;
    reg  [RX_AW:0] rx_wptr = 0;
    reg [WD_BITS-1:0] wd = 0;

    function [7:0] crc8;   // CRC-8 SAE J1850 step (poly 0x1D, no final xor), as uart_packet_wrapper_*
        input [7:0] c; input [7:0] d;
        integer k; reg [7:0] x;
        begin
            x = c ^ d;
            for (k = 0; k < 8; k = k + 1) x = x[7] ? ({x[6:0], 1'b0} ^ 8'h1D) : {x[6:0], 1'b0};
            crc8 = x;
        end
    endfunction

    wire [TX_AW:0] chunk     = g_link_busy ? CHUNK_SMALL : CHUNK;
    wire [TX_AW:0] remain    = stream_end - stream_off;
    wire [TX_AW:0] this_n    = (remain < chunk) ? remain : chunk;
    wire           req_match = active & ~posted & (f_id == act_id);

    always @(posedge gclk) begin
        g_commit_sync <= {g_commit_sync[1:0], h_commit_tog};
        g_abort_sync  <= {g_abort_sync[1:0],  h_abort_tog};
        g_rxack_sync  <= {g_rxack_sync[1:0],  h_rxack_tog};
        g_unl_sync    <= {g_unl_sync[0], h_unlocked};
        rx_we <= 1'b0;

        if (g_reset) begin
            g_commit_seen <= g_commit_sync[2];
            g_abort_seen  <= g_abort_sync[2];
            g_rxack_seen  <= g_rxack_sync[2];
            g_unl_prev <= 1'b0;
            active <= 1'b0; posted <= 1'b0; stream_on <= 1'b0;
            p_state <= 1'b0; p_abort <= 1'b0; p_reply <= 1'b0; p_commit <= 1'b0; p_rxack <= 1'b0;
            g_tx_valid <= 1'b0;
            g_svc <= 1'b0; g_online <= 1'b0; g_notify <= 1'b0; g_awake <= 1'b0;
            rs <= RS_IDLE;
            wd <= 0;
        end else begin
            // ---------------- service watchdog ----------------
            if (g_svc) begin
                if (&wd) begin
                    g_svc <= 1'b0; g_online <= 1'b0; g_notify <= 1'b0; g_awake <= 1'b0;
                end else
                    wd <= wd + 1'b1;
            end

            // ---------------- events from the GB side ----------------
            g_unl_prev <= g_unl_sync[1];
            if (g_unl_sync[1] != g_unl_prev) begin
                p_state <= 1'b1;
                if (!g_unl_sync[1] && active) begin          // locked / GB reset: abort
                    active <= 1'b0; stream_on <= 1'b0; p_commit <= 1'b0;
                    p_abort <= 1'b1; p_abort_id <= act_id;
                end
            end
            if (g_abort_sync[2] != g_abort_seen) begin
                g_abort_seen <= g_abort_sync[2];
                p_abort <= 1'b1; p_abort_id <= h_abort_id;
                if (active && h_abort_id == act_id) begin
                    active <= 1'b0; stream_on <= 1'b0; p_commit <= 1'b0;
                end
            end
            if (g_rxack_sync[2] != g_rxack_seen) begin
                g_rxack_seen <= g_rxack_sync[2];
                p_rxack <= 1'b1; p_rxack_id <= h_rxack_id;
            end
            if (g_commit_sync[2] != g_commit_seen) begin
                g_commit_seen <= g_commit_sync[2];
                active <= 1'b1; act_id <= h_commit_id; act_len <= h_commit_len;
                posted <= 1'b0; rx_have <= 0;
                p_commit <= 1'b1;
                stream_on <= 1'b1; stream_off <= 0; stream_end <= h_commit_len;
            end

            // ---------------- UART RX sniffer ----------------
            if (g_uart_rx_val) begin
                case (rs)
                    RS_IDLE: if (g_uart_rx_data == 8'h8F) begin
                        rs <= RS_ADDR; rs_crc <= crc8(8'hFF, 8'h8F);
                    end
                    RS_ADDR: begin
                        rs <= RS_LEN; rs_crc <= crc8(rs_crc, g_uart_rx_data);
                        rs_mine <= (g_uart_rx_data[6:0] == RX_ADDR);
                    end
                    RS_LEN: begin
                        rs_crc <= crc8(rs_crc, g_uart_rx_data);
                        rs_len <= g_uart_rx_data; rs_i <= 8'd0; wr_ok <= 1'b0;
                        f_type <= 8'd0;
                        rs <= (g_uart_rx_data == 8'd0) ? RS_CRC : RS_DATA;
                    end
                    RS_DATA: begin
                        rs_crc <= crc8(rs_crc, g_uart_rx_data);
                        rs_i <= rs_i + 8'd1;
                        if (rs_i + 8'd1 == rs_len) rs <= RS_CRC;
                        case (rs_i)
                            8'd0: f_type <= g_uart_rx_data;
                            8'd1: f_id   <= g_uart_rx_data;
                            8'd2: f_alo  <= g_uart_rx_data;
                            8'd3: begin
                                f_a <= {g_uart_rx_data, f_alo};
                                rx_wptr <= {g_uart_rx_data, f_alo};
                                wr_ok <= rs_mine && (f_type == R_RXDATA) && req_match &&
                                         ({g_uart_rx_data, f_alo} == rx_have);
                            end
                            8'd4: f_b[7:0]  <= g_uart_rx_data;
                            8'd5: f_b[15:8] <= g_uart_rx_data;
                            default: ;
                        endcase
                        // RXDATA payload goes straight into the RX buffer; it only
                        // counts (rx_have) once the CRC checks out.
                        if (rs_i >= 8'd4 && wr_ok && rx_wptr < RX_SIZE) begin
                            rx_we    <= 1'b1;
                            rx_waddr <= rx_wptr[RX_AW-1:0];
                            rx_wdata <= g_uart_rx_data;
                            rx_wptr  <= rx_wptr + 1'b1;
                        end
                    end
                    RS_CRC: begin
                        rs <= RS_IDLE;
                        if (rs_crc != g_uart_rx_data) begin
                            if (rs_mine) g_crc_errors <= g_crc_errors + 16'd1;
                        end else if (rs_mine) begin
                            wd <= 0;
                            case (f_type)
                                R_CONFIG: if (rs_len >= 8'd2) begin
                                    g_svc <= f_id[0]; g_online <= f_id[1];
                                    g_notify <= f_id[2]; g_awake <= f_id[3];
                                    p_state <= 1'b1;
                                end
                                R_RXDATA: if (rs_len >= 8'd4) begin
                                    if (wr_ok && req_match) rx_have <= rx_wptr;
                                    else if (req_match) begin
                                        p_reply <= 1'b1; p_reply_type <= T_RXNAK; p_reply_id <= act_id;
                                        p_reply_val <= rx_have;
                                    end
                                end
                                R_RXPOST: if (rs_len >= 8'd4) begin
                                    if (req_match && f_a <= rx_have) begin
                                        posted <= 1'b1;
                                        stream_on <= 1'b0;
                                        g_post_id <= act_id; g_post_len <= f_a[RX_AW:0];
                                        g_post_tog <= ~g_post_tog;
                                        p_reply <= 1'b1; p_reply_type <= T_POSTED; p_reply_id <= act_id;
                                        p_reply_val <= f_a;
                                    end else if (req_match) begin
                                        p_reply <= 1'b1; p_reply_type <= T_RXNAK; p_reply_id <= act_id;
                                        p_reply_val <= rx_have;
                                    end else
                                        p_state <= 1'b1;
                                end
                                R_TXREAD: if (rs_len >= 8'd6) begin
                                    if (active && f_id == act_id && f_a < act_len) begin
                                        stream_on  <= 1'b1;
                                        stream_off <= f_a[TX_AW:0];
                                        stream_end <= ({1'b0, f_a} + {1'b0, f_b} > act_len) ? act_len
                                                                                            : f_a[TX_AW:0] + f_b[TX_AW:0];
                                    end else
                                        p_state <= 1'b1;
                                end
                                R_STATUS: p_state <= 1'b1;
                                default: ;
                            endcase
                        end
                    end
                    default: rs <= RS_IDLE;
                endcase
            end

            // ---------------- TX slot ----------------
            if (g_tx_valid) begin
                if (g_tx_taken) g_tx_valid <= 1'b0;
            end else if (p_abort) begin              // first, so the ESP32 hears ABORT before the STATE that follows a lock
                p_abort <= 1'b0;
                pkt_type <= T_ABORT; pkt_id <= p_abort_id; pkt_a <= 16'd0;
                pkt_ram <= 1'b0; pkt_n <= 8'd0; g_tx_valid <= 1'b1;
            end else if (p_state) begin
                p_state <= 1'b0;
                pkt_type <= T_STATE; pkt_id <= act_id;
                pkt_a <= {NW_VERSION, 4'd0, stream_on, posted, active, g_unl_sync[1]};
                pkt_ext <= {{(15-RX_AW){1'b0}}, rx_have, {(15-TX_AW){1'b0}}, act_len};
                pkt_ram <= 1'b0; pkt_n <= 8'd4; g_tx_valid <= 1'b1;
            end else if (p_reply) begin
                p_reply <= 1'b0;
                pkt_type <= p_reply_type; pkt_id <= p_reply_id; pkt_a <= p_reply_val;
                pkt_ram <= 1'b0; pkt_n <= 8'd0; g_tx_valid <= 1'b1;
            end else if (p_commit) begin
                p_commit <= 1'b0;
                pkt_type <= T_COMMIT; pkt_id <= act_id; pkt_a <= act_len;
                pkt_ram <= 1'b0; pkt_n <= 8'd0; g_tx_valid <= 1'b1;
            end else if (p_rxack) begin
                p_rxack <= 1'b0;
                pkt_type <= T_RXACK; pkt_id <= p_rxack_id; pkt_a <= 16'd0;
                pkt_ram <= 1'b0; pkt_n <= 8'd0; g_tx_valid <= 1'b1;
            end else if (stream_on && active && !posted) begin
                pkt_type <= T_TXDATA; pkt_id <= act_id; pkt_a <= stream_off;
                pkt_off <= stream_off; pkt_ram <= 1'b1; pkt_n <= this_n[7:0];
                g_tx_valid <= 1'b1;
                stream_off <= stream_off + this_n;
                if (stream_off + this_n >= stream_end) stream_on <= 1'b0;
            end
        end
    end
endmodule
