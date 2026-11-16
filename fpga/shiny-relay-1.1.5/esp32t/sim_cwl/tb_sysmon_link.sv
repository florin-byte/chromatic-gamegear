// tb_sysmon_link.sv: system_monitor CWL1 paths at the UART byte level.
// - ESP32 -> FPGA: packet [8F, 10, 02, cmd, byte, crc] raises link_cmd_valid.
// - FPGA -> ESP32: a link event goes out once as [8F, 0A, 02, type, byte, crc],
//   with the sleep/wake preamble only when the network link is off.
`timescale 1ns/1ps

module tb_sysmon_link;
    reg clk = 0;
    always #59.6 clk = ~clk;                   // gClk ~8.39 MHz
    reg reset = 1;

    // UART model faithful to BSP/uart/uart_tx.vhd (UART2 at 115200 from gClk):
    //   BUSY = tx_busy | DATA_SEND, where tx_busy is a register that only goes
    //   high one cycle AFTER the byte is accepted (idle -> txsync), and goes low
    //   again for the whole stop bit (a new DATA_SEND there starts the next
    //   byte). tx_data latches on DATA_SEND & !tx_busy, so a DATA_SEND in the
    //   cycle after acceptance overwrites the byte, and one a cycle later is
    //   silently dropped. txlog records the bytes that reach the wire.
    localparam BIT = 72;                       // divider_value in uart.v
    reg  [7:0] rx_data = 0; reg rx_val = 0;
    wire [7:0] tx_data; wire tx_val;
    reg  u_busy_r = 0; reg [7:0] u_data = 0; integer u_cnt = 0;   // u_cnt: clocks left in the byte
    wire tx_busy = u_busy_r | tx_val;
    reg [7:0] txlog [0:4095]; integer txn = 0;
    integer dropped = 0;
    always @(posedge clk) begin
        if (tx_val && !u_busy_r) u_data <= tx_data;
        if (tx_val && u_busy_r) dropped = dropped + 1;
        if (u_cnt == 0) begin                  // idle
            u_busy_r <= 0;
            if (tx_val) u_cnt <= 10 * BIT;
        end else if (u_cnt > BIT) begin        // txsync .. data bits
            u_busy_r <= 1;
            u_cnt <= u_cnt - 1;
            if (u_cnt == BIT + 1) begin txlog[txn] = u_data; txn = txn + 1; end   // byte is on the wire
        end else begin                         // stop bit
            u_busy_r <= 0;
            if (tx_val) u_cnt <= 10 * BIT;
            else u_cnt <= u_cnt - 1;
        end
    end

    reg  link_ev_valid = 0; reg [2:0] link_ev_type = 0; reg [7:0] link_ev_byte = 0;
    wire link_ev_taken, link_cmd_valid; wire [7:0] link_cmd, link_cmd_byte;
    reg  link_net_en = 0;
    reg  link_esp_may_sleep = 0;

    system_monitor dut (
        .appear_off(1'b0), .clk(clk), .reset(reset),
        .BTN_A(1'b0), .BTN_B(1'b0), .BTN_DPAD_DOWN(1'b0), .BTN_DPAD_LEFT(1'b0),
        .BTN_DPAD_RIGHT(1'b0), .BTN_DPAD_UP(1'b0), .BTN_MENU(1'b1), .BTN_SEL(1'b0), .BTN_START(1'b0),
        .menuDisabled(), .ADC_SEL(), .hAdcReq_ext(), .LCD_INIT_DONE(1'b1), .LCD_PWM(),
        .LCD_BACKLIGHT_INIT(), .hAdcReady_r1(1'b0), .hAdcValue_r1(14'd2000), .hButtons(9'd0),
        .MCU_buttons(), .hVolume(7'd0), .pmic_sys_status(8'd0), .temperature(8'd0),
        .hHeadphones(1'b0), .gSecondEna(1'b0), .gHalfSecondEna(1'b0), .low_battery(),
        .LED_Green(), .LED_Red(), .LED_Yellow(), .LED_White(), .system_control(),
        .debug_system(), .paletteBGIn(), .paletteOBJ0In(), .paletteOBJ1In(), .gbc_color_temp(),
        .gbc_mode(1'b1), .gpd(64'd0),
        .uart_rx_data(rx_data), .uart_rx_val(rx_val), .uart_tx_busy(tx_busy),
        .uart_tx_data(tx_data), .uart_tx_val(tx_val),
        .link_ev_valid(link_ev_valid), .link_ev_type(link_ev_type), .link_ev_byte(link_ev_byte),
        .link_ev_taken(link_ev_taken), .link_cmd_valid(link_cmd_valid), .link_cmd(link_cmd),
        .link_cmd_byte(link_cmd_byte), .link_net_en(link_net_en), .link_esp_may_sleep(link_esp_may_sleep),
        .nw_tx_valid(1'b0), .nw_tx_len(8'd0), .nw_tx_byte(8'd0), .nw_tx_bytepos(), .nw_tx_taken(), .nw_esp_awake(1'b0),   // network window idle (18.21+)
        .xs_tx_valid(1'b0), .xs_tx_len(8'd0), .xs_tx_byte(8'd0), .xs_tx_bytepos(), .xs_tx_taken(),
        .VERSION_DET(1'b0), .POWERED_BY_LIPO()
    );

    function [7:0] crc8(input [7:0] c, input [7:0] d);
        integer i; reg [7:0] r;
        begin
            r = c ^ d;
            for (i = 0; i < 8; i = i + 1) r = r[7] ? ((r << 1) ^ 8'h1D) : (r << 1);
            crc8 = r;
        end
    endfunction


    // Link event source: hold until taken (like link_net_cdc).
    always @(posedge clk) if (link_ev_taken) link_ev_valid <= 0;

    integer errors = 0;
    task check(input cond, input [8*64-1:0] msg);
        begin
            if (!cond) begin errors = errors + 1; $display("FAIL: %0s", msg); end
            else $display("ok:   %0s", msg);
        end
    endtask

    task send_byte(input [7:0] b);
        begin
            @(posedge clk); rx_data <= b; rx_val <= 1;
            @(posedge clk); rx_val <= 0;
            repeat (30) @(posedge clk);         // CRC engine needs 8+ clocks per byte
        end
    endtask

    task send_cmd(input [7:0] c, input [7:0] b);
        reg [7:0] crc;
        begin
            crc = 8'hFF;
            crc = crc8(crc, 8'h8F); crc = crc8(crc, 8'h10); crc = crc8(crc, 8'h02);
            crc = crc8(crc, c); crc = crc8(crc, b);
            send_byte(8'h8F); send_byte(8'h10); send_byte(8'h02); send_byte(c); send_byte(b); send_byte(crc);
        end
    endtask

    // Count well-formed link packets in txlog[from..txn) and check payloads.
    integer found, preamble;
    task scan_link_packets(input integer from, input [2:0] t, input [7:0] b);
        integer i; reg [7:0] crc;
        begin
            found = 0; preamble = 0;
            for (i = from; i < txn; i = i + 1) begin
                if (txlog[i] == 8'h00) preamble = preamble + 1;
                if (i + 5 < txn + 1 && txlog[i] == 8'h8F && txlog[i+1] == 8'h0A && txlog[i+2] == 8'h02) begin
                    crc = 8'hFF;
                    crc = crc8(crc, 8'h8F); crc = crc8(crc, 8'h0A); crc = crc8(crc, 8'h02);
                    crc = crc8(crc, txlog[i+3]); crc = crc8(crc, txlog[i+4]);
                    if (txlog[i+3] == {5'd0, t} && txlog[i+4] == b && txlog[i+5] == crc) found = found + 1;
                    else $display("  bad link packet at %0d: %02h %02h %02h", i, txlog[i+3], txlog[i+4], txlog[i+5]);
                end
            end
        end
    endtask

    reg got_cmd = 0; reg [7:0] got_c, got_b;
    always @(posedge clk) if (link_cmd_valid) begin got_cmd <= 1; got_c <= link_cmd; got_b <= link_cmd_byte; end

    integer t0;
    initial begin
        repeat (20) @(posedge clk); reset = 0; repeat (200) @(posedge clk);

        // RX: MASTER_REPLY(0x55)
        send_cmd(8'd1, 8'h55);
        repeat (20) @(posedge clk);
        check(got_cmd && got_c == 8'd1 && got_b == 8'h55, "RX 0x10 -> link_cmd(1, 55)");

        // TX with network link off: one packet, after the 0x00 wake preamble.
        t0 = txn;
        @(posedge clk); link_ev_valid <= 1; link_ev_type <= 3'd5; link_ev_byte <= 8'h29;
        repeat (400000) @(posedge clk);
        scan_link_packets(t0, 3'd5, 8'h29);
        check(found == 1, "net off: exactly one SLAVE_NAK(29) packet (3-bit type)");
        check(preamble >= 19, "net off: wake preamble sent");
        check(!link_ev_valid, "event taken");

        // TX with network link on: one packet, no preamble.
        link_net_en = 1;
        t0 = txn;
        @(posedge clk); link_ev_valid <= 1; link_ev_type <= 3'd1; link_ev_byte <= 8'h8F;
        repeat (400000) @(posedge clk);
        scan_link_packets(t0, 3'd1, 8'h8F);
        begin : dump integer i; $write("  net-on bytes:"); for (i = t0; i < txn; i = i + 1) $write(" %02h", txlog[i]); $display(""); end
        check(found == 1, "net on: exactly one ARMED(8F) packet");
        check(preamble == 0, "net on: no wake preamble");

        // Link events at every gClk phase relative to the wrapper's 16-cycle
        // byte pacing: each must arrive as one intact packet (an event raised
        // at the wrong phase used to lose its address byte to the UART BUSY gap).
        begin : sweep
            integer k, i, n, good, bad; reg [7:0] crc;
            t0 = txn;
            for (k = 0; k < 48; k = k + 1) begin
                @(posedge clk); repeat (k) @(posedge clk);
                link_ev_valid <= 1; link_ev_type <= 3'd3; link_ev_byte <= 8'h80 + k;
                @(posedge clk); wait (!link_ev_valid);
                repeat (10 * BIT + 50) @(posedge clk);
            end
            // Parse the wire like the ESP32 (fpga_rx.c, V2 framing).
            good = 0; bad = 0; i = t0;
            while (i < txn) begin
                n = (i + 2 < txn) ? txlog[i+2] : 0;
                if (txlog[i] == 8'h8F && i + 3 + n < txn + 1 && n >= 1 && n <= 10) begin
                    crc = 8'hFF;
                    for (k = 0; k < 3 + n; k = k + 1) crc = crc8(crc, txlog[i+k]);
                    if (crc == txlog[i+3+n]) begin
                        if (txlog[i+1] == 8'h0A) begin
                            if (n == 2 && txlog[i+3] == 8'd3 && txlog[i+4] == 8'h80 + good) good = good + 1;
                            else bad = bad + 1;
                        end
                        i = i + 4 + n;
                    end else begin bad = bad + 1; i = i + 1; end
                end else begin bad = bad + 1; i = i + 1; end
            end
            $display("  phase sweep: %0d/48 intact packets, %0d stray bytes, %0d bytes dropped by the UART", good, bad, dropped);
            check(good == 48 && bad == 0 && dropped == 0, "net on: 48 MASTER events at every phase arrive intact");
        end

        // Network link on while the ESP32 may light-sleep (CWL dormant): the
        // stock wake preamble comes back, so the first event is not lost.
        link_esp_may_sleep = 1;
        repeat (2000) @(posedge clk);
        t0 = txn;
        @(posedge clk); link_ev_valid <= 1; link_ev_type <= 3'd3; link_ev_byte <= 8'h42;
        repeat (400000) @(posedge clk);
        scan_link_packets(t0, 3'd3, 8'h42);
        check(found == 1, "net on, ESP may sleep: exactly one MASTER(42) packet");
        check(preamble >= 19, "net on, ESP may sleep: wake preamble sent");
        link_esp_may_sleep = 0;
        repeat (2000) @(posedge clk);
        t0 = txn;
        @(posedge clk); link_ev_valid <= 1; link_ev_type <= 3'd1; link_ev_byte <= 8'h11;
        repeat (400000) @(posedge clk);
        scan_link_packets(t0, 3'd1, 8'h11);
        check(found == 1 && preamble == 0, "ESP awake again: one ARMED(11) packet, no preamble");

        if (errors == 0) $display("ALL PASSED"); else $display("%0d FAILED", errors);
        $finish;
    end
endmodule
