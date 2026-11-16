// tb_cart_xs.v (lane cartspeed1): the express cart stream end to end.
//   ESP32 model --QSPI bit level--> QSPI_Slave (real) --> cart_xs ring
//                                   mm_burst_write (real, open FIFO): must not touch PSRAM for ring writes
//   ESP32 model --UART packets (CRC-8, byte-timed)--> cart_xs sniffer (address 0x12)
//   cart_xs status channel <-- a model of system monitor channel 12 (bytepos / taken)
//   cart_xs --> cart_engine (real) express port --> the picky SST39VF1682 + MBC5 cart (tb_cart_xs_cart.vh)
// Run: iverilog -g2012 -o tb_cart_xs tb_cart_xs.v ../src/rtl/BSP/cart_xs.v ../src/rtl/BSP/cart_engine.v \
//        ../src/rtl/BSP/net_window_dpram.v ../src/rtl/BSP/qspi_slave.v ../src/rtl/BSP/mm_burst_write.v \
//        ../src/open/fifo1k_open.v && vvp tb_cart_xs
`timescale 1ns/1ps
module tb_cart_xs;
    reg clk = 0;  always #29.8 clk = ~clk;                    // hclk 16.78 MHz
    reg gclk = 0; always @(posedge clk) gclk <= ~gclk;         // gclk = hclk / 2 (same PLL)
    reg xclk = 0; always #6.67 xclk = ~xclk;                   // xclk ~75 MHz
    reg reset = 1, allow = 0, g_reset = 1;
    integer UART_GAP = 80, TX_GAP = 8;                                     // gclk per UART byte (real 115200: ~730)

    // ---------------------------------------------------------------- QSPI
    reg QCLK = 0, QCS = 1, Q0 = 0, Q1 = 0, Q2 = 0, Q3 = 0;
    wire qmi, qdv, qar, qbv; wire [15:0] qd; wire [31:0] qa; wire [7:0] qb; wire [9:0] ql; wire qc;
    QSPI_Slave u_qs (.QSPI_CLK(QCLK), .QSPI_CS(QCS), .QSPI_MOSI(Q0), .QSPI_MISO(Q1), .QSPI_WP(Q2), .QSPI_HD(Q3),
        .qMenuInit(qmi), .qDataValid(qdv), .qData(qd), .qAddress(qa), .qLength(ql), .qCommand(qc),
        .qAddrReadyO(qar), .qByteValid(qbv), .qByte(qb));
    wire xreq; wire [15:0] xdout; wire [22:0] xaddr;
    mm_burst_write u_mbw (.QSPI_CLK(QCLK), .QSPI_CS(QCS), .qAddress(qa), .qDataValid(qdv), .qData(qd),
        .xClk(xclk), .xRdEn(1'b0), .xRamReady(1'b1), .xMcuReqWrite(xreq), .xDout(xdout), .xAddress(xaddr));
    integer n_psram_req = 0; always @(posedge xclk) if (xreq) n_psram_req = n_psram_req + 1;

    // ---------------------------------------------------------------- DUT
    reg  [7:0] urx = 8'd0; reg urx_val = 1'b0;
    wire g_tx_valid; wire [7:0] g_tx_len, g_tx_byte; reg [7:0] g_tx_bytepos = 8'd0; reg g_tx_taken = 1'b0;
    wire xs_session, xs_hold, xs_bus_req, xs_bus_wr, xs_bus_done, xs_tcfg;
    wire [15:0] xs_bus_a; wire [7:0] xs_bus_d, xs_bus_q;
    wire [4:0] ts, tp, th, ta, tg;
    wire eng_busy;
    cart_xs #(.PROG_TMO(1680), .ERASE_TMO(40000), .STARVE_TMO(400000), .STARVE_FLAG(100000)) dut (
        .qspi_clk_n(~QCLK), .q_addr_ready(qar), .q_address(qa), .q_byte_valid(qbv), .q_byte(qb),
        .gclk(gclk), .g_reset(g_reset), .g_uart_rx_data(urx), .g_uart_rx_val(urx_val),
        .g_tx_valid(g_tx_valid), .g_tx_len(g_tx_len), .g_tx_bytepos(g_tx_bytepos), .g_tx_byte(g_tx_byte), .g_tx_taken(g_tx_taken),
        .hclk(clk), .h_reset(reset), .h_allow(allow), .h_eng_busy(eng_busy & ~xs_hold),
        .x_session(xs_session), .x_hold(xs_hold), .x_bus_req(xs_bus_req), .x_bus_wr(xs_bus_wr), .x_bus_a(xs_bus_a),
        .x_bus_d(xs_bus_d), .x_bus_done(xs_bus_done), .x_bus_q(xs_bus_q),
        .x_tcfg(xs_tcfg), .x_t_setup(ts), .x_t_pulse(tp), .x_t_hold(th), .x_t_access(ta), .x_t_gap(tg));

    wire  [7:0] DIN;
    wire sel; wire [7:0] edo;
    wire active, e_clk, e_cs, e_rd, e_wr, e_dir_e; wire [15:0] e_a; wire [7:0] e_d_out; wire [15:0] ncyc;
    cart_engine #(.PROG_TMO(1680), .ERASE_TMO(40000)) eng (
        .clk(clk), .reset(reset), .allow(allow),
        .cpu_addr(16'hC000), .cpu_do(8'h00), .cpu_rd_n(1'b1), .cpu_wr_stb(1'b0), .sel(sel), .do_(edo),
        .active(active), .e_a(e_a), .e_clk(e_clk), .e_cs(e_cs), .e_rd(e_rd), .e_wr(e_wr), .e_d_out(e_d_out),
        .e_dir_e(e_dir_e), .cart_d_in(DIN),
        .x_session(xs_session), .x_hold(xs_hold), .x_bus_req(xs_bus_req), .x_bus_wr(xs_bus_wr), .x_bus_a(xs_bus_a),
        .x_bus_d(xs_bus_d), .x_bus_done(xs_bus_done), .x_bus_q(xs_bus_q),
        .x_tcfg(xs_tcfg), .x_t_setup(ts), .x_t_pulse(tp), .x_t_hold(th), .x_t_access(ta), .x_t_gap(tg),
        .busy(eng_busy), .n_cycles(ncyc));

    // the mux in top.v (the core bus keeps toggling; it must never reach the cart in a session)
    reg [15:0] core_a = 16'h1234; reg core_rd = 1'b0, core_wr = 1'b1, core_clk = 1'b0;
    always @(posedge clk) begin core_a <= core_a + 16'd7; core_rd <= ~core_rd; core_clk <= ~core_clk; end
    wire [15:0] A   = active ? e_a : core_a;
    wire        CLK = active ? e_clk : core_clk;
    wire        RD  = active ? e_rd : core_rd;
    wire        WR  = active ? e_wr : core_wr;
    wire        CS  = active ? e_cs : 1'b1;
    wire        DRV = active ? ~e_dir_e : 1'b0;
    wire  [7:0] DOUT = e_d_out;
`include "tb_cart_xs_cart.vh"

    // ---------------------------------------------------------------- checks
    integer ok = 0, bad = 0;
    task check(input c, input [8*96-1:0] what);
        begin if (c) ok = ok + 1; else begin bad = bad + 1; $display("FAIL %0s", what); end end
    endtask

    // ---------------------------------------------------------------- CRCs
    function [31:0] crc32_upd(input [31:0] c, input [7:0] d);
        integer k; reg [31:0] x;
        begin x = c ^ {24'd0, d}; for (k = 0; k < 8; k = k + 1) x = x[0] ? ((x >> 1) ^ 32'hEDB88320) : (x >> 1); crc32_upd = x; end
    endfunction
    function [7:0] crc8(input [7:0] c, input [7:0] d);
        integer k; reg [7:0] x;
        begin x = c ^ d; for (k = 0; k < 8; k = k + 1) x = x[7] ? ({x[6:0], 1'b0} ^ 8'h1D) : {x[6:0], 1'b0}; crc8 = x; end
    endfunction

    // ---------------------------------------------------------------- UART (ESP32 -> FPGA)
    reg [7:0] pk [0:15];
    task uart_byte(input [7:0] b);
        begin @(posedge gclk); urx <= b; urx_val <= 1'b1; @(posedge gclk); urx_val <= 1'b0; repeat (UART_GAP - 1) @(posedge gclk); end
    endtask
    // every packet goes through one queue and one sender (no two processes on the line at once)
    reg [7:0] pkq [0:255][0:15]; integer pkql [0:255]; integer pkq_h = 0, pkq_t = 0;
    task uart_pkt(input integer len, input integer corrupt);
        integer i;
        begin
            for (i = 0; i < 16; i = i + 1) pkq[pkq_t % 256][i] = pk[i];
            pkql[pkq_t % 256] = corrupt ? -len : len;
            pkq_t = pkq_t + 1;
        end
    endtask
    always begin : sender
        integer i, len, cor; reg [7:0] c, a;
        if (pkq_h == pkq_t) @(posedge gclk);
        else begin
            len = pkql[pkq_h % 256]; cor = len < 0; if (cor) len = -len;
            a = (len == 99) ? 8'h11 : 8'h12;
            if (len == 99) len = 2;
            c = crc8(8'hFF, 8'h8F); uart_byte(8'h8F);
            c = crc8(c, a); uart_byte(a);
            c = crc8(c, len[7:0]); uart_byte(len[7:0]);
            for (i = 0; i < len; i = i + 1) begin c = crc8(c, pkq[pkq_h % 256][i]); uart_byte(pkq[pkq_h % 256][i]); end
            uart_byte(cor ? ~c : c);
            pkq_h = pkq_h + 1;
        end
    end
    // other traffic on the line (another address) must be ignored
    task uart_other; begin pk[0] = 8'd1; pk[1] = 8'd9; uart_pkt(99, 0); end endtask
    reg [7:0] seq = 8'd0;
    task op(input [7:0] o, input [23:0] f, input [23:0] n, input [7:0] arg);
        begin
            seq = seq + 8'd1;
            pk[0] = 8'd1; pk[1] = seq; pk[2] = o; pk[3] = f[7:0]; pk[4] = f[15:8]; pk[5] = f[23:16];
            pk[6] = n[7:0]; pk[7] = n[15:8]; pk[8] = n[23:16]; pk[9] = arg;
            uart_pkt(10, 0);
        end
    endtask
    task cfg(input [4:0] s_, input [4:0] p_, input [4:0] h_, input [4:0] a_, input [4:0] g_, input sess);
        begin op(8'd4, {3'd0, h_, 3'd0, p_, 3'd0, s_}, {7'd0, sess, 3'd0, g_, 3'd0, a_}, 8'd0); end
    endtask
    task simple(input [7:0] t); begin pk[0] = t; uart_pkt(1, 0); end endtask

    // ---------------------------------------------------------------- QSPI master (ESP32 SPI host, mode 0)
    // 11 command bits, 32 address bits, 3 dummy (one line), then data 4 bits per clock, high nibble first
    reg qbusy = 0;
    task qhalf; #12.5; endtask
    task qbit(input b); begin Q0 = b; qhalf; QCLK = 1; qhalf; QCLK = 0; end endtask
    reg [7:0] qbuf [0:4095];
    task qspi_write(input [31:0] addr, input integer len);
        integer i, k;
        begin
            wait (!qbusy); qbusy = 1;
            QCS = 0; #75;
            for (k = 10; k >= 0; k = k - 1) qbit(k == 10 ? 1'b1 : 1'b1);
            for (k = 31; k >= 0; k = k - 1) qbit(addr[k]);
            for (k = 0; k < 3; k = k - 0) begin qbit(1'b0); k = k + 1; end
            for (i = 0; i < len; i = i + 1) begin
                {Q3, Q2, Q1, Q0} = qbuf[i][7:4]; qhalf; QCLK = 1; qhalf; QCLK = 0;
                {Q3, Q2, Q1, Q0} = qbuf[i][3:0]; qhalf; QCLK = 1; qhalf; QCLK = 0;
            end
            #75; QCS = 1; {Q3, Q2, Q1, Q0} = 4'h0; #200;
            qbusy = 0;
        end
    endtask

    // ---------------------------------------------------------------- status channel (system monitor channel 12)
    reg [7:0] stb [0:31];
    reg [7:0] s_flags, s_done, s_err, s_q, s_run; reg [31:0] s_cons, s_valid, s_crc, s_pbusy, s_ebusy; reg [23:0] s_f, s_nprog;
    integer n_status = 0;
    always @(posedge gclk) begin : txmodel
        integer i;
        if (g_tx_valid && !g_tx_taken) begin
            check(g_tx_len == 8'd32, "status length 32");
            for (i = 0; i < 32; i = i + 1) begin g_tx_bytepos <= i; @(posedge gclk); @(posedge gclk); stb[i] = g_tx_byte; repeat (TX_GAP) @(posedge gclk); end
            g_tx_taken <= 1'b1; @(posedge gclk); g_tx_taken <= 1'b0;
            s_flags = stb[1]; s_done = stb[2]; s_err = stb[3];
            s_cons = {stb[7], stb[6], stb[5], stb[4]}; s_valid = {stb[11], stb[10], stb[9], stb[8]};
            s_crc = {stb[15], stb[14], stb[13], stb[12]}; s_f = {stb[18], stb[17], stb[16]}; s_q = stb[19]; s_run = stb[20];
            s_pbusy = {stb[24], stb[23], stb[22], stb[21]}; s_ebusy = {stb[28], stb[27], stb[26], stb[25]}; s_nprog = {stb[31], stb[30], stb[29]};
            n_status = n_status + 1;
        end
    end

    // ---------------------------------------------------------------- the ESP32 feeder: stream data[0..total) into the
    // ring as the status says it is consumed, 1 KiB per QSPI write + POST, window = ring size
    reg [7:0] img [0:2097151];          // what the ESP32 has staged (the stream, in PROG order)
    integer feed_total = 0, feed_pos = 0, feed_bad_once = -1, n_posts = 0, n_naks = 0;
    reg feeding = 0;
    integer RING = 4096, CH = 1024;
    always begin : feeder
        integer len, i; reg [31:0] c; reg [31:0] endp; reg [2:0] last_naks;
        wait (feeding);
        last_naks = s_q[6:4];
        while (feeding) begin
            if (s_q[6:4] != last_naks) begin                             // refused: from `valid` again (once per refusal)
                last_naks = s_q[6:4]; n_naks = n_naks + 1; feed_pos = s_valid;
            end
            if (feed_pos < feed_total && feed_pos + 1 <= s_cons + RING && pkq_t - pkq_h < 3) begin
                len = CH - (feed_pos % CH);
                if (feed_pos + len > feed_total) len = feed_total - feed_pos;
                if (feed_pos + len > s_cons + RING) len = s_cons + RING - feed_pos;
                c = 32'hFFFFFFFF;
                for (i = 0; i < len; i = i + 1) begin qbuf[i] = img[feed_pos + i]; c = crc32_upd(c, img[feed_pos + i]); end
                if (feed_bad_once >= feed_pos && feed_bad_once < feed_pos + len) begin
                    qbuf[feed_bad_once - feed_pos] = ~qbuf[feed_bad_once - feed_pos]; feed_bad_once = -1;   // a QSPI bit error
                end
                qspi_write(32'h80000000 | (feed_pos % RING), len);
                endp = feed_pos + len;
                pk[0] = 8'd2; pk[1] = endp[7:0]; pk[2] = endp[15:8]; pk[3] = endp[23:16]; pk[4] = endp[31:24];
                pk[5] = len[7:0]; pk[6] = len[15:8]; c = ~c;
                pk[7] = c[7:0]; pk[8] = c[15:8]; pk[9] = c[23:16]; pk[10] = c[31:24];
                uart_pkt(11, 0); n_posts = n_posts + 1;
                feed_pos = endp;                                         // pipelined: no wait for the answer
            end else @(posedge gclk);
        end
    end

    // ---------------------------------------------------------------- helpers
    task wait_done(input [7:0] sq, input integer max_us);
        real t0;
        begin
            t0 = $realtime;
            while (!(s_done == sq && !s_flags[1]) && !s_flags[2] && $realtime - t0 < max_us * 1000.0) @(posedge gclk);
        end
    endtask
    // a status that surely comes after everything sent so far took effect
    task status_now; integer n0;
        begin
            while (pkq_h != pkq_t) @(posedge gclk);
            repeat (3000 + 25 * TX_GAP) @(posedge gclk);
            n0 = n_status; simple(8'd4);
            while (pkq_h != pkq_t) @(posedge gclk);
            while (n_status == n0) @(posedge gclk);
            repeat (3000 + 25 * TX_GAP) @(posedge gclk);
        end
    endtask
    function [31:0] flash_crc(input integer f, input integer n);
        integer i; reg [31:0] c;
        begin c = 32'hFFFFFFFF; for (i = 0; i < n; i = i + 1) c = crc32_upd(c, flash[f + i]); flash_crc = ~c; end
    endfunction
    task stream_prog(input integer f, input integer n, input integer base);   // PROG op + feed img[base..base+n)
        begin
            feed_total = base + n; feeding = 1;
            op(8'd2, f, n, 8'd0);
        end
    endtask
    integer k, nbad, t_us, p0;
    real t_start, t_end;
    reg [7:0] sq_e, sq_p, sq_c;
    reg [31:0] want;

    initial begin
        if ($test$plusargs("real")) begin UART_GAP = 730; TX_GAP = 728; $display("  UART at 115200 both ways"); end
        for (i = 0; i < SIZE; i = i + 1) flash[i] = (i < 262144) ? (i * 13 + 7) & 8'hFF : 8'hFF;
        for (i = 0; i < 32768; i = i + 1) fram[i] = 8'hFF;
        for (i = 0; i < 2097152; i = i + 1) img[i] = ((i * 7 + (i >> 8) * 3) & 8'hFF);
        for (i = 4096; i < 4096 + 1500; i = i + 1) img[i] = 8'hFF;              // an FF run (erased, skipped)
        img[100] = 8'hFF;
        s_flags = 0; s_valid = 0; s_cons = 0; s_done = 0;
        repeat (20) @(posedge clk); reset = 0; g_reset = 0; repeat (20) @(posedge clk);

        // 1. ops without allow / session are refused; other UART traffic ignored
        uart_other;
        op(8'd3, 24'h010000, 24'd16, 8'd0); status_now;
        check(s_err == 8'd7, "op without a session: error NOSESS");
        check(!active, "no session: the core owns the bus");
        simple(8'd5); status_now;
        check(s_err == 8'd0 && s_cons == 0 && s_valid == 0, "RESET clears the error and the positions");

        // 2. session (default timing), erase block 1, blank check
        allow = 1; repeat (10) @(posedge clk);
        cfg(5'd4, 5'd7, 5'd4, 5'd7, 5'd4, 1'b1); status_now;
        check(xs_session && active, "CFG session on: the engine has the bus");
        op(8'd1, 24'h010000, 24'd0, 8'h30); sq_e = seq;
        op(8'd3, 24'h010000, 24'h010000, 8'd0); sq_c = seq;                   // queued behind the erase
        wait_done(sq_c, 400000);
        check(s_err == 0 && s_done == sq_c, "erase + CRC queued back to back");
        check(s_q[7] == 1'b1, "the erased block reads all FF");
        check(s_crc == flash_crc(24'h010000, 65536), "CRC op = crc32 of the block");
        check(n_erase == 1 && n_suspend == 0 && n_busy_writes == 0, "one erase, nothing written while busy");

        // 3. program 8 KiB across a bank boundary (0x013000 .. 0x015000: banks 4 -> 5) from the ring
        p0 = n_prog;
        t_start = $realtime;
        stream_prog(24'h013000, 8192, 0); sq_p = seq;
        op(8'd3, 24'h013000, 24'd8192, 8'd0); sq_c = seq;
        wait_done(sq_c, 900000);
        t_end = $realtime;
        feeding = 0;
        check(s_err == 0 && s_done == sq_c, "PROG 8 KiB + CRC done, no error");
        nbad = 0; for (i = 0; i < 8192; i = i + 1) if (flash[24'h013000 + i] !== img[i]) nbad = nbad + 1;
        check(nbad == 0, "the cart holds the 8 KiB");
        want = 32'hFFFFFFFF; for (i = 0; i < 8192; i = i + 1) want = crc32_upd(want, img[i]); want = ~want;
        check(s_crc == want, "CRC op after PROG = crc32 of the data");
        check(s_cons == 8192 && s_valid == 8192, "consumed = valid = 8192");
        k = 0; for (i = 0; i < 8192; i = i + 1) if (img[i] != 8'hFF) k = k + 1;
        check(n_prog - p0 == k, "one program per non-FF byte, FF skipped");
        check(n_suspend == 0 && n_busy_writes == 0 && n_rejected == 0, "default timing: every pulse accepted, none while busy");
        check(n_psram_req == 0, "ring writes never request a PSRAM burst");
        check(s_nprog == k, "measured: bytes programmed = the non-FF bytes");
        $display("  measured by the FPGA: %0d programs, program wait %0.2f us per byte (model tBP %0.1f us), last erase busy %0.1f us",
                 s_nprog, s_pbusy * 0.0596 / s_nprog, T_BP / 1000.0, s_ebusy * 0.0596);
        check(s_pbusy * 0.0596 / s_nprog > T_BP / 1000.0 && s_pbusy * 0.0596 / s_nprog < T_BP / 1000.0 + 1.6, "measured program wait = tBP + under two reads");
        $display("  default timing: %0d bytes (%0d programmed) in %0.1f us incl CRC = %0.2f us per programmed byte",
                 8192, k, (t_end - t_start) / 1000.0, (t_end - t_start) / 1000.0 / k);

        // 4. a QSPI bit error: the post is refused, the range written again
        n_naks = 0;
        op(8'd1, 24'h020000, 24'd0, 8'h30);
        feed_bad_once = 8192 + 300; feed_pos = 8192;
        stream_prog(24'h020000, 2048, 8192); sq_p = seq;
        wait_done(sq_p, 400000); feeding = 0;
        check(s_err == 0 && s_done == sq_p, "PROG after a refused post: done");
        check(n_naks >= 1, "the bad range was refused (crc32 per post)");
        nbad = 0; for (i = 0; i < 2048; i = i + 1) if (flash[24'h020000 + i] !== img[8192 + i]) nbad = nbad + 1;
        check(nbad == 0, "the cart holds the good bytes, never the bad one");

        // 5. a cell that will not program: VERIFY error at its address, queue cleared, sticky until RESET
        op(8'd1, 24'h030000, 24'd0, 8'h30);
        stuck_addr = 21'h030010; stuck_mask = 8'hFE;
        for (i = 0; i < 64; i = i + 1) img[10240 + i] = 8'h22;               // bit 0 must be 0 anyway... make it need a 1
        img[10240 + 16] = 8'h21;
        feed_pos = 10240;
        stream_prog(24'h030000, 64, 10240); sq_p = seq;
        op(8'd3, 24'h030000, 24'd64, 8'd0);
        wait_done(sq_p, 200000); feeding = 0;
        status_now;
        check(s_err == 8'd2, "VERIFY error");
        check(s_f == 24'h030010, "error address = the bad cell");
        check(s_done != sq_p && (s_q & 8'h0F) == 0, "the failed op not done, the queue cleared");
        op(8'd3, 24'h030000, 24'd16, 8'd0); status_now;
        check(s_err == 8'd2 && (s_q & 8'h0F) == 0, "ops refused while the error stands");
        stuck_addr = 21'h1FFFFF;
        simple(8'd5); status_now;
        check(s_err == 0 && s_cons == 0 && s_valid == 0, "RESET after an error");

        // 6. ABORT while a PROG waits for data (starved), then the starve flag / timeout
        op(8'd1, 24'h040000, 24'd0, 8'h30);
        op(8'd2, 24'h040000, 24'd100, 8'd0); sq_p = seq;
        repeat (200000) @(posedge clk);
        status_now;
        check(s_flags[1] && s_flags[4], "PROG without data: running, starved flag");
        simple(8'd3); status_now;
        check(s_err == 8'd8 && !s_flags[1], "ABORT stops it (error ABORTED)");
        simple(8'd5); status_now;
        op(8'd2, 24'h040000, 24'd10, 8'd0); sq_p = seq;
        wait_done(sq_p, 100000); status_now;
        check(s_err == 8'd4, "no data for the starve timeout: error STARVE");
        simple(8'd5); status_now;

        // 6b. the ESP32's restart while an op runs and more wait: ABORT, RESET, CFG sent back to back;
        //     the queue is emptied at ABORT / RESET, the CFG (sent after them) runs, positions are 0
        op(8'd1, 24'h060000, 24'd0, 8'h30);
        op(8'd3, 24'h060000, 24'h010000, 8'd0);
        op(8'd3, 24'h070000, 24'h010000, 8'd0); sq_c = seq;
        repeat (3000) @(posedge gclk);
        simple(8'd3); simple(8'd5);
        cfg(5'd4, 5'd7, 5'd4, 5'd7, 5'd4, 1'b1); sq_e = seq;
        wait_done(sq_e, 400000); status_now;
        check(s_err == 0 && s_done == sq_e && s_cons == 0 && s_valid == 0 && (s_q & 8'h0F) == 0,
              "ABORT + RESET + CFG back to back: the CFG ran, nothing else, positions 0");
        op(8'd3, 24'h060000, 24'd16, 8'd0); sq_c = seq; wait_done(sq_c, 100000);
        check(s_err == 0 && s_done == sq_c, "ops run again after the restart");

        // 7. allow falls mid-op: the session ends, error NOSESS, the core owns the bus
        cfg(5'd4, 5'd7, 5'd4, 5'd7, 5'd4, 1'b1);
        op(8'd3, 24'h000000, 24'h010000, 8'd0); sq_c = seq;
        repeat (20000) @(posedge clk);
        allow = 0; repeat (100) @(posedge clk); status_now;
        check(s_err == 8'd7 && !xs_session && !active, "allow off mid-op: NOSESS, session over");
        allow = 1; simple(8'd5); status_now;

        // 8. fast timing (a cart that takes short cycles): setup 1 pulse 2 hold 1 access 3 gap 1
        T_DSET = 40.0; T_DHOLD = 20.0;
        cfg(5'd1, 5'd2, 5'd1, 5'd3, 5'd1, 1'b1);
        op(8'd1, 24'h050000, 24'd0, 8'h30);
        p0 = n_prog; feed_pos = 0; for (i = 0; i < 4096; i = i + 1) img[i] = (i * 29 + 5) & 8'hFF;
        t_start = $realtime;
        stream_prog(24'h050000, 4096, 0); sq_p = seq;
        op(8'd3, 24'h050000, 24'd4096, 8'd0); sq_c = seq;
        wait_done(sq_c, 900000); t_end = $realtime; feeding = 0;
        check(s_err == 0 && s_done == sq_c, "fast timing: PROG + CRC done");
        nbad = 0; for (i = 0; i < 4096; i = i + 1) if (flash[24'h050000 + i] !== img[i]) nbad = nbad + 1;
        check(nbad == 0, "fast timing: the cart holds the data");
        k = 0; for (i = 0; i < 4096; i = i + 1) if (img[i] != 8'hFF) k = k + 1;
        $display("  fast timing: %0d programmed bytes in %0.1f us incl erase wait + CRC = %0.2f us per byte", k, (t_end - t_start) / 1000.0, (t_end - t_start) / 1000.0 / k);

        // 8b. cart RAM: RAMW 2 KiB across a RAM bank (0x1C00..0x2400) from the ring, RAMCRC, safe timing
        T_DSET = 200.0; T_DHOLD = 100.0;
        cfg(5'd4, 5'd7, 5'd4, 5'd7, 5'd4, 1'b1);
        simple(8'd5); status_now;
        for (i = 0; i < 2048; i = i + 1) img[i] = (i * 3 + 1) & 8'hFF;
        feed_pos = 0; feed_total = 2048; feeding = 1;
        op(8'd5, 24'h001C00, 24'd2048, 8'd0); sq_p = seq;
        op(8'd6, 24'h001C00, 24'd2048, 8'd0); sq_c = seq;
        wait_done(sq_c, 200000); feeding = 0;
        check(s_err == 0 && s_done == sq_c, "RAMW + RAMCRC done");
        nbad = 0; for (i = 0; i < 2048; i = i + 1) if (fram[16'h1C00 + i] !== img[i]) nbad = nbad + 1;
        check(nbad == 0 && ram_bank == 4'd1, "the cart RAM holds the 2 KiB across banks 0 -> 1");
        want = 32'hFFFFFFFF; for (i = 0; i < 2048; i = i + 1) want = crc32_upd(want, img[i]); want = ~want;
        check(s_crc == want, "RAMCRC = crc32 of what was written");
        check(n_fram_bad == 0 && n_fram_torn == 0, "FRAM timing rules kept (address before /CS, precharge)");

        // 9. the OSD's own QSPI writes still go to PSRAM
        for (i = 0; i < 64; i = i + 1) qbuf[i] = i;
        qspi_write(32'h00001000, 64);
        repeat (20) @(posedge xclk);
        check(n_psram_req == 1, "an OSD write still requests its PSRAM burst");

        $display("  %0d status packets, %0d posts, %0d engine bus cycles, %0d programs, %0d erases", n_status, n_posts, ncyc, n_prog, n_erase);
        $display("tb_cart_xs: %0d ok, %0d failed", ok, bad);
        if (bad == 0) $display("ALL PASSED");
        $finish;
    end
endmodule
