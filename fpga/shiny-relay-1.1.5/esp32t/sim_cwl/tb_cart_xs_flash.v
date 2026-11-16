// tb_cart_xs_flash.v (lane fpgacomp1, FPGA 18.31): cart_xs's boot flash ops against a SPI NOR model (W25Q32 subset:
// 9F, 05, 06, 03, 02, 20, D8), with the 18.31 guards (FLASH_W_BASE 0x090000, FLASH_H_TOP 0x7C0, no FHDRERASE) and
// FREAD (op 22) read back through the flash read window (q_win_addr / q_fwin, as qspi_slave reads it).
// Run: iverilog -g2012 -o tb_cart_xs_flash tb_cart_xs_flash.v ../src/rtl/BSP/cart_xs.v ../src/rtl/BSP/net_window_dpram.v && vvp tb_cart_xs_flash
`timescale 1ns/1ps
module tb_cart_xs_flash;
    reg clk = 0;  always #29.8 clk = ~clk;                    // hclk 16.78 MHz
    reg gclk = 0; always @(posedge clk) gclk <= ~gclk;
    reg qclk = 0; always #12.5 qclk = ~qclk;                   // the QSPI clock (window reads)
    reg reset = 1, g_reset = 1;
    integer UART_GAP = 80, TX_GAP = 8;

    // ---------------------------------------------------------------- the boot flash (4 MiB SPI NOR, mode 0)
    localparam SIZE = 4194304;
    reg [7:0] flash [0:SIZE-1];
    wire f_sck, f_cs_n, f_mosi; reg f_miso = 1'b1;
    reg [7:0] sh = 0; integer nb = 0, bits = 0; reg [7:0] cmd = 0; reg [23:0] fa = 0; reg wel = 0; integer busy_until = 0;
    integer n_erase = 0, n_prog = 0;
    reg [7:0] rd_byte = 8'hFF;
    always @(negedge f_cs_n) begin nb = 0; bits = 0; end
    always @(posedge f_cs_n) begin
        if (cmd == 8'h06 && nb == 1) wel = 1;
        if ((cmd == 8'h20 || cmd == 8'hD8) && nb == 4 && wel) begin : er
            integer i, base, len;
            len = (cmd == 8'h20) ? 4096 : 65536; base = fa & ~(len - 1);
            for (i = 0; i < len; i = i + 1) flash[base + i] = 8'hFF;
            wel = 0; n_erase = n_erase + 1; busy_until = $time + 20000;
        end
        if (cmd == 8'h02 && nb > 4) begin wel = 0; busy_until = $time + 2000; end
    end
    always @(posedge f_sck) if (!f_cs_n) begin
        sh = {sh[6:0], f_mosi}; bits = bits + 1;
        if (bits == 8) begin
            bits = 0;
            if (nb == 0) cmd = sh;
            else if (nb <= 3) fa = {fa[15:0], sh};
            else if (cmd == 8'h02 && wel) begin flash[{fa[23:8], 8'h00} | ((fa[7:0] + nb - 4) & 8'hFF)] = flash[{fa[23:8], 8'h00} | ((fa[7:0] + nb - 4) & 8'hFF)] & sh; n_prog = n_prog + 1; end
            nb = nb + 1;
        end
    end
    // MISO: after the 8 command clocks (9F, 05) or the 32 command + address clocks (03), a new byte every 8 rising
    // edges, its bits put out on the falling edges (mode 0)
    integer rises = 0; reg [7:0] osh = 8'hFF;
    always @(negedge f_cs_n) rises = 0;
    always @(posedge f_sck) if (!f_cs_n) rises = rises + 1;
    always @(negedge f_sck) if (!f_cs_n) begin : mo
        integer hdr, k;
        hdr = (cmd == 8'h03) ? 32 : 8;
        if (rises >= hdr && (rises - hdr) % 8 == 0) begin
            k = (rises - hdr) / 8;
            if (rises < 8) osh = 8'hFF;
            else if (cmd == 8'h9F) osh = (k == 0) ? 8'hEF : (k == 1) ? 8'h40 : 8'h16;
            else if (cmd == 8'h05) osh = ($time < busy_until) ? 8'h01 : 8'h00;
            else if (cmd == 8'h03) osh = flash[(fa + k) & (SIZE - 1)];
            else osh = 8'hFF;
        end
        f_miso = osh[7]; osh = {osh[6:0], 1'b1};
    end

    // ---------------------------------------------------------------- DUT
    reg  [7:0] urx = 8'd0; reg urx_val = 1'b0;
    wire g_tx_valid; wire [7:0] g_tx_len, g_tx_byte; reg [7:0] g_tx_bytepos = 8'd0; reg g_tx_taken = 1'b0;
    wire x_bus_req; reg x_bus_done = 0; always @(posedge clk) x_bus_done <= x_bus_req & ~x_bus_done;
    reg [8:0] win_a = 0; wire [15:0] win_q; wire win_sel;
    wire r_wread_tog;
    // the RAM cart's PSRAM port for FCOPY: busy a few clocks after each word
    wire wtag_on; wire [15:0] wtag;
    wire cw_go; wire [22:0] cw_addr; wire [15:0] cw_data; reg [3:0] cw_busy = 0;
    wire cw_ready = (cw_busy == 0);
    reg [15:0] psram [0:4194303];
    integer n_cw = 0;
    always @(posedge clk) begin
        if (cw_busy) cw_busy <= cw_busy - 1;
        if (cw_go) begin if (!cw_ready) $display("FAIL: a word while busy"); psram[cw_addr[22:1]] <= cw_data; cw_busy <= 4'd7; n_cw = n_cw + 1; end
    end
    cart_xs #(.ERASE_TMO(4000000), .FLASH_W_BASE(24'h05C000), .FLASH_H_TOP(24'h0007C0), .HDRERASE_OK(1'b0), .HAS_FCOPY(1'b1)) dut (
        .qspi_clk_n(~qclk), .q_addr_ready(1'b0), .q_address(32'd0), .q_byte_valid(1'b0), .q_byte(8'd0),
        .gclk(gclk), .g_reset(g_reset), .g_uart_rx_data(urx), .g_uart_rx_val(urx_val),
        .g_tx_valid(g_tx_valid), .g_tx_len(g_tx_len), .g_tx_bytepos(g_tx_bytepos), .g_tx_byte(g_tx_byte), .g_tx_taken(g_tx_taken),
        .hclk(clk), .h_reset(reset), .h_allow(1'b0), .h_eng_busy(1'b0),
        .x_session(), .x_hold(), .x_bus_req(x_bus_req), .x_bus_wr(), .x_bus_a(), .x_bus_d(), .x_bus_done(x_bus_done), .x_bus_q(8'hFF),
        .x_tcfg(), .x_t_setup(), .x_t_pulse(), .x_t_hold(), .x_t_access(), .x_t_gap(),
        .f_sck(f_sck), .f_cs_n(f_cs_n), .f_mosi(f_mosi), .f_miso(f_miso),
        .r_on(), .r_hold(), .r_mbc(), .r_rom_mask(), .r_ram_mask(), .r_ram_has(), .r_rtc_has(), .r_inval_tog(), .r_dirty_clr(),
        .r_dirty(1'b0), .r_busy(1'b0), .r_wread_tog(r_wread_tog), .r_wread_addr(), .r_wread_len(), .r_wread_done_tog(r_wread_tog),
        .r_rtc_set(), .r_rtc_in(), .r_rtc_out(40'd0), .r_dbg(48'd0), .r_density(3'd3), .h_cart_in(1'b0),
        .q_win_addr(win_a), .q_fwin(win_q), .h_fwin_sel(win_sel),
        .r_cw_ready(cw_ready), .r_cw_go(cw_go), .r_cw_addr(cw_addr), .r_cw_data(cw_data),
        .h_wtag_on(wtag_on), .h_wtag(wtag), .r_wread_off());

    integer ok = 0, bad = 0;
    task check(input c, input [8*96-1:0] what);
        begin if (c) ok = ok + 1; else begin bad = bad + 1; $display("FAIL %0s", what); end end
    endtask
    function [31:0] crc32_upd(input [31:0] c, input [7:0] d);
        integer k; reg [31:0] x;
        begin x = c ^ {24'd0, d}; for (k = 0; k < 8; k = k + 1) x = x[0] ? ((x >> 1) ^ 32'hEDB88320) : (x >> 1); crc32_upd = x; end
    endfunction
    function [7:0] crc8(input [7:0] c, input [7:0] d);
        integer k; reg [7:0] x;
        begin x = c ^ d; for (k = 0; k < 8; k = k + 1) x = x[7] ? ({x[6:0], 1'b0} ^ 8'h1D) : {x[6:0], 1'b0}; crc8 = x; end
    endfunction
    function [31:0] flash_crc(input integer f, input integer n);
        integer i; reg [31:0] c;
        begin c = 32'hFFFFFFFF; for (i = 0; i < n; i = i + 1) c = crc32_upd(c, flash[f + i]); flash_crc = ~c; end
    endfunction

    // UART ESP32 -> FPGA
    reg [7:0] pk [0:15];
    task uart_byte(input [7:0] b);
        begin @(posedge gclk); urx <= b; urx_val <= 1'b1; @(posedge gclk); urx_val <= 1'b0; repeat (UART_GAP - 1) @(posedge gclk); end
    endtask
    task uart_pkt(input integer len);
        integer i; reg [7:0] c;
        begin
            c = crc8(8'hFF, 8'h8F); uart_byte(8'h8F);
            c = crc8(c, 8'h12); uart_byte(8'h12);
            c = crc8(c, len[7:0]); uart_byte(len[7:0]);
            for (i = 0; i < len; i = i + 1) begin c = crc8(c, pk[i]); uart_byte(pk[i]); end
            uart_byte(c);
        end
    endtask
    reg [7:0] seq = 8'd0;
    task op(input [7:0] o, input [23:0] f, input [23:0] n, input [7:0] arg);
        begin
            seq = seq + 8'd1;
            pk[0] = 8'd1; pk[1] = seq; pk[2] = o; pk[3] = f[7:0]; pk[4] = f[15:8]; pk[5] = f[23:16];
            pk[6] = n[7:0]; pk[7] = n[15:8]; pk[8] = n[23:16]; pk[9] = arg;
            uart_pkt(10);
        end
    endtask
    task simple(input [7:0] t); begin pk[0] = t; uart_pkt(1); end endtask

    // status (system monitor channel 12)
    reg [7:0] stb [0:31];
    reg [7:0] s_flags, s_done, s_err, s_q; reg [31:0] s_crc; reg [23:0] s_f;
    integer n_status = 0;
    always @(posedge gclk) begin : txmodel
        integer i;
        if (g_tx_valid && !g_tx_taken) begin
            for (i = 0; i < 32; i = i + 1) begin g_tx_bytepos <= i; @(posedge gclk); @(posedge gclk); stb[i] = g_tx_byte; repeat (TX_GAP) @(posedge gclk); end
            g_tx_taken <= 1'b1; @(posedge gclk); g_tx_taken <= 1'b0;
            s_flags = stb[1]; s_done = stb[2]; s_err = stb[3]; s_crc = {stb[15], stb[14], stb[13], stb[12]}; s_f = {stb[18], stb[17], stb[16]}; s_q = stb[19];
            n_status = n_status + 1;
        end
    end
    // an op and its end (done = its seq, not running) or an error
    task run(input [7:0] o, input [23:0] f, input [23:0] n, input [7:0] arg);
        integer t;
        begin
            op(o, f, n, arg);
            t = 0;
            while (!((s_done == seq && !s_flags[1]) || s_err != 0) && t < 2000000) begin @(posedge gclk); t = t + 1; end
            repeat (200) @(posedge gclk);
        end
    endtask
    task reset_xs; begin simple(8'd5); repeat (3000) @(posedge gclk); simple(8'd4); repeat (6000) @(posedge gclk); end endtask
    // the window as the QSPI read sees it: n bytes, low byte of each word first
    reg [7:0] wbuf [0:1023];
    task win_read(input integer n);
        integer i;
        begin for (i = 0; i < n; i = i + 2) begin @(posedge qclk); win_a = i / 2; @(negedge qclk); @(negedge qclk); {wbuf[i + 1], wbuf[i]} = win_q; end end
    endtask
    function [31:0] wcrc(input integer n);
        integer i; reg [31:0] c;
        begin c = 32'hFFFFFFFF; for (i = 0; i < n; i = i + 1) c = crc32_upd(c, wbuf[i]); wcrc = ~c; end
    endfunction

    integer i, k, f0, n0, nbad;
    initial begin
        for (i = 0; i < SIZE; i = i + 1) flash[i] = 8'hFF;
        for (i = 24'h2F0000; i < 24'h300000; i = i + 1) flash[i] = (i * 13 + (i >> 9)) & 8'hFF;
        for (i = 24'h001000; i < 24'h002000; i = i + 1) flash[i] = 8'h5A;
        repeat (20) @(posedge clk); reset = 0; g_reset = 0; repeat (20) @(posedge clk);
        simple(8'd4); repeat (6000) @(posedge gclk);          // a first status (the bench's copy starts as x)

        // FREAD again and again (the unit 1 symptom: every second one refused)
        for (k = 0; k < 6; k = k + 1) begin
            f0 = (k < 3) ? 24'h2F0000 : 24'h2F0000 + 1024 * (k - 2);
            n0 = (k == 1) ? 64 : (k == 4) ? 2 : 1024;
            run(8'd22, f0, n0, 8'd0);
            $display("  k=%0d f %06x n %0d: err %0d done %02x seq %02x flags %02x crc %08x want %08x", k, f0, n0, s_err, s_done, seq, s_flags, s_crc, flash_crc(f0, n0));
            check(s_err == 0, "FREAD: no error");
            check(s_crc == flash_crc(f0, n0), "FREAD: the crc of the flash bytes");
            check(win_sel, "FREAD: the window is FREAD's");
            win_read(n0);
            check(wcrc(n0) == s_crc, "FREAD: the window holds the bytes");
            if (s_err != 0) begin $display("  k=%0d err %0d f %06x", k, s_err, s_f); reset_xs; end
        end
        run(8'd19, 24'h2F0000, 24'd4096, 8'd0);
        check(s_err == 0 && s_crc == flash_crc(24'h2F0000, 4096), "FCRC after FREADs");
        run(8'd22, 24'h3FFC00, 24'd1024, 8'd0);
        check(s_err == 0, "FREAD of the flash's last KiB");
        run(8'd22, 24'h3FFC02, 24'd1024, 8'd0);
        check(s_err == 8'd6, "FREAD past the end: refused"); reset_xs;
        run(8'd22, 24'h000000, 24'd1025, 8'd0);
        check(s_err == 8'd6, "FREAD of more than 1 KiB: refused"); reset_xs;
        run(8'd22, 24'h000000, 24'd63, 8'd0);
        check(s_err == 8'd6, "FREAD of an odd count: refused"); reset_xs;

        // a tagged FREAD: the bytes from word 1, the tag {seq, 00} once done
        run(8'd22, 24'h2F0000, 24'd1022, 8'd1);
        check(s_err == 0 && wtag_on && wtag == {seq, 8'h00}, "tagged FREAD: the tag is the op's seq");
        win_read(1024);
        nbad = 0; for (i = 0; i < 1022; i = i + 1) if (wbuf[2 + i] !== flash[24'h2F0000 + i]) nbad = nbad + 1;
        check(nbad == 0, "tagged FREAD: the bytes from word 1");
        run(8'd22, 24'h2F0000, 24'd1024, 8'd1);
        check(s_err == 8'd6, "tagged FREAD of 1024: refused (1022 at most)"); reset_xs;
        run(8'd22, 24'h2F0000, 24'd64, 8'd0);
        check(s_err == 0 && !wtag_on, "untagged FREAD: no tag");
        // FCOPY: boot flash -> PSRAM (FCDST sets where)
        run(8'd24, 24'h500000, 24'd0, 8'd0);
        check(s_err == 0, "FCDST 0x500000");
        run(8'd23, 24'h2F0000, 24'd16384, 8'd0);
        check(s_err == 0 && s_crc == flash_crc(24'h2F0000, 16384), "FCOPY 16 KiB: no error, the crc of the flash bytes");
        nbad = 0;
        for (i = 0; i < 8192; i = i + 1) if (psram[(24'h500000 >> 1) + i] !== {flash[24'h2F0000 + 2*i + 1], flash[24'h2F0000 + 2*i]}) nbad = nbad + 1;
        check(nbad == 0 && n_cw == 8192, "FCOPY: every word in PSRAM, low byte first");
        run(8'd23, 24'h2F4000, 24'd2, 8'd0);
        check(s_err == 0 && psram[(24'h504000 >> 1)] == {flash[24'h2F4001], flash[24'h2F4000]}, "FCOPY again goes on after the last (FCDST moves on)");
        run(8'd24, 24'h020000, 24'd0, 8'd0);
        check(s_err == 8'd6, "FCDST into the frame buffers: refused"); reset_xs;
        run(8'd24, 24'h7FF000, 24'd0, 8'd0);
        run(8'd23, 24'h2F0000, 24'd8192, 8'd0);
        check(s_err == 8'd6, "FCOPY past the PSRAM's end: refused"); reset_xs;
        run(8'd24, 24'h500000, 24'd0, 8'd0);
        run(8'd23, 24'h2F0000, 24'd3, 8'd0);
        check(s_err == 8'd6, "FCOPY of an odd count: refused"); reset_xs;
        // the 18.31 guards (recovery 0x000800-0x05BFFF)
        for (i = 24'h050000; i < 24'h05C000; i = i + 1) flash[i] = 8'h5A;
        run(8'd17, 24'h05B000, 24'd0, 8'h20);
        check(s_err == 8'd6 && flash[24'h05B000] == 8'h5A, "FERASE 4 KiB below 0x05C000 (the recovery design): refused"); reset_xs;
        run(8'd17, 24'h05C000, 24'd0, 8'hD8);
        check(s_err == 8'd6 && flash[24'h050000] == 8'h5A, "FERASE 64 KiB at 0x05C000 (its block starts at 0x050000): refused"); reset_xs;
        run(8'd17, 24'h05F000, 24'd0, 8'hD8);
        check(s_err == 8'd6, "FERASE 64 KiB anywhere in the block 0x050000: refused"); reset_xs;
        run(8'd17, 24'h05C000, 24'd0, 8'h20);
        check(s_err == 0 && flash[24'h05BFFF] == 8'h5A, "FERASE 4 KiB at 0x05C000 (the game area): runs, the recovery design intact");
        run(8'd17, 24'h060000, 24'd0, 8'hD8);
        check(s_err == 0, "FERASE 64 KiB at 0x060000: runs");
        run(8'd20, 24'h0007C0, 24'd64, 8'd0);
        check(s_err == 8'd6, "FHDR at 0x7C0 (the recovery header): refused"); reset_xs;
        run(8'd20, 24'h000790, 24'd64, 8'd0);
        check(s_err == 8'd6, "FHDR reaching into 0x7C0: refused"); reset_xs;
        run(8'd21, 24'h000000, 24'd0, 8'h5A);
        check(s_err == 8'd6, "FHDRERASE: refused (18.31)"); reset_xs;
        run(8'd18, 24'h05BF00, 24'd256, 8'd0);
        check(s_err == 8'd6, "FPROG below 0x05C000: refused"); reset_xs;
        $display("tb_cart_xs_flash: %0d ok, %0d failed (%0d erases, %0d bytes programmed)", ok, bad, n_erase, n_prog);
        $finish;
    end
endmodule
