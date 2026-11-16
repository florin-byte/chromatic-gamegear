// tb_nw_sysmon.sv: network window through the real system_monitor (UART byte
// level), i.e. the hookup in open_build/patches/net_window_integration.patch.
//
//   GB writes/reads (hclk) -> net_window -> system_monitor channel 11 ->
//   uart_packet_wrapper_tx -> UART2 BUSY model -> wire -> behavioural ESP32
//   ESP32 bytes -> uart_rx -> (system_monitor's stock RX wrapper + the window's sniffer)
//
// NEEDS THE INTEGRATION PATCH: system_monitor must have the nw_* ports. Before
// the patch is applied, point SYSMON at a patched copy:
//   iverilog -g2012 -o tb_nw_sysmon tb_nw_sysmon.sv ${SYSMON:-../src/rtl/BSP/system_monitor.sv} \
//       ../src/rtl/BSP/system_monitor_arbiter.sv ../src/rtl/BSP/uart_packet_wrapper_tx.sv \
//       ../src/rtl/BSP/uart_packet_wrapper_rx.sv ../src/rtl/BSP/net_window.v \
//       ../src/rtl/BSP/net_window_dpram.v && vvp tb_nw_sysmon
`timescale 1ns/1ps

module tb_nw_sysmon;
    reg clk = 0, hclk = 0;
    always #59.6 clk = ~clk;                   // gClk ~8.39 MHz
    always #29.83 hclk = ~hclk;                // hClk ~16.76 MHz, drifting against gClk
    reg reset = 1, h_reset = 1;

    // UART2 model (as tb_sysmon_link)
    localparam BIT = 72;
    reg  [7:0] rx_data = 0; reg rx_val = 0;
    wire [7:0] tx_data; wire tx_val;
    reg  u_busy_r = 0; reg [7:0] u_data = 0; integer u_cnt = 0;
    wire tx_busy = u_busy_r | tx_val;
    reg [7:0] txlog [0:65535]; integer txn = 0;
    integer dropped = 0;
    always @(posedge clk) begin
        if (tx_val && !u_busy_r) u_data <= tx_data;
        if (tx_val && u_busy_r) dropped = dropped + 1;
        if (u_cnt == 0) begin
            u_busy_r <= 0;
            if (tx_val) u_cnt <= 10 * BIT;
        end else if (u_cnt > BIT) begin
            u_busy_r <= 1;
            u_cnt <= u_cnt - 1;
            if (u_cnt == BIT + 1) begin txlog[txn] = u_data; txn = txn + 1; end
        end else begin
            u_busy_r <= 0;
            if (tx_val) u_cnt <= 10 * BIT;
            else u_cnt <= u_cnt - 1;
        end
    end

    reg  link_ev_valid = 0; reg [2:0] link_ev_type = 0; reg [7:0] link_ev_byte = 0;
    wire link_ev_taken, link_cmd_valid; wire [7:0] link_cmd, link_cmd_byte;
    reg  link_net_en = 0, link_esp_may_sleep = 0;
    always @(posedge clk) if (link_ev_taken) link_ev_valid <= 0;

    wire       nw_tx_valid, nw_tx_taken, nw_esp_awake, nw_unlocked;
    wire [7:0] nw_tx_len, nw_tx_byte, nw_tx_bytepos;

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
        .nw_tx_valid(nw_tx_valid), .nw_tx_len(nw_tx_len), .nw_tx_byte(nw_tx_byte),
        .nw_tx_bytepos(nw_tx_bytepos), .nw_tx_taken(nw_tx_taken), .nw_esp_awake(nw_esp_awake),
        .xs_tx_valid(1'b0), .xs_tx_len(8'd0), .xs_tx_byte(8'd0), .xs_tx_bytepos(), .xs_tx_taken(),
        .VERSION_DET(1'b0), .POWERED_BY_LIPO()
    );

    // GB bus: one write strobe per write, reads hold RD_n low for 4 hclk
    reg [15:0] cpu_addr = 16'h0100; reg [7:0] cpu_do = 0; reg cpu_rd_n = 1, cpu_wr_stb = 0;
    wire nw_sel; wire [7:0] nw_do;
    net_window u_nw (
        .hclk(hclk), .h_reset(h_reset),
        .h_cpu_addr(cpu_addr), .h_cpu_do(cpu_do), .h_cpu_rd_n(cpu_rd_n), .h_cpu_wr_stb(cpu_wr_stb),
        .h_sel(nw_sel), .h_do(nw_do),
        .gclk(clk), .g_reset(reset),
        .g_uart_rx_data(rx_data), .g_uart_rx_val(rx_val),
        .g_tx_valid(nw_tx_valid), .g_tx_len(nw_tx_len), .g_tx_bytepos(nw_tx_bytepos), .g_tx_byte(nw_tx_byte),
        .g_tx_taken(nw_tx_taken), .g_link_busy(link_net_en),
        .g_esp_awake(nw_esp_awake), .g_unlocked(nw_unlocked), .g_crc_errors());

    task gbw(input [15:0] a, input [7:0] v);
        begin
            @(posedge hclk); #1 cpu_addr = a; cpu_do = v;
            repeat (3) @(posedge hclk); #1 cpu_wr_stb = 1;
            @(posedge hclk); #1 cpu_wr_stb = 0;
            repeat (3) @(posedge hclk); #1 cpu_addr = 16'h0100;
        end
    endtask
    task gbr(input [15:0] a, output [7:0] v);
        begin
            @(posedge hclk); #1 cpu_addr = a; cpu_rd_n = 0;
            repeat (4) @(posedge hclk); v = nw_sel ? nw_do : 8'hFF;
            #1 cpu_rd_n = 1;
            repeat (3) @(posedge hclk); #1 cpu_addr = 16'h0100;
        end
    endtask

    function [7:0] crc8(input [7:0] c, input [7:0] d);
        integer i; reg [7:0] r;
        begin
            r = c ^ d;
            for (i = 0; i < 8; i = i + 1) r = r[7] ? ((r << 1) ^ 8'h1D) : (r << 1);
            crc8 = r;
        end
    endfunction
    task send_byte(input [7:0] b);
        begin
            @(posedge clk); rx_data <= b; rx_val <= 1;
            @(posedge clk); rx_val <= 0;
            repeat (10 * BIT - 2) @(posedge clk);
        end
    endtask
    reg [7:0] eo [0:255];
    task esp_send(input [6:0] addr, input integer n);
        integer i; reg [7:0] c;
        begin
            c = crc8(8'hFF, 8'h8F); c = crc8(c, {1'b0, addr}); c = crc8(c, n[7:0]);
            for (i = 0; i < n; i = i + 1) c = crc8(c, eo[i]);
            send_byte(8'h8F); send_byte({1'b0, addr}); send_byte(n[7:0]);
            for (i = 0; i < n; i = i + 1) send_byte(eo[i]);
            send_byte(c);
        end
    endtask

    integer errors = 0;
    task check(input cond, input [8*80-1:0] msg);
        begin
            if (!cond) begin errors = errors + 1; $display("FAIL: %0s", msg); end
            else $display("ok:   %0s", msg);
        end
    endtask

    // Parse txlog[from..txn) like the ESP32: count packets per address, check
    // CRCs, and rebuild TXDATA bytes into req[].
    integer n_nw, n_link, n_bad, n_pre, n_commit, n_txdata, n_state, req_bytes;
    reg [7:0] req [0:4095];
    task parse(input integer from);
        integer i, k, n; reg [7:0] c;
        begin
            n_nw = 0; n_link = 0; n_bad = 0; n_pre = 0; n_commit = 0; n_txdata = 0; n_state = 0; req_bytes = 0;
            i = from;
            while (i < txn) begin
                if (txlog[i] == 8'h00) begin n_pre = n_pre + 1; i = i + 1; end
                else if (txlog[i] == 8'h8F && i + 3 < txn) begin
                    n = txlog[i+2];
                    c = 8'hFF;
                    for (k = 0; k < 3 + n; k = k + 1) c = crc8(c, txlog[i+k]);
                    if (i + 3 + n < txn && c == txlog[i+3+n]) begin
                        if (txlog[i+1] == 8'h0B) begin
                            n_nw = n_nw + 1;
                            if (txlog[i+3] == 8'd1) n_commit = n_commit + 1;
                            if (txlog[i+3] == 8'd7) n_state = n_state + 1;
                            if (txlog[i+3] == 8'd2) begin
                                n_txdata = n_txdata + 1;
                                for (k = 4; k < n; k = k + 1) req[{txlog[i+6], txlog[i+5]} + k - 4] = txlog[i+3+k];
                                req_bytes = req_bytes + n - 4;
                            end
                        end else if (txlog[i+1] == 8'h0A) n_link = n_link + 1;
                        i = i + 4 + n;
                    end else begin n_bad = n_bad + 1; i = i + 1; end
                end else begin n_bad = n_bad + 1; i = i + 1; end
            end
        end
    endtask

    integer t0, i, ok; reg [7:0] v;
    initial begin
        repeat (20) @(posedge clk); reset = 0; h_reset = 0; repeat (200) @(posedge clk);

        // unlock; the window announces itself (STATE), with the wake preamble
        // (ESP32 may be asleep: no CONFIG yet)
        t0 = txn;
        gbw(16'hFF58, 8'h53); gbw(16'hFF58, 8'h47); gbw(16'hFF58, 8'h4E); gbw(16'hFF58, 8'h57);
        repeat (40000) @(posedge clk);
        parse(t0);
        check(n_state == 1 && n_bad == 0, "unlock -> one STATE packet on address 0x0B, CRC good");
        check(n_pre >= 19, "ESP32 not known awake: STATE preceded by the wake preamble");

        // ESP32 attaches, awake
        eo[0] = 8'd1; eo[1] = 8'h0B; esp_send(7'h11, 2);
        repeat (40000) @(posedge clk);
        gbr(16'hFF5B, v);
        check(v == 8'h18, "CONFIG through the real UART path: STAT = SVC|ONLINE");

        // 200-byte request, with a link event raised in the middle of the stream
        t0 = txn;
        gbw(16'hFF5C, 0); gbw(16'hFF5D, 0);
        for (i = 0; i < 200; i = i + 1) gbw(16'hFF5E, i[7:0] ^ 8'h5A);
        gbw(16'hFF5A, 8'h01);
        repeat (60000) @(posedge clk);
        @(posedge clk); link_ev_valid <= 1; link_ev_type <= 3'd3; link_ev_byte <= 8'h42;
        repeat (300000) @(posedge clk);
        parse(t0);
        ok = (req_bytes == 200);
        for (i = 0; i < 200; i = i + 1) if (req[i] !== (i[7:0] ^ 8'h5A)) ok = 0;
        $display("  wire: %0d window packets (%0d TXDATA), %0d link, %0d stray, %0d preamble, %0d UART drops",
                 n_nw, n_txdata, n_link, n_bad, n_pre, dropped);
        check(n_commit == 1 && n_txdata == 2 && ok, "COMMIT + TXDATA 128 + 72 carry the 200 bytes exactly");
        check(n_link == 1 && !link_ev_valid, "a link event raised mid-stream still goes out (channel 10)");
        check(n_bad == 0 && n_pre == 0 && dropped == 0, "no stray bytes, no preamble (CONFIG AWAKE), no UART drops");

        // response through the real RX path; the stock wrapper parses it too (address 0x11 is ignored there)
        eo[0] = 8'd2; eo[1] = 8'd1; eo[2] = 0; eo[3] = 0;
        for (i = 0; i < 100; i = i + 1) eo[4 + i] = 8'hC0 ^ i[7:0];
        esp_send(7'h11, 104);
        eo[0] = 8'd3; eo[1] = 8'd1; eo[2] = 100; eo[3] = 0; esp_send(7'h11, 4);
        repeat (20000) @(posedge clk);
        gbr(16'hFF5B, v);
        check(v[1] && !v[0], "RXDATA + RXPOST: RX_READY");
        gbw(16'hFF5C, 0); gbw(16'hFF5D, 0);
        ok = 1;
        for (i = 0; i < 100; i = i + 1) begin gbr(16'hFF5E, v); if (v !== (8'hC0 ^ i[7:0])) ok = 0; end
        check(ok, "game reads the 100-byte response");
        check(!link_cmd_valid, "system_monitor raised no link command for window packets");

        // ESP32 drops AWAKE: packets get the wake preamble again
        eo[0] = 8'd1; eo[1] = 8'h03; esp_send(7'h11, 2);
        repeat (40000) @(posedge clk);
        t0 = txn;
        gbw(16'hFF5A, 8'h02);                   // RX_ACK -> RXACK packet
        repeat (40000) @(posedge clk);
        parse(t0);
        check(n_nw == 1 && n_pre >= 19, "without AWAKE the next window packet carries the wake preamble");

        if (errors == 0) $display("PASS tb_nw_sysmon"); else $display("FAIL tb_nw_sysmon (%0d)", errors);
        $finish;
    end
endmodule
