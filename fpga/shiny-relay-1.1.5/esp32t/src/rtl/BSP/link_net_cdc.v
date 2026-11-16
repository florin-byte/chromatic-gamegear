// link_net_cdc.v
//
// CWL1 network link: carries link events from the GB core (hclk) to the
// system monitor UART (gclk), and ESP32 commands the other way.
//
// Commands arrive at most once per UART packet (hundreds of microseconds
// apart), so a toggle handshake with the payload held stable is enough.
// Events can be produced back to back by the CPU and each takes a ~0.5 ms
// UART packet, so the hclk side queues up to 16 and hands them over one at a
// time. Pokemon Gold/Silver's partner search writes SC=0x00, SC=0x80, SB=1,
// SC=0x01, SC=0x81 within ~20 us (DISARM ARMED ARMED DISARM MASTER): with
// the old 4-deep queue the MASTER was dropped, the ESP32 never heard of the
// held master, ignored its ABANDON, and the CPU sat stalled until the ~4 s
// safety timeout completed it with 0xFF (the game then read its own parked
// SB=2 as the partner's and "connected" alone). ARMED / DISARM are only hints
// (the port decides every offer), so when the queue is nearly full they are
// dropped to keep room for the events that must arrive.

module link_net_cdc (
    input            gclk,
    input            g_reset,          // active high, gclk domain
    input            hclk,
    input            h_reset,          // active high, hclk domain

    // gclk side: ESP32 command (from system_monitor RX address 0x10)
    input            g_cmd_valid,      // one-cycle pulse
    input      [7:0] g_cmd,            // 1 MASTER_REPLY, 2 SLAVE_XFER, 3 CONFIG (bit0 net_en, bit1 ESP may sleep, bit2 write barrier), 4 MASTER_DROP, 5 CART_POWER_CYCLE (debug), 6 SLAVE_WITHDRAW, 9 ENGINE_ALLOW (byte bit0: the cart bus engine may take the cart bus), 10 BOOT_SELECT (byte bit0: 1 = alternate netboot image); 7 and 8 unused (18.23's slave lock numbers); CONFIG bit3 standing offers
    input      [7:0] g_cmd_byte,

    // gclk side: next event for system_monitor TX channel 10
    output reg       g_ev_valid,       // high until g_ev_taken
    output reg [2:0] g_ev_type,
    output reg [7:0] g_ev_byte,
    input            g_ev_taken,       // one-cycle pulse after the packet is sent
    output           g_net_en,         // net_en, as seen by the gclk side
    output           g_esp_may_sleep,  // CONFIG bit 1: the ESP32 may be in light sleep, wake it first

    // hclk side: to/from link.v
    output reg       h_net_en,
    output reg       h_master_reply_pend,
    output reg [7:0] h_master_reply_byte,
    output reg       h_master_reply_drop,  // MASTER_DROP: release the held master without completing it
    output reg       h_barrier_en,         // CONFIG bit 2: the ESP32 handles ABANDON / MASTER_DROP / event 7
    input            h_master_reply_ack,
    output reg       h_slave_xfer_pend,
    output reg [7:0] h_slave_xfer_byte,
    input            h_slave_xfer_ack,
    output reg       h_cart_cycle,
    output reg       h_standing_en,        // CONFIG bit 3: standing offers (18.20)
    output reg       h_slave_withdraw_pend,// cmd 6: take back the standing offer
    input            h_slave_withdraw_ack,         // cmd 5: one-cycle pulse, power-cycle the cartridge (debug: back to a flash cart's menu)
    output reg       h_eng_allow,          // cmd 9: cart bus engine allowed (cart jobs only); cleared at FPGA reset
    output reg       h_boot_alt,           // cmd 10: boot image select (cart jobs); kept across GB / cart resets, cleared at FPGA reset
    input            h_ev_valid,
    input      [2:0] h_ev_type,
    input      [7:0] h_ev_byte
);

    // ---------------- commands: gclk -> hclk ----------------
    reg       g_cmd_tog = 1'b0;
    reg [7:0] g_cmd_r = 8'd0;
    reg [7:0] g_cmd_byte_r = 8'd0;
    reg       g_net_en_r = 1'b0;
    reg       g_may_sleep_r = 1'b0;
    assign g_net_en = g_net_en_r;
    assign g_esp_may_sleep = g_may_sleep_r;

    always @(posedge gclk or posedge g_reset) begin
        if (g_reset) begin
            g_cmd_tog     <= 1'b0;
            g_net_en_r    <= 1'b0;
            g_may_sleep_r <= 1'b0;
        end else if (g_cmd_valid) begin
            g_cmd_r      <= g_cmd;
            g_cmd_byte_r <= g_cmd_byte;
            g_cmd_tog    <= ~g_cmd_tog;
            if (g_cmd == 8'd3) begin
                g_net_en_r    <= g_cmd_byte[0];
                g_may_sleep_r <= g_cmd_byte[1];
            end
        end
    end

    reg [2:0] h_cmd_sync = 3'd0;
    wire h_cmd_new = h_cmd_sync[2] ^ h_cmd_sync[1];

    always @(posedge hclk or posedge h_reset) begin
        if (h_reset) begin
            h_cmd_sync          <= 3'd0;
            h_net_en            <= 1'b0;
            h_master_reply_pend <= 1'b0;
            h_master_reply_drop <= 1'b0;
            h_barrier_en        <= 1'b0;
            h_slave_xfer_pend   <= 1'b0;
            h_cart_cycle        <= 1'b0;
            h_standing_en       <= 1'b0;
            h_slave_withdraw_pend <= 1'b0;
            h_eng_allow         <= 1'b0;
            h_boot_alt          <= 1'b0;
        end else begin
            h_cmd_sync <= {h_cmd_sync[1:0], g_cmd_tog};
            h_cart_cycle <= 1'b0;

            if (h_master_reply_ack) h_master_reply_pend <= 1'b0;
            if (h_slave_xfer_ack)   h_slave_xfer_pend   <= 1'b0;
            if (h_slave_withdraw_ack) h_slave_withdraw_pend <= 1'b0;

            if (h_cmd_new) begin
                case (g_cmd_r)
                    8'd1: begin
                        h_master_reply_pend <= 1'b1;
                        h_master_reply_byte <= g_cmd_byte_r;
                        h_master_reply_drop <= 1'b0;
                    end
                    8'd4: begin                        // abandoned master, peer did not exchange
                        h_master_reply_pend <= 1'b1;
                        h_master_reply_drop <= 1'b1;
                    end
                    8'd2: begin
                        h_slave_xfer_pend <= 1'b1;
                        h_slave_xfer_byte <= g_cmd_byte_r;
                    end
                    8'd3: begin
                        h_net_en     <= g_cmd_byte_r[0];
                        h_barrier_en <= g_cmd_byte_r[0] & g_cmd_byte_r[2];
                        h_standing_en <= g_cmd_byte_r[0] & g_cmd_byte_r[3];
                        if (!g_cmd_byte_r[0]) begin
                            h_master_reply_pend <= 1'b0;
                            h_slave_xfer_pend   <= 1'b0;
                            h_slave_withdraw_pend <= 1'b0;
                        end
                    end
                    8'd5: h_cart_cycle <= 1'b1;
                    8'd6: h_slave_withdraw_pend <= 1'b1;
                    8'd9:  h_eng_allow <= g_cmd_byte_r[0];
                    8'd10: h_boot_alt  <= g_cmd_byte_r[0];
                    default: ;
                endcase
            end
        end
    end

    // ---------------- events: hclk -> gclk ----------------
    reg [10:0] h_fifo [0:15];
    reg [3:0] h_wr = 4'd0;
    reg [3:0] h_rd = 4'd0;
    reg [4:0] h_count = 5'd0;
    reg       h_busy = 1'b0;          // an event is handed over, waiting for ack
    reg       h_ev_tog = 1'b0;
    reg [10:0] h_ev_hold = 11'd0;
    reg [2:0] h_ack_sync = 3'd0;
    reg       g_ack_tog = 1'b0;
    wire      h_ack_new = h_ack_sync[2] ^ h_ack_sync[1];

    // 1 ARMED / 2 DISARM are hints: keep the last 4 slots for the others.
    wire h_ev_hint = (h_ev_type == 3'd1) || (h_ev_type == 3'd2);
    wire h_push = h_ev_valid && (h_ev_hint ? (h_count < 5'd12) : (h_count != 5'd16));
    wire h_send = !h_busy && (h_count != 5'd0);

    always @(posedge hclk or posedge h_reset) begin
        if (h_reset) begin
            h_wr       <= 4'd0;
            h_rd       <= 4'd0;
            h_count    <= 5'd0;
            h_busy     <= 1'b0;
            h_ev_tog   <= 1'b0;
            h_ack_sync <= 3'd0;
        end else begin
            h_ack_sync <= {h_ack_sync[1:0], g_ack_tog};

            if (h_push) begin
                h_fifo[h_wr] <= {h_ev_type, h_ev_byte};
                h_wr <= h_wr + 4'd1;
            end
            if (h_send) begin
                h_ev_hold <= h_fifo[h_rd];
                h_rd      <= h_rd + 4'd1;
                h_ev_tog  <= ~h_ev_tog;
                h_busy    <= 1'b1;
            end else if (h_busy && h_ack_new) begin
                h_busy <= 1'b0;
            end
            h_count <= h_count + (h_push ? 5'd1 : 5'd0) - (h_send ? 5'd1 : 5'd0);
        end
    end

    reg [2:0] g_ev_sync = 3'd0;
    wire g_ev_new = g_ev_sync[2] ^ g_ev_sync[1];

    always @(posedge gclk or posedge g_reset) begin
        if (g_reset) begin
            g_ev_sync  <= 3'd0;
            g_ev_valid <= 1'b0;
            g_ack_tog  <= 1'b0;
        end else begin
            g_ev_sync <= {g_ev_sync[1:0], h_ev_tog};
            if (g_ev_new) begin
                g_ev_valid <= 1'b1;
                g_ev_type  <= h_ev_hold[10:8];
                g_ev_byte  <= h_ev_hold[7:0];
            end else if (g_ev_valid && g_ev_taken) begin
                g_ev_valid <= 1'b0;
                g_ack_tog  <= ~g_ack_tog;
            end
        end
    end

endmodule
