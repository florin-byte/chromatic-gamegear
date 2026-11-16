// tb_net_window.v: network window v1 (BSP/net_window.v) end to end.
//
//   GB CPU model (hclk, T80-like M-cycles, normal and CGB double speed, the
//   gb.v write strobe) <-> net_window <-> UART byte level both ways:
//     FPGA -> ESP32: the real uart_packet_wrapper_tx + the UART2 BUSY model of
//                    tb_sysmon_link (115200 at gClk), parsed and CRC-checked
//                    by a behavioural ESP32.
//     ESP32 -> FPGA: behavioural ESP32 bytes at UART pace; the stock
//                    uart_packet_wrapper_rx runs in parallel on the same bytes
//                    to show it only ever reports address 0x11 for them.
//   gclk is asynchronous to hclk (default 8.37 MHz vs 16.78 MHz, drifting phase).
//
// Run (from this directory):
//   iverilog -g2012 -o tb_net_window tb_net_window.v ../src/rtl/BSP/net_window.v \
//       ../src/rtl/BSP/net_window_dpram.v ../src/rtl/BSP/uart_packet_wrapper_tx.sv \
//       ../src/rtl/BSP/uart_packet_wrapper_rx.sv && vvp tb_net_window
//   plusargs: +gper=<gclk half period ns, default 59.71>  +fast (ESP bytes 40 gclk apart)
`timescale 1ns/1ps

module tb_net_window;
    // ------------------------------------------------------------ clocks
    reg hclk = 0, gclk = 0;
    real gper = 59.71;
    always #29.8 hclk = ~hclk;                 // ~16.78 MHz
    initial begin
        if ($value$plusargs("gper=%f", gper)) ;
        forever #(gper) gclk = ~gclk;
    end

    reg h_reset = 1, g_reset = 1;

    // ------------------------------------------------------------ CPU bus (hclk)
    reg  [1:0] ce_div = 0;
    reg        double_speed = 0;
    always @(posedge hclk) ce_div <= ce_div + 1;
    wire ce     = (ce_div == 2'd3);
    wire ce_2x  = ce_div[0];
    wire ce_cpu = double_speed ? ce_2x : ce;

    reg [15:0] cpu_addr = 16'h0100;
    reg  [7:0] cpu_do = 0;
    reg        cpu_rd_n = 1, cpu_wr_n = 1;
    reg        old_cpu_wr_n = 1;                   // as gb.v
    always @(posedge hclk) if (ce_cpu) old_cpu_wr_n <= cpu_wr_n;
    wire cpu_wr_n_edge = ~(old_cpu_wr_n & ~cpu_wr_n);
    wire cpu_wr_stb = ce_cpu & ~cpu_wr_n_edge;     // the gb.v write strobe

    wire       nw_sel;
    wire [7:0] nw_do;
    wire [7:0] cpu_di = nw_sel ? nw_do : (cpu_addr[15:8] == 8'hFF ? 8'hFF : 8'h00);

    // ------------------------------------------------------------ UART FPGA -> ESP
    localparam BIT = 72;                           // 115200 at gClk
    wire [7:0] u_tx_data; wire u_tx_val;
    reg  u_busy_r = 0; reg [7:0] u_data = 0; integer u_cnt = 0;
    wire u_tx_busy = u_busy_r | u_tx_val;
    reg  u_tx_val_q = 0;
    always @(posedge gclk) u_tx_val_q <= u_tx_val;
    wire u_tx_busy_safe = u_tx_busy | u_tx_val_q;  // system_monitor's busy fix
    reg  [7:0] wire_byte; reg wire_stb = 0;
    integer dropped = 0;
    always @(posedge gclk) begin
        wire_stb <= 0;
        if (u_tx_val && !u_busy_r) u_data <= u_tx_data;
        if (u_tx_val && u_busy_r) dropped = dropped + 1;
        if (u_cnt == 0) begin
            u_busy_r <= 0;
            if (u_tx_val) u_cnt <= 10 * BIT;
        end else if (u_cnt > BIT) begin
            u_busy_r <= 1;
            u_cnt <= u_cnt - 1;
            if (u_cnt == BIT + 1) begin wire_byte <= u_data; wire_stb <= 1; end
        end else begin
            u_busy_r <= 0;
            if (u_tx_val) u_cnt <= 10 * BIT;
            else u_cnt <= u_cnt - 1;
        end
    end

    // single-channel stand-in for system_monitor_arbiter (channel 11 -> address 0x0B)
    wire       g_tx_valid; wire [7:0] g_tx_len, g_tx_byte, tx_bytepos;
    wire       write_done;
    reg        tx_write = 0, tx_req_sent = 0;
    wire       g_tx_taken = write_done & tx_req_sent;
    reg        link_busy = 0;
    always @(posedge gclk) begin
        tx_write <= 0;
        if (g_tx_taken) tx_req_sent <= 0;
        else if (g_tx_valid && !tx_req_sent && !u_tx_busy_safe) begin tx_write <= 1; tx_req_sent <= 1; end
    end

    uart_packet_wrapper_tx u_wtx (
        .clk(gclk), .reset(g_reset), .uart_tx_busy(u_tx_busy_safe),
        .uart_tx_data(u_tx_data), .uart_tx_val(u_tx_val), .write_done(write_done),
        .write(tx_write), .menuDisabled(1'b0), .stayAwake(1'b1), .uartDisabled(1'b0),
        .tx_address(7'h0B), .tx_byteCount(g_tx_len), .tx_bytepos(tx_bytepos), .tx_senddata(g_tx_byte));

    // ------------------------------------------------------------ UART ESP -> FPGA
    reg  [7:0] e_rx_data = 0; reg e_rx_val = 0;
    wire [6:0] stock_rx_addr; wire [79:0] stock_rx_data; wire stock_rx_val;
    uart_packet_wrapper_rx u_wrx (
        .clk(gclk), .reset(g_reset), .uart_rx_val(e_rx_val), .uart_rx_data(e_rx_data),
        .uartDisabled(1'b0), .rx_address(stock_rx_addr), .rx_data(stock_rx_data), .rx_data_val(stock_rx_val));
    integer stock_other = 0, stock_ours = 0;
    always @(posedge gclk) if (stock_rx_val) begin
        if (stock_rx_addr == 7'h11) stock_ours = stock_ours + 1;
        else stock_other = stock_other + 1;
    end

    wire g_esp_awake, g_unlocked; wire [15:0] g_crc_errors;
    net_window #(.WD_BITS(22)) dut (
        .hclk(hclk), .h_reset(h_reset),
        .h_cpu_addr(cpu_addr), .h_cpu_do(cpu_do), .h_cpu_rd_n(cpu_rd_n), .h_cpu_wr_stb(cpu_wr_stb),
        .h_sel(nw_sel), .h_do(nw_do),
        .gclk(gclk), .g_reset(g_reset),
        .g_uart_rx_data(e_rx_data), .g_uart_rx_val(e_rx_val),
        .g_tx_valid(g_tx_valid), .g_tx_len(g_tx_len), .g_tx_bytepos(tx_bytepos), .g_tx_byte(g_tx_byte),
        .g_tx_taken(g_tx_taken), .g_link_busy(link_busy),
        .g_esp_awake(g_esp_awake), .g_unlocked(g_unlocked), .g_crc_errors(g_crc_errors));

    // ------------------------------------------------------------ checks
    integer errors = 0, checks = 0;
    task check(input cond, input [8*96-1:0] msg);
        begin
            checks = checks + 1;
            if (!cond) begin errors = errors + 1; $display("FAIL: %0s  (t=%0t)", msg, $time); end
            else $display("ok:   %0s", msg);
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

    // ------------------------------------------------------------ ESP32: FPGA packet parser
    // Packets from address 0x0B land in a queue: type, id, a (16 bit), n data bytes.
    localparam QN = 512;
    reg  [7:0] pq_type [0:QN-1]; reg [7:0] pq_id [0:QN-1]; reg [15:0] pq_a [0:QN-1];
    integer    pq_n [0:QN-1]; integer pq_base [0:QN-1];
    reg  [7:0] pq_data [0:65535];
    reg [31:0] pq_ext [0:QN-1];
    integer pq_wr = 0, pq_rd = 0, pq_dw = 0, bad_pkts = 0, preamble_bytes = 0, txdata_pkts = 0, max_txdata = 0;
    integer ps = 0, p_len = 0, p_i = 0; reg [7:0] p_crc, p_addr; reg [7:0] p_buf [0:255];
    always @(posedge gclk) if (wire_stb) begin
        case (ps)
            0: if (wire_byte == 8'h8F) begin ps = 1; p_crc = crc8(8'hFF, 8'h8F); end
               else if (wire_byte == 8'h00) preamble_bytes = preamble_bytes + 1;
            1: begin p_addr = wire_byte; p_crc = crc8(p_crc, wire_byte); ps = 2; end
            2: begin p_len = wire_byte; p_i = 0; p_crc = crc8(p_crc, wire_byte); ps = (p_len == 0) ? 4 : 3; end
            3: begin p_buf[p_i] = wire_byte; p_i = p_i + 1; p_crc = crc8(p_crc, wire_byte); if (p_i == p_len) ps = 4; end
            4: begin
                ps = 0;
                if (p_crc != wire_byte || p_addr != 8'h0B || p_len < 4) begin
                    bad_pkts = bad_pkts + 1;
                    $display("  ESP: bad packet addr %02h len %0d crc %02h/%02h", p_addr, p_len, p_crc, wire_byte);
                end else begin : push
                    integer k;
                    pq_type[pq_wr % QN] = p_buf[0]; pq_id[pq_wr % QN] = p_buf[1];
                    pq_a[pq_wr % QN] = {p_buf[3], p_buf[2]};
                    pq_n[pq_wr % QN] = p_len - 4; pq_base[pq_wr % QN] = pq_dw;
                    pq_ext[pq_wr % QN] = (p_len >= 8) ? {p_buf[7], p_buf[6], p_buf[5], p_buf[4]} : 0;
                    for (k = 4; k < p_len; k = k + 1) begin pq_data[pq_dw % 65536] = p_buf[k]; pq_dw = pq_dw + 1; end
                    if (p_buf[0] == 8'd2) begin
                        txdata_pkts = txdata_pkts + 1;
                        if (p_len - 4 > max_txdata) max_txdata = p_len - 4;
                    end
                    pq_wr = pq_wr + 1;
                end
            end
        endcase
    end

    // ESP: wait for the next packet of want_type, skipping noise (see below);
    // any other packet type is returned as a mismatch.
    integer got;      // index of the packet returned
    task esp_next(input [7:0] want_type, input integer timeout_us);
        integer t; reg done;
        begin
            done = 0; got = -1;
            for (t = 0; t < timeout_us * 10 && !done; t = t + 1) begin
                while (pq_rd < pq_wr && !done) begin
                    // TXDATA still on the wire after an abort, RXACK and STATE
                    // (keepalive answers) are noise unless asked for.
                    if (pq_type[pq_rd % QN] == want_type ||
                        !(pq_type[pq_rd % QN] == 8'd2 || pq_type[pq_rd % QN] == 8'd4 || pq_type[pq_rd % QN] == 8'd7)) begin
                        got = pq_rd % QN; done = 1;
                    end
                    pq_rd = pq_rd + 1;
                end
                if (!done) #100;
            end
            if (got < 0) $display("  ESP: timeout waiting for packet type %0d", want_type);
            else if (pq_type[got] != want_type)
                $display("  ESP: got packet type %0d id %0d a %0d, wanted %0d", pq_type[got], pq_id[got], pq_a[got], want_type);
        end
    endtask
    task esp_flush; begin pq_rd = pq_wr; end endtask

    // ------------------------------------------------------------ ESP32: send to FPGA
    integer esp_gap = 10 * BIT;                // one UART byte time at 115200
    reg [7:0] eo [0:300];
    task esp_byte(input [7:0] b);
        begin
            @(posedge gclk); e_rx_data <= b; e_rx_val <= 1;
            @(posedge gclk); e_rx_val <= 0;
            repeat (esp_gap - 2) @(posedge gclk);
        end
    endtask
    // packet [8F, addr, n, eo[0..n-1], crc]; corrupt: flip the CRC
    task esp_send(input [6:0] addr, input integer n, input corrupt);
        integer i; reg [7:0] c;
        begin
            c = crc8(8'hFF, 8'h8F); c = crc8(c, {1'b0, addr}); c = crc8(c, n[7:0]);
            for (i = 0; i < n; i = i + 1) c = crc8(c, eo[i]);
            esp_byte(8'h8F); esp_byte({1'b0, addr}); esp_byte(n[7:0]);
            for (i = 0; i < n; i = i + 1) esp_byte(eo[i]);
            esp_byte(corrupt ? ~c : c);
        end
    endtask
    task esp_config(input [7:0] flags);
        begin eo[0] = 8'd1; eo[1] = flags; esp_send(7'h11, 2, 0); end
    endtask
    task esp_post(input [7:0] id, input [15:0] len);
        begin eo[0] = 8'd3; eo[1] = id; eo[2] = len[7:0]; eo[3] = len[15:8]; esp_send(7'h11, 4, 0); end
    endtask
    task esp_txread(input [7:0] id, input [15:0] off, input [15:0] cnt);
        begin
            eo[0] = 8'd4; eo[1] = id; eo[2] = off[7:0]; eo[3] = off[15:8]; eo[4] = cnt[7:0]; eo[5] = cnt[15:8];
            esp_send(7'h11, 6, 0);
        end
    endtask
    // RXDATA from the response image resp[] (byte i = resp_byte(i))
    function [7:0] resp_byte(input integer i); resp_byte = (i * 7 + 3) ^ (i >> 8); endfunction
    task esp_rxdata(input [7:0] id, input integer off, input integer n, input corrupt);
        integer i;
        begin
            eo[0] = 8'd2; eo[1] = id; eo[2] = off[7:0]; eo[3] = off[15:8];
            for (i = 0; i < n; i = i + 1) eo[4 + i] = resp_byte(off + i);
            esp_send(7'h11, 4 + n, corrupt);
        end
    endtask
    task esp_respond(input [7:0] id, input integer len, input integer chunk);
        integer off, n;
        begin
            off = 0;
            while (off < len) begin
                n = (len - off < chunk) ? len - off : chunk;
                esp_rxdata(id, off, n, 0);
                off = off + n;
            end
            esp_post(id, len[15:0]);
        end
    endtask

    // ------------------------------------------------------------ GB CPU
    // One M-cycle = 4 ce_cpu ticks. Address and RD_n change on a ce_cpu edge;
    // read data is taken just before the 4th edge (T3/T4), RD_n then rises.
    task ce_edge;
        begin @(posedge hclk); while (!ce_cpu) @(posedge hclk); #1; end
    endtask
    integer mcycles = 0;
    task mcycle_read(input [15:0] a, output [7:0] d);
        begin
            ce_edge; cpu_addr = a; cpu_rd_n = 0;
            ce_edge; ce_edge;
            @(posedge hclk); while (!ce_cpu) @(posedge hclk);
            d = cpu_di;                           // value seen at the T4 edge
            #1 cpu_rd_n = 1;
            mcycles = mcycles + 1;
        end
    endtask
    task mcycle_write(input [15:0] a, input [7:0] v);
        begin
            ce_edge; cpu_addr = a; cpu_rd_n = 1;
            ce_edge; cpu_wr_n = 0; cpu_do = v;
            ce_edge; ce_edge; cpu_wr_n = 1;
            mcycles = mcycles + 1;
        end
    endtask
    reg [7:0] junk;
    task fetch; begin mcycle_read(16'h0150, junk); end endtask
    // ldh [n],a: 3 M-cycles.  ldh a,[n]: 3 M-cycles.
    task gb_write(input [7:0] reg_lo, input [7:0] v);
        begin fetch; fetch; mcycle_write({8'hFF, reg_lo}, v); end
    endtask
    task gb_read(input [7:0] reg_lo, output [7:0] v);
        begin fetch; fetch; mcycle_read({8'hFF, reg_lo}, v); end
    endtask
    // ld [c],a / ld a,[c]: 2 M-cycles (the data-port loop instructions)
    task gb_write_c(input [7:0] v); begin fetch; mcycle_write(16'hFF5E, v); end endtask
    task gb_read_c(output [7:0] v); begin fetch; mcycle_read(16'hFF5E, v); end endtask

    reg [7:0] v, st;
    task stat; begin gb_read(8'h5B, st); end endtask
    task unlock; begin gb_write(8'h58, 8'h53); gb_write(8'h58, 8'h47); gb_write(8'h58, 8'h4E); gb_write(8'h58, 8'h57); end endtask
    task set_ptr(input [15:0] p); begin gb_write(8'h5C, p[7:0]); gb_write(8'h5D, p[15:8]); end endtask
    task cmd(input [7:0] c); begin gb_write(8'h5A, c); end endtask
    task get_err(output [7:0] e); begin gb_read(8'h5A, e); end endtask
    function [7:0] req_byte(input integer i); req_byte = 8'hA0 ^ i[7:0] ^ {i[10:8], 5'd0}; endfunction
    task write_request(input integer n);
        integer i; begin set_ptr(0); for (i = 0; i < n; i = i + 1) gb_write_c(req_byte(i)); end
    endtask
    task wait_stat_bit(input integer b, input integer timeout_us);
        integer t; begin
            stat; t = 0;
            while (!st[b] && t < timeout_us) begin #10000; t = t + 10; stat; end
        end
    endtask
    task keepalive_wait(input integer us);        // ESP idle with CONFIG keepalives every 5 ms
        integer t; begin
            for (t = 0; t < us; t = t + 5000) begin esp_config(8'h0B); #5000000; end
        end
    endtask

    // ------------------------------------------------------------ scenario
    integer i, n, ok_bytes, id, id2, t_start, base, bad;
    reg [7:0] e;
    initial begin
        if ($test$plusargs("fast")) esp_gap = 40;
        repeat (20) @(posedge gclk); g_reset = 0;
        repeat (20) @(posedge hclk); h_reset = 0;
        repeat (50) @(posedge hclk);

        $display("--- 1. locked: stock behaviour ---");
        bad = 0;
        for (i = 8'h58; i <= 8'h5F; i = i + 1) begin gb_read(i[7:0], v); if (v !== 8'hFF) bad = bad + 1; end
        check(bad == 0, "locked: FF58-FF5F all read 0xFF");
        gb_write(8'h5E, 8'h12); gb_write(8'h5A, 8'h01); gb_write(8'h5C, 8'h34);
        gb_read(8'h5C, v); check(v == 8'hFF, "locked: writes to other registers ignored");
        gb_write(8'h58, 8'h53); gb_write(8'h58, 8'h47); gb_write(8'h58, 8'h00); gb_write(8'h58, 8'h57);
        gb_read(8'h58, v); check(v == 8'hFF, "locked: broken key sequence stays locked");
        gb_write(8'h58, 8'h53); gb_write(8'h58, 8'h53); gb_write(8'h58, 8'h47); gb_write(8'h58, 8'h4E); gb_write(8'h58, 8'h57);
        gb_read(8'h58, v); check(v == 8'h4E, "key S S G N W unlocks (repeated S restarts the sequence)");
        gb_read(8'h59, v); check(v == 8'h10, "NW_VER = 0x10");
        gb_read(8'h5F, v); check(v == 8'hFF, "FF5F reserved reads 0xFF");
        stat; check(st == 8'h00, "STAT = 0 (no service yet)");
        esp_next(8'd7, 2000); check(got >= 0 && pq_a[got][0] == 1'b1, "ESP gets STATE(unlocked)");

        $display("--- 2. SEND without service ---");
        write_request(4); cmd(8'h01); get_err(e); stat;
        check(e == 8'd2 && st[2] && !st[0], "SEND with no service -> NOSVC, not busy");
        cmd(8'h00); get_err(e); check(e == 0, "CLR_ERR");

        $display("--- 3. ESP attaches ---");
        esp_config(8'h0B);                         // SVC | ONLINE | AWAKE
        esp_next(8'd7, 2000); check(got >= 0, "CONFIG answered with STATE");
        stat; check(st == 8'h18, "STAT = SVC|ONLINE");
        check(g_esp_awake === 1'b1, "g_esp_awake follows CONFIG bit 3");

        $display("--- 4. 300-byte request, back-to-back data-port writes ---");
        mcycles = 0;
        write_request(300);
        $display("  300 ld [c],a writes took %0d M-cycles (2 per byte; with ld a,[hl+] a copy loop is 4 per byte)", mcycles);
        gb_read(8'h5C, v); gb_read(8'h5D, e); check({e, v} == 16'd300, "PTR = 300 after 300 writes");
        cmd(8'h01); stat; check(st[0] && !st[1], "SEND -> BUSY");
        gb_read(8'h5C, v); check(v == 0, "SEND resets PTR");
        esp_next(8'd1, 5000); id = pq_id[got];
        check(got >= 0 && pq_type[got] == 1 && pq_a[got] == 300, "ESP: COMMIT(id, 300)");
        bad = 0; n = 0; base = 0;
        while (n < 300) begin
            esp_next(8'd2, 50000);
            if (got < 0 || pq_type[got] != 2 || pq_id[got] != id || pq_a[got] != n) begin bad = bad + 1; n = 300; end
            else begin
                for (i = 0; i < pq_n[got]; i = i + 1) if (pq_data[(pq_base[got] + i) % 65536] != req_byte(n + i)) bad = bad + 1;
                n = n + pq_n[got];
            end
        end
        check(bad == 0 && n == 300, "ESP: TXDATA 0..299 match what the game wrote");
        check(max_txdata == 128, "TXDATA chunk = 128 bytes");
        gb_write_c(8'h99); get_err(e); check(e == 8'd1, "data write while BUSY -> ERR BUSY");
        cmd(8'h00);
        cmd(8'h01); get_err(e); check(e == 8'd1, "SEND while BUSY -> ERR BUSY");
        cmd(8'h00);

        $display("--- 5. 1000-byte response, one corrupted chunk, data-port read loop ---");
        esp_rxdata(id, 0, 200, 0);
        esp_rxdata(id, 200, 200, 1);               // CRC broken: must not count
        esp_rxdata(id, 400, 200, 0);               // out of order now: NAK
        esp_next(8'd6, 20000); check(got >= 0 && pq_a[got] == 200, "ESP: RXNAK(have=200) for the chunk after the lost one");
        check(g_crc_errors == 1, "FPGA counted 1 CRC error");
        esp_post(id, 1000);
        esp_next(8'd6, 20000); check(got >= 0 && pq_a[got] == 200, "ESP: RXPOST(1000) early -> RXNAK(200)");
        stat; check(st[0] && !st[1], "game still BUSY until a good post");
        begin : esp_respond_from
            n = 200;
            while (n < 1000) begin esp_rxdata(id, n, (1000 - n < 200) ? 1000 - n : 200, 0); n = n + 200; end
            esp_post(id, 1000);
        end
        esp_next(8'd5, 20000); check(got >= 0 && pq_a[got] == 1000 && pq_id[got] == id, "ESP: POSTED(id, 1000)");
        wait_stat_bit(1, 2000); check(st[1] && !st[0], "STAT: RX_READY, not BUSY");
        set_ptr(0);
        mcycles = 0; t_start = $time; ok_bytes = 0;
        for (i = 0; i < 1000; i = i + 1) begin
            gb_read_c(v);
            mcycle_write(16'hC000 + i, v);         // ld [hl+],a -> WRAM (2 M-cycles with its fetch)
            fetch;
            if (v == resp_byte(i)) ok_bytes = ok_bytes + 1;
        end
        $display("  1000 bytes in %0d M-cycles = %0d us (%0d%% of a 16.74 ms frame), 4 M-cycles per byte",
                 mcycles, ($time - t_start) / 1000, (($time - t_start) / 1000) * 100 / 16742);
        check(ok_bytes == 1000, "game reads the 1000-byte response exactly (ldh a,[c] / ld [hl+],a loop)");
        get_err(e); check(e == 0, "no error after reading exactly the response");
        gb_read_c(v); get_err(e); check(v == 8'hFF && e == 8'd4, "read past the response -> 0xFF + RX_UNDERRUN");
        gb_read(8'h5C, v); gb_read(8'h5D, e); check({e, v} == 16'd1000, "PTR stays at 1000 after the underrun");
        cmd(8'h00);
        set_ptr(10); gb_read_c(v); check(v == resp_byte(10), "random access: PTR=10 reads byte 10");
        cmd(8'h02); stat; check(!st[1], "RX_ACK clears RX_READY");
        esp_next(8'd4, 20000); check(got >= 0 && pq_id[got] == id, "ESP: RXACK(id)");
        gb_read_c(v); get_err(e); check(v == 8'hFF && e == 8'd7, "read with no response -> 0xFF + NOT_READY");
        cmd(8'h00);

        $display("--- 6. TX overrun + full 2 KiB request with the link cable busy ---");
        set_ptr(2047); gb_write_c(8'h55); gb_write_c(8'h66); get_err(e);
        gb_read(8'h5C, v); gb_read(8'h5D, st);
        check(e == 8'd3 && {st, v} == 16'd2048, "write past 2 KiB -> TX_OVERRUN, PTR saturates at 2048");
        cmd(8'h00);
        set_ptr(3000); cmd(8'h01); get_err(e); check(e == 8'd5, "SEND with PTR > 2048 -> BADLEN");
        cmd(8'h00);
        set_ptr(0); cmd(8'h01); get_err(e); check(e == 8'd5, "SEND with PTR = 0 -> BADLEN");
        cmd(8'h00);
        // fill 2 KiB (pattern), SEND, check every TXDATA is 32 bytes while the link is busy
        set_ptr(0); for (i = 0; i < 2048; i = i + 1) gb_write_c(req_byte(i));
        link_busy = 1; max_txdata = 0; txdata_pkts = 0;
        cmd(8'h01);
        esp_next(8'd1, 5000); id = pq_id[got]; check(pq_a[got] == 2048, "ESP: COMMIT(2048)");
        bad = 0; n = 0;
        while (n < 2048) begin
            esp_next(8'd2, 50000);
            if (got < 0 || pq_a[got] != n) begin bad = bad + 1; n = 2048; end
            else begin
                for (i = 0; i < pq_n[got]; i = i + 1) if (pq_data[(pq_base[got] + i) % 65536] != req_byte(n + i)) bad = bad + 1;
                n = n + pq_n[got];
            end
        end
        check(bad == 0 && txdata_pkts == 64 && max_txdata == 32, "2048 bytes in 64 TXDATA of 32 (link busy)");
        link_busy = 0;
        // ESP lost one chunk: re-read 100 bytes from 500
        esp_txread(id, 500, 100);
        bad = 0; n = 500;
        while (n < 600) begin
            esp_next(8'd2, 50000);
            if (got < 0 || pq_a[got] != n) begin bad = bad + 1; n = 600; end
            else begin
                for (i = 0; i < pq_n[got]; i = i + 1) if (pq_data[(pq_base[got] + i) % 65536] != req_byte(n + i)) bad = bad + 1;
                n = n + pq_n[got];
            end
        end
        check(bad == 0 && n == 600, "TXREAD(500, 100) re-streams exactly 500..599");
        esp_txread(id, 2000, 500);
        esp_next(8'd2, 50000); check(got >= 0 && pq_a[got] == 2000 && pq_n[got] == 48, "TXREAD past the end is clipped to the request (48 bytes)");

        $display("--- 7. RX overrun: ESP sends more than 4 KiB ---");
        n = 0;
        while (n < 4096 + 200) begin esp_rxdata(id, n, 200, 0); n = n + 200; end
        esp_next(8'd6, 50000); check(got >= 0 && pq_a[got] == 4096, "RXDATA past 4 KiB is dropped: RXNAK(have=4096)");
        esp_post(id, 4097);
        esp_next(8'd6, 50000); check(got >= 0 && pq_a[got] == 4096, "RXPOST(4097) -> RXNAK(4096)");
        esp_post(id, 4096);
        esp_next(8'd5, 50000); check(got >= 0 && pq_a[got] == 4096, "RXPOST(4096) -> POSTED");
        wait_stat_bit(1, 2000);
        set_ptr(4090); bad = 0;
        for (i = 4090; i < 4096; i = i + 1) begin gb_read_c(v); if (v != resp_byte(i)) bad = bad + 1; end
        gb_read_c(v); get_err(e);
        check(bad == 0 && v == 8'hFF && e == 8'd4, "bytes 4090..4095 read, 4096 -> RX_UNDERRUN");
        cmd(8'h00);

        $display("--- 8. ABORT, stale response, new request ---");
        write_request(10); cmd(8'h01);
        esp_next(8'd1, 5000); id = pq_id[got];
        cmd(8'h03); stat; check(!st[0] && !st[1], "ABORT -> not BUSY");
        esp_next(8'd3, 20000); check(got >= 0 && pq_id[got] == id, "ESP: ABORT(id)");
        write_request(5); cmd(8'h01);
        esp_next(8'd1, 5000); id2 = pq_id[got]; check(id2 == ((id + 1) & 255), "new request gets the next id");
        esp_respond(id, 16, 16);                   // stale: id no longer active
        esp_next(8'd7, 20000); check(got >= 0 && pq_id[got] == id2, "stale RXPOST answered with STATE(active id2)");
        stat; check(st[0] && !st[1], "stale response ignored: still BUSY on id2");
        esp_respond(id2, 3, 16);
        esp_next(8'd5, 20000); wait_stat_bit(1, 2000);
        set_ptr(0); gb_read_c(v); check(st[1] && v == resp_byte(0), "id2 response arrives");

        $display("--- 9. game polls the DATA port while the response is posted ---");
        write_request(1); cmd(8'h01);
        esp_next(8'd1, 5000); id = pq_id[got];
        esp_rxdata(id, 0, 64, 0);
        fork
            begin esp_post(id, 64); end
            begin : poll
                integer k; reg [7:0] pv;
                n = 0; k = 0; bad = 0;
                // ldh a,[c] in a tight loop until the response shows up, then keep reading
                while (n < 64 && k < 200000) begin
                    gb_read_c(pv); k = k + 1;
                    if (n > 0 || pv != 8'hFF) begin
                        if (pv != resp_byte(n)) bad = bad + 1;
                        n = n + 1;
                    end
                end
            end
        join
        check(n == 64 && bad == 0, "polling reads: 0xFF until posted, then bytes 0..63 with none skipped");
        esp_next(8'd5, 20000); check(got >= 0 && pq_id[got] == id, "ESP: POSTED(id, 64)");
        cmd(8'h00);

        $display("--- 10. lock and GB reset abort an in-flight request ---");
        write_request(8); cmd(8'h01);
        esp_next(8'd1, 5000); id = pq_id[got];
        gb_write(8'h58, 8'h00);
        gb_read(8'h58, v); check(v == 8'hFF, "write 0 to FF58 locks (reads 0xFF again)");
        esp_next(8'd3, 20000); check(got >= 0 && pq_id[got] == id, "lock -> ESP: ABORT(id)");
        esp_next(8'd7, 20000); check(got >= 0 && pq_a[got][0] == 1'b0, "lock -> ESP: STATE(locked)");
        unlock; write_request(8); cmd(8'h01);
        esp_next(8'd1, 5000); id = pq_id[got];
        @(posedge hclk); h_reset <= 1; repeat (8) @(posedge hclk); h_reset <= 0;
        gb_read(8'h58, v); check(v == 8'hFF, "GB reset locks the window");
        esp_next(8'd3, 20000); check(got >= 0 && pq_id[got] == id, "GB reset -> ESP: ABORT(id)");
        esp_respond(id, 8, 8);
        esp_next(8'd7, 20000); check(got >= 0 && pq_a[got][1] == 1'b0, "response after the reset -> STATE(no active request)");

        $display("--- 11. CGB double speed ---");
        double_speed = 1;
        unlock; esp_config(8'h0B); esp_flush;
        mcycles = 0; t_start = $time;
        write_request(256);
        $display("  256 writes at double speed: %0d us", ($time - t_start) / 1000);
        cmd(8'h01);
        esp_next(8'd1, 5000); id = pq_id[got];
        bad = 0; n = 0;
        while (n < 256) begin
            esp_next(8'd2, 50000);
            if (got < 0 || pq_a[got] != n) begin bad = bad + 1; n = 256; end
            else begin
                for (i = 0; i < pq_n[got]; i = i + 1) if (pq_data[(pq_base[got] + i) % 65536] != req_byte(n + i)) bad = bad + 1;
                n = n + pq_n[got];
            end
        end
        check(bad == 0, "double speed: 256-byte request arrives intact");
        esp_respond(id, 512, 250);
        esp_next(8'd5, 50000); wait_stat_bit(1, 2000);
        set_ptr(0); ok_bytes = 0; t_start = $time;
        for (i = 0; i < 512; i = i + 1) begin gb_read_c(v); if (v == resp_byte(i)) ok_bytes = ok_bytes + 1; end
        $display("  512 reads at double speed: %0d us", ($time - t_start) / 1000);
        check(ok_bytes == 512, "double speed: 512-byte response read back-to-back");
        double_speed = 0;
        cmd(8'h02);

        $display("--- 12. unrelated UART traffic ---");
        esp_flush;
        eo[0] = 8'h8F; eo[1] = 8'h11; esp_send(7'h10, 2, 0);          // link cmd whose payload holds 8F 11
        eo[0] = 8'h03; eo[1] = 8'h01; esp_send(7'h10, 2, 0);
        for (i = 0; i < 10; i = i + 1) eo[i] = 8'h8F; esp_send(7'h0D, 10, 0);
        stat; get_err(e);
        check(st == 8'h18 && e == 0 && g_crc_errors == 1, "packets for other addresses (with 8F inside) are ignored");
        check(stock_other == 3, "stock RX wrapper still decodes the 3 other packets");

        $display("--- 13. service watchdog ---");
        write_request(4); cmd(8'h01);
        esp_next(8'd1, 5000);
        #(4194304.0 * 2.0 * gper * 1.05);          // 2^22 gclk (0.5 s at 8.39 MHz) with no ESP packet
        stat; get_err(e);
        check(!st[3] && !st[0] && e == 8'd8, "ESP silent -> SVC drops, request fails with SVC_LOST");
        esp_next(8'd3, 20000); check(got >= 0, "FPGA tells the (returning) ESP: ABORT");
        cmd(8'h00);
        esp_config(8'h0B); stat; check(st[3], "CONFIG brings the service back");

        $display("--- summary ---");
        check(bad_pkts == 0 && dropped == 0, "every FPGA->ESP packet had a good CRC, no UART byte dropped");
        check(stock_ours > 0, "stock RX wrapper saw our packets as address 0x11 (ignored by system_monitor)");
        $display("%0d checks, %0d errors", checks, errors);
        if (errors == 0) $display("PASS tb_net_window"); else $display("FAIL tb_net_window");
        $finish;
    end

    initial begin #3000000000; $display("FAIL: global timeout"); $finish; end
endmodule
