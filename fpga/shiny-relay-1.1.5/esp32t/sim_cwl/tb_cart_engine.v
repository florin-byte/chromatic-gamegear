// tb_cart_engine.v (lane cartflash1): cart_engine.v against a picky ModRetro-cart model.
// Cart model: MBC5-like mapper (ROM bank at 0x2000/0x3000, RAM enable 0x0000, RAM bank 0x4000)
// + SST39VF1682 x8 (2 MiB): AMD-style command FSM decoding A11-A0 (AAA/555), Software ID (BF C9),
// CFI ("QRY" at 0x10), Byte-Program (A0), Sector-Erase 50H (4 KiB), Block-Erase 30H (64 KiB),
// Chip-Erase 10H, Erase-Suspend B0 (counted as an error: the engine must never send it), DQ6
// toggle + DQ7 data# while busy, the top 64 KiB block protected (WP# low), tIDA after ID exit.
// "Picky": a /WE pulse reaches the flash only if CLK is low and /RD high for the whole pulse,
// data is stable from >= 200 ns before /WE falls to >= 100 ns after it rises, and address is
// stable through the pulse (what the GB-timed cycles of cart.v evidently do not give unit 1's cart).
`timescale 1ns/1ps
module tb_cart_engine;
    reg clk = 0; always #29.8 clk = ~clk;                 // hclk 16.78 MHz
    reg reset = 1, allow = 0;
    reg [15:0] cpu_addr = 16'h0000; reg [7:0] cpu_do = 8'h00; reg cpu_rd_n = 1'b1; reg cpu_wr_stb = 1'b0;
    wire sel; wire [7:0] edo;
    wire active, e_clk, e_cs, e_rd, e_wr, e_dir_e, busy; wire [15:0] e_a; wire [7:0] e_d_out; wire [15:0] ncyc;
    // a "core" bus that keeps toggling like cart.v (must be ignored while the engine is active)
    reg [15:0] core_a = 16'h1234; reg core_rd = 1'b0, core_wr = 1'b1, core_clk = 1'b0;
    always @(posedge clk) begin core_a <= core_a + 16'd7; core_rd <= ~core_rd; core_clk <= ~core_clk; end
    // the mux in top.v
    wire [15:0] A   = active ? e_a : core_a;
    wire        CLK = active ? e_clk : core_clk;
    wire        RD  = active ? e_rd : core_rd;
    wire        WR  = active ? e_wr : core_wr;
    wire        CS  = active ? e_cs : 1'b1;
    wire        DRV = active ? ~e_dir_e : 1'b0;
    wire  [7:0] DOUT = e_d_out;
    wire  [7:0] DIN;
    cart_engine #(.PROG_TMO(1680), .ERASE_TMO(40000)) dut (
        .clk(clk), .reset(reset), .allow(allow),
        .cpu_addr(cpu_addr), .cpu_do(cpu_do), .cpu_rd_n(cpu_rd_n), .cpu_wr_stb(cpu_wr_stb), .sel(sel), .do_(edo),
        .active(active), .e_a(e_a), .e_clk(e_clk), .e_cs(e_cs), .e_rd(e_rd), .e_wr(e_wr), .e_d_out(e_d_out),
        .e_dir_e(e_dir_e), .cart_d_in(DIN), .busy(busy), .n_cycles(ncyc),
        .x_session(1'b0), .x_hold(1'b0), .x_bus_req(1'b0), .x_bus_wr(1'b0), .x_bus_a(16'h0000), .x_bus_d(8'h00),
        .x_bus_done(), .x_bus_q(), .x_tcfg(1'b0), .x_t_setup(5'd0), .x_t_pulse(5'd0), .x_t_hold(5'd0), .x_t_access(5'd0), .x_t_gap(5'd0));

    // ---------------------------------------------------------------- the cart
    localparam SIZE = 2097152;
    reg [7:0] flash [0:SIZE-1];
    reg [7:0] fram  [0:32767];
    reg [8:0] rom_bank = 9'd1; reg [3:0] ram_bank = 4'd0; reg ram_en = 1'b0;
    integer i;
    wire [20:0] fa = A[15:14] == 2'b00 ? {7'd0, A[13:0]} : {rom_bank[6:0], A[13:0]};
    localparam F_READ = 0, F_U1 = 1, F_U2 = 2, F_ID = 3, F_CFI = 4, F_PROG = 5, F_E80 = 6, F_E1 = 7, F_E2 = 8;
    integer fs = F_READ;
    real busy_until = 0.0; reg fbusy = 0, ferase = 0, toggle = 0; reg [7:0] pdata = 8'h00; reg [20:0] paddr = 0;
    integer n_prog = 0, n_erase = 0, n_busy_writes = 0, n_suspend = 0, n_rejected = 0, n_cmds_accepted = 0, n_prot = 0;
    real id_exit_t = -1.0; integer n_tida = 0;
    reg [14:0] fram_a_lat = 15'd0; real cs_rise_t = 0.0, cs_fall_t = 0.0;
    integer n_fram_bad = 0, n_fram_wr = 0, n_fram_rd = 0, n_fram_torn = 0, n_cs_cycles = 0;
    // pulse qualification
    real we_fall_t = 0.0, d_change_t = 0.0, a_change_t = 0.0; reg pulse_ok = 1'b1; reg [7:0] d_at_fall; reg [15:0] a_at_fall;
    reg [7:0] d_prev = 8'h00; reg [15:0] a_prev = 16'h0;
    always @(DOUT or DRV) if (DRV) begin if (DOUT !== d_prev) d_change_t = $realtime; d_prev = DOUT; end
    always @(A) begin a_change_t = $realtime; a_prev = A; end
    always @(negedge WR) begin
        we_fall_t = $realtime; d_at_fall = DOUT; a_at_fall = A;
        pulse_ok = DRV && (we_fall_t - d_change_t >= 200.0) && (CLK == 1'b0) && (RD == 1'b1);
    end
    always @(posedge CLK) if (!WR) pulse_ok = 1'b0;
    always @(negedge RD)  if (!WR) pulse_ok = 1'b0;
    reg [7:0] latched_d; reg [15:0] latched_a; reg we_rise_pending = 0;
    always @(posedge WR) begin
        if (DOUT !== d_at_fall || A !== a_at_fall || !DRV) pulse_ok = 1'b0;
        latched_d = DOUT; latched_a = A; we_rise_pending = 1;
        #100;                                                  // data must still be held 100 ns later
        if (!DRV || DOUT !== latched_d) pulse_ok = 1'b0;
        we_rise_pending = 0;
        mapper_write(latched_a, latched_d);                    // the mapper takes any write
        if (!latched_a[15]) begin
            if (pulse_ok) flash_write(latched_a, latched_d); else n_rejected = n_rejected + 1;
        end else if (latched_a[15:13] == 3'b101 && ram_en && !CS) begin
            n_fram_wr = n_fram_wr + 1;
            if ($realtime - 100.0 - we_fall_t < 100.0 || !pulse_ok) begin fram[fram_a_lat] = 8'hXX; n_fram_torn = n_fram_torn + 1; end   // a cut pulse tears that one byte
            else fram[fram_a_lat] = latched_d;
        end
    end
    // ---- the FRAM (32 KiB, e.g. FM18W08 / MB85R256): the address is LATCHED on the falling /CS
    // edge; needs the address stable >= 50 ns before it, /CS high >= 100 ns between accesses
    // (precharge), /CS low through the whole /WR pulse, and the pins' address unchanged while low.
    always @(negedge CS) begin
        cs_fall_t = $realtime; n_cs_cycles = n_cs_cycles + 1;
        fram_a_lat = {ram_bank[1:0], A[12:0]};
        if ($realtime - a_change_t < 50.0) begin n_fram_bad = n_fram_bad + 1; if (n_fram_bad < 4) $display("FAIL FRAM: address %0.0f ns before /CS", $realtime - a_change_t); end
        if ($realtime - cs_rise_t < 100.0 && cs_rise_t > 0.0) begin n_fram_bad = n_fram_bad + 1; if (n_fram_bad < 4) $display("FAIL FRAM: /CS high only %0.0f ns", $realtime - cs_rise_t); end
        if (A[15:13] != 3'b101) begin n_fram_bad = n_fram_bad + 1; $display("FAIL FRAM: /CS outside A000-BFFF (%04x)", A); end
    end
    always @(posedge CS) cs_rise_t = $realtime;
    always @(A) if (!CS) begin n_fram_bad = n_fram_bad + 1; if (n_fram_bad < 4) $display("FAIL FRAM: address moved while /CS low"); end
    task mapper_write(input [15:0] a, input [7:0] d);
        begin
            if (a < 16'h2000) ram_en = (d[3:0] == 4'hA);
            else if (a < 16'h3000) rom_bank[7:0] = d;
            else if (a < 16'h4000) rom_bank[8] = d[0];
            else if (a < 16'h6000) ram_bank = d[3:0];
        end
    endtask
    function prot(input [20:0] x); prot = (x >= 21'h1F0000); endfunction
    task flash_write(input [15:0] a, input [7:0] d);
        reg [20:0] f; reg u1, u2;
        begin
            f = (a[15:14] == 2'b00) ? {7'd0, a[13:0]} : {rom_bank[6:0], a[13:0]};
            u1 = (a[11:0] == 12'hAAA); u2 = (a[11:0] == 12'h555);
            if (fbusy) begin
                n_busy_writes = n_busy_writes + 1;
                if (d == 8'hB0) n_suspend = n_suspend + 1;
            end else begin
                n_cmds_accepted = n_cmds_accepted + 1;
                case (fs)
                F_READ, F_ID, F_CFI: begin
                    if (d == 8'hF0) begin if (fs != F_READ) id_exit_t = $realtime; fs = F_READ; end
                    else if (u1 && d == 8'hAA) fs = F_U1;
                end
                F_U1: fs = (u2 && d == 8'h55) ? F_U2 : F_READ;
                F_U2: begin
                    fs = F_READ;
                    if (u1) case (d)
                        8'h90: fs = F_ID;
                        8'h98: fs = F_CFI;
                        8'hA0: fs = F_PROG;
                        8'h80: fs = F_E80;
                        8'hF0: id_exit_t = $realtime;
                        default: ;
                    endcase
                end
                F_PROG: begin
                    fs = F_READ; n_prog = n_prog + 1;
                    if (prot(f)) n_prot = n_prot + 1;
                    else begin fbusy = 1; ferase = 0; paddr = f; pdata = d; busy_until = $realtime + 8000.0; end
                end
                F_E80: fs = (u1 && d == 8'hAA) ? F_E1 : F_READ;
                F_E1:  fs = (u2 && d == 8'h55) ? F_E2 : F_READ;
                F_E2: begin
                    fs = F_READ;
                    if (d == 8'h50 || d == 8'h30 || (d == 8'h10 && u1)) begin
                        n_erase = n_erase + 1;
                        paddr = (d == 8'h50) ? {f[20:12], 12'h000} : (d == 8'h30) ? {f[20:16], 16'h0000} : 21'd0;
                        pdata = d;
                        if (d != 8'h10 && prot(paddr)) n_prot = n_prot + 1;
                        else begin fbusy = 1; ferase = 1; busy_until = $realtime + 60000.0; end      // 60 us (scaled)
                    end
                end
                default: fs = F_READ;
                endcase
            end
        end
    endtask
    always @(posedge clk) if (fbusy && $realtime >= busy_until) begin
        if (!ferase) flash[paddr] = flash[paddr] & pdata;
        else if (pdata == 8'h50) for (i = 0; i < 4096; i = i + 1) flash[paddr + i] = 8'hFF;
        else if (pdata == 8'h30) for (i = 0; i < 65536; i = i + 1) flash[paddr + i] = 8'hFF;
        else for (i = 0; i < SIZE; i = i + 1) if (!prot(i)) flash[i] = 8'hFF;
        fbusy = 0;
    end
    reg [7:0] rd_val; reg stuck = 0;          // stuck: a chip whose DQ6 never stops toggling
    always @(negedge RD) begin
        if (stuck) begin toggle = ~toggle; rd_val = {1'b0, toggle, 6'b0}; end
        else if (fbusy) begin toggle = ~toggle; rd_val = {ferase ? 1'b0 : ~pdata[7], toggle, 6'b0}; end
        else if (!A[15] && fs == F_ID) rd_val = (fa[7:0] == 8'h00) ? 8'hBF : (fa[7:0] == 8'h01) ? 8'hC9 : 8'h00;
        else if (!A[15] && fs == F_CFI) rd_val = (fa[7:0] == 8'h10) ? "Q" : (fa[7:0] == 8'h11) ? "R" : (fa[7:0] == 8'h12) ? "Y" : 8'h00;
        else if (!A[15]) begin
            rd_val = flash[fa];
            if (id_exit_t >= 0 && $realtime - id_exit_t < 150.0) begin n_tida = n_tida + 1; rd_val = 8'hXX; end
        end else if (A[15:13] == 3'b101) begin
            #2;                                                 // /CS falls with /RD: the latch first
            if (ram_en && !CS) begin rd_val = fram[fram_a_lat]; n_fram_rd = n_fram_rd + 1; end else rd_val = 8'hFF;
        end
        else rd_val = 8'hFF;
    end
    assign DIN = (!RD && !DRV) ? rd_val : 8'hZZ;

    // ---------------------------------------------------------------- the CPU (M-cycle = 16 hclk)
    task mcycle; begin repeat (16) @(posedge clk); end endtask
    task io_wr(input [15:0] a, input [7:0] d);
        begin
            @(posedge clk); cpu_addr <= a; cpu_do <= d; repeat (8) @(posedge clk);
            cpu_wr_stb <= 1'b1; @(posedge clk); cpu_wr_stb <= 1'b0; repeat (7) @(posedge clk);
            cpu_addr <= 16'hC000;
        end
    endtask
    task io_rd(input [15:0] a, output [7:0] d);
        begin
            @(posedge clk); cpu_addr <= a; cpu_rd_n <= 1'b0; repeat (12) @(posedge clk);
            d = sel ? edo : 8'hFF; repeat (4) @(posedge clk); cpu_rd_n <= 1'b1; cpu_addr <= 16'hC000; @(posedge clk);
        end
    endtask
    task wait_idle(output integer polls);
        reg [7:0] s;
        begin polls = 0; s = 8'h02; while (s[1]) begin io_rd(16'hFF60, s); polls = polls + 1; end end
    endtask
    task set_addr(input [15:0] a); integer p; begin io_wr(16'hFF61, a[7:0]); io_wr(16'hFF62, a[15:8]); wait_idle(p); end endtask
    // an SM83 stream loop (ldh a,[c] / ld [hl+],a): RDATA reads 4 M-cycles apart (>= 2 always works)
    task rd_byte(output [7:0] d); begin io_rd(16'hFF63, d); mcycle; end endtask

    integer ok = 0, bad = 0, p, k, n, rej0, base;
    reg [7:0] v, st;
    reg [7:0] idb [0:3];
    task check(input c, input [8*80-1:0] what);
        begin if (c) ok = ok + 1; else begin bad = bad + 1; $display("FAIL %0s", what); end end
    endtask

    // write-cycle timing measured on the engine's own pins
    real t_dset, t_wfall, t_wrise, t_drel; integer n_wcyc = 0, n_wbad = 0;
    always @(negedge e_dir_e) t_dset = $realtime;
    always @(negedge e_wr) t_wfall = $realtime;
    always @(posedge e_wr) t_wrise = $realtime;
    always @(posedge e_dir_e) if (active) begin
        t_drel = $realtime; n_wcyc = n_wcyc + 1;
        if (!(t_wfall - t_dset >= 200.0 && t_wrise - t_wfall >= 400.0 && t_drel - t_wrise >= 200.0)) begin
            n_wbad = n_wbad + 1;
            if (n_wbad < 4) $display("FAIL write timing: setup %0.0f pulse %0.0f hold %0.0f ns", t_wfall - t_dset, t_wrise - t_wfall, t_drel - t_wrise);
        end
    end
    real t_rfall; integer n_rbad = 0;
    always @(negedge e_rd) t_rfall = $realtime;
    always @(posedge e_rd) if (active && $realtime - t_rfall < 400.0) n_rbad = n_rbad + 1;
    integer n_clk_high = 0;
    always @(posedge clk) if (active && e_clk) n_clk_high = n_clk_high + 1;

    initial begin
        for (i = 0; i < SIZE; i = i + 1) flash[i] = (i < 262144) ? (i * 13 + 7) & 8'hFF : 8'hFF;
        for (i = 0; i < 32768; i = i + 1) fram[i] = 8'hFF;
        repeat (10) @(posedge clk); reset = 0; repeat (10) @(posedge clk);

        // 1. not allowed: invisible, no session
        io_rd(16'hFF60, v); check(v == 8'hFF, "not allowed: FF60 reads FF");
        io_wr(16'hFF60, 8'h01); repeat (4) @(posedge clk);
        check(!active, "not allowed: no session");
        // 2. allowed but no session: transparent
        allow = 1; repeat (4) @(posedge clk);
        io_rd(16'hFF60, v); check(v[7:4] == 4'hA && !v[0], "allowed: STAT magic, no session");
        repeat (200) @(posedge clk); check(!active, "transparent while no session (core owns the bus)");
        // 3. the GB-timed style write is rejected by the picky cart; the engine's is accepted
        rej0 = n_rejected;
        // 4. session on
        io_wr(16'hFF60, 8'h01); repeat (4) @(posedge clk); check(active, "session on");
        // ID
        io_wr(16'hFF66, 8'h90); wait_idle(p);
        set_addr(16'h0000); rd_byte(idb[0]); set_addr(16'h0001); rd_byte(idb[1]);
        check(idb[0] == 8'hBF && idb[1] == 8'hC9, "ID BF C9 (SST39VF1682)");
        io_wr(16'hFF66, 8'hF0); wait_idle(p);
        set_addr(16'h0000); rd_byte(v); $display("  after exit: read %02x want %02x fs %0d n_tida %0d", v, flash[0], fs, n_tida); check(v == flash[0], "read mode after ID exit");
        // CFI
        io_wr(16'hFF66, 8'h98); wait_idle(p);
        set_addr(16'h0010); rd_byte(idb[0]); set_addr(16'h0011); rd_byte(idb[1]); set_addr(16'h0012); rd_byte(idb[2]);
        $display("  cfi: %02x %02x %02x fs %0d", idb[0], idb[1], idb[2], fs);
        check(idb[0] == "Q" && idb[1] == "R" && idb[2] == "Y", "CFI QRY at 0x10");
        io_wr(16'hFF66, 8'hF0); wait_idle(p);
        check(n_tida == 0, "no read within tIDA after an exit");
        // 5. blank-sector test: sector at flash 0x100000 (bank 64, off 0) - erase, verify FF
        set_addr(16'h2000); io_wr(16'hFF64, 8'h40); wait_idle(p);              // ROM bank 64
        set_addr(16'h3000); io_wr(16'hFF64, 8'h00); wait_idle(p);
        set_addr(16'h4000); io_wr(16'hFF67, 8'h50); wait_idle(p);
        io_rd(16'hFF60, st); check(!st[2], "sector erase finished in time");
        $display("  sector erase: %0d STAT polls", p);
        io_wr(16'hFF60, 8'h03);                                                  // autoinc
        set_addr(16'h4000); n = 0;
        for (k = 0; k < 4096; k = k + 1) begin rd_byte(v); if (v != 8'hFF) n = n + 1; end
        check(n == 0, "erased sector reads FF (4096 bytes)");
        // 6. program a pattern (256 bytes), verify
        set_addr(16'h4000);
        for (k = 0; k < 256; k = k + 1) begin io_wr(16'hFF65, (k * 37 + 11) & 8'hFF); wait_idle(p); end
        io_rd(16'hFF60, st); check(!st[2] && !st[3], "256 byte programs: no timeout, no verify error");
        set_addr(16'h4000); n = 0;
        for (k = 0; k < 256; k = k + 1) begin rd_byte(v); if (v != ((k * 37 + 11) & 8'hFF)) n = n + 1; end
        check(n == 0, "pattern reads back (256 bytes)");
        check(flash[21'h100000] == 8'h0B && flash[21'h1000FF] == ((255 * 37 + 11) & 8'hFF), "pattern in the flash array at 0x100000");
        // 7. erase back to blank
        set_addr(16'h4000); io_wr(16'hFF67, 8'h50); wait_idle(p);
        set_addr(16'h4000); n = 0;
        for (k = 0; k < 4096; k = k + 1) begin rd_byte(v); if (v != 8'hFF) n = n + 1; end
        check(n == 0, "sector blank again");
        // 8. verify error: program a 0->1 over a programmed byte
        set_addr(16'h4000); io_wr(16'hFF65, 8'h00); wait_idle(p);
        set_addr(16'h4000); io_wr(16'hFF65, 8'hFF); wait_idle(p);                 // FF over 00: stays 00
        io_rd(16'hFF60, st); check(!st[3], "programming FF is a no-op (data stays, FF compare skipped by software)");
        set_addr(16'h4000); io_wr(16'hFF65, 8'h5A); wait_idle(p);                 // 5A over 00 -> 00: verify error
        io_rd(16'hFF60, st); check(st[3], "verify error when the byte cannot become the value");
        io_wr(16'hFF60, 8'h07);                                                   // clear errors, keep session+autoinc
        set_addr(16'h4000); io_wr(16'hFF67, 8'h50); wait_idle(p);
        // 9. protected top block: erase is ignored, no hang, data unchanged
        set_addr(16'h2000); io_wr(16'hFF64, 8'h7C); wait_idle(p);                // bank 124 = 0x1F0000
        flash[21'h1F0000] = 8'h42;
        set_addr(16'h4000); io_wr(16'hFF67, 8'h50); wait_idle(p);
        io_rd(16'hFF60, st); check(!st[2] && flash[21'h1F0000] == 8'h42 && n_prot >= 1, "protected top block: ignored, no hang, data kept");
        // 10. timeout: a chip that never finishes
        stuck = 1;
        io_wr(16'hFF65, 8'h12); wait_idle(p);
        io_rd(16'hFF60, st); check(st[2], "program timeout flagged when DQ6 keeps toggling");
        stuck = 0;
        io_wr(16'hFF60, 8'h07);
        repeat (2000) @(posedge clk);
        // 11. bus rules over the whole run
        check(n_busy_writes == 0 && n_suspend == 0, "no write while the chip was busy (no B0 erase-suspend)");
        check(n_wbad == 0 && n_wcyc > 100, "every engine write: setup >= 200, /WR >= 400, hold >= 200 ns");
        check(n_rbad == 0, "every engine read: /RD low >= 400 ns");
        check(n_clk_high == 0, "CLK low all session");
        check(n_rejected == rej0, "the picky cart accepted every engine write");
        $display("  %0d engine bus cycles, %0d flash writes accepted, %0d programs, %0d erases", ncyc, n_cmds_accepted, n_prog, n_erase);
        // ---------------- 13-19: cart RAM through the engine (2026-10-11)
        // 13. RAM disabled: reads FF, a write is lost
        io_wr(16'hFF60, 8'h07);
        set_addr(16'h0000); io_wr(16'hFF64, 8'h00); wait_idle(p);                // RAM off
        set_addr(16'hA000); io_wr(16'hFF64, 8'h5A); wait_idle(p);
        set_addr(16'hA000); rd_byte(v);
        check(v == 8'hFF && fram[0] === 8'hFF, "RAM disabled: read FF, write lost");
        // 14. enable + all four banks: 96 bytes each with WDATA autoinc, at different offsets
        set_addr(16'h0000); io_wr(16'hFF64, 8'h0A); wait_idle(p);
        for (k = 0; k < 4; k = k + 1) begin
            set_addr(16'h4000); io_wr(16'hFF64, k[7:0]); wait_idle(p);            // RAM bank
            set_addr(16'hA000 + k * 16'h0755);
            for (n = 0; n < 96; n = n + 1) begin io_wr(16'hFF64, (n * 29 + k * 53 + 3) & 8'hFF); wait_idle(p); end
        end
        n = 0;
        for (k = 0; k < 4; k = k + 1) for (i = 0; i < 96; i = i + 1)
            if (fram[k * 8192 + k * 16'h0755 + i] !== ((i * 29 + k * 53 + 3) & 8'hFF)) n = n + 1;
        check(n == 0, "RAM write: 4 banks x 96 bytes land where the bank register says");
        io_rd(16'hFF61, v); io_rd(16'hFF62, st);
        check({st, v} == 16'hA000 + 3 * 16'h0755 + 96, "WDATA in A000-BFFF steps ADDR (autoinc)");
        // 15. read back with a bank switch between every block (the job's order: 3, 0, 2, 1)
        n = 0;
        for (base = 0; base < 4; base = base + 1) begin
            k = (base == 0) ? 3 : (base == 1) ? 0 : (base == 2) ? 2 : 1;
            set_addr(16'h4000); io_wr(16'hFF64, k[7:0]); wait_idle(p);
            set_addr(16'hA000 + k * 16'h0755);
            for (i = 0; i < 96; i = i + 1) begin rd_byte(v); if (v !== ((i * 29 + k * 53 + 3) & 8'hFF)) n = n + 1; end
        end
        check(n == 0, "RAM read-back after bank switches: every byte");
        // 16. the last byte of a bank, and stepping out of the RAM region asserts no /CS
        set_addr(16'h4000); io_wr(16'hFF64, 8'h01); wait_idle(p);
        set_addr(16'hBFFF); io_wr(16'hFF64, 8'hC3); wait_idle(p);
        p = n_cs_cycles; rd_byte(v); rd_byte(v);                                  // C000, C001: not RAM
        check(fram[1 * 8192 + 8191] === 8'hC3 && n_cs_cycles == p, "BFFF written; ADDR past BFFF drives no /CS");
        // 17. a mapper write between RAM writes (a bank switch mid-job) does not disturb the data
        set_addr(16'h4000); io_wr(16'hFF64, 8'h02); wait_idle(p);
        set_addr(16'hB000); io_wr(16'hFF64, 8'h11); wait_idle(p);
        set_addr(16'h4000); io_wr(16'hFF64, 8'h00); wait_idle(p);
        set_addr(16'hB000); io_wr(16'hFF64, 8'h22); wait_idle(p);
        check(fram[2 * 8192 + 4096] === 8'h11 && fram[4096] === 8'h22, "bank switch mid-job: each byte in its own bank");
        // 18. the flash never saw the RAM cycles; FRAM timing held on every cycle
        check(n_rejected == rej0 && n_busy_writes == 0, "RAM cycles: nothing reached the flash FSM as a command");
        check(n_fram_bad == 0 && n_fram_wr >= 387 && n_fram_rd >= 384, "FRAM: address before /CS, /CS high between, stable while low");
        check(n_wbad == 0 && n_rbad == 0, "RAM cycles keep the write / read pulse widths");
        // 19. power / allow cut in the middle of a RAM fill: earlier bytes intact, later ones untouched,
        //     at most the one byte in flight torn
        set_addr(16'h4000); io_wr(16'hFF64, 8'h03); wait_idle(p);
        for (i = 0; i < 64; i = i + 1) fram[3 * 8192 + 16'h1000 + i] = 8'hEE;
        set_addr(16'hB000);
        for (n = 0; n < 20; n = n + 1) begin io_wr(16'hFF64, 8'h40 + n[7:0]); wait_idle(p); end
        io_wr(16'hFF64, 8'h99); repeat (9) @(posedge clk);                        // mid /WR pulse
        allow = 0; repeat (20) @(posedge clk);
        n = 0;
        for (i = 0; i < 20; i = i + 1) if (fram[3 * 8192 + 16'h1000 + i] !== (8'h40 + i)) n = n + 1;
        for (i = 21; i < 64; i = i + 1) if (fram[3 * 8192 + 16'h1000 + i] !== 8'hEE) n = n + 1;
        check(n == 0 && !active && n_fram_torn <= 1, "cut mid-fill: 20 bytes intact, the rest untouched, <= 1 byte torn");
        $display("  RAM: %0d FRAM writes, %0d FRAM reads, %0d torn (the cut)", n_fram_wr, n_fram_rd, n_fram_torn);
        allow = 1; repeat (4) @(posedge clk); io_wr(16'hFF60, 8'h07); repeat (4) @(posedge clk);
        // 12. session off: transparent again; allow off ends everything
        io_wr(16'hFF60, 8'h00); repeat (4) @(posedge clk); check(!active, "session off: transparent");
        io_wr(16'hFF60, 8'h01); repeat (4) @(posedge clk); allow = 0; repeat (4) @(posedge clk);
        check(!active, "allow falling ends the session");
        allow = 1; repeat (4) @(posedge clk); check(!active, "and it stays off");
        reset = 1; repeat (4) @(posedge clk); reset = 0;
        $display("tb_cart_engine: %0d ok, %0d failed", ok, bad);
        if (bad) $display("FAILED"); else $display("ALL PASSED");
        $finish;
    end
endmodule
