// Trace recorder for the Game Gear core on the Chromatic (build gg5diag, reviews CG-111 / CG-112).
//
// Read with the ESP32 console command `rc dbg` (bench word of 48 bits: a = [15:0], fills = [31:16],
// flags = [47:32]). Every read (op 35 in cart_xs.v) returns the current word and moves to the next one:
//   0 .. 1023      main trace: 256 entries x 4 parts   {2'b00, entry[7:0], part[1:0], data[35:0]}
//   1024 .. 2047   pclk trace: 512 entries x 2 halves  {2'b01, entry[8:0], half, data[35:0]}
//   2048 .. 2055   summary: 8 slots                    {2'b10, slot[2:0], data[42:0]}
// then again from 0. A new game load (hold) clears the recorder and the read position.
//
// Main trace: one entry at every CPU clock enable after the game's reset is released (values before the edge):
//   part 0  {pc[15:0], A[15:0], MCycle[2:0], M1_n}
//   part 1  {rom_do[7:0], D_out[7:0], GENIE_DI[7:0], IR[7:0], TState[2:0], MREQ_n}
//   part 2  {sp[15:0], ACC[7:0], F[7:0], IORQ_n, RD_n, WR_n, halt}
//   part 3  {rom_a[21:0], rom_stall, stall_r, rom_ok, bootloader_n, cart_selected, ram_we, IFF1, IRQ_n,
//            IntCycle, pclk_since_previous[4:0] (saturates)}
//   rom_do = what the RAM cart cache gives, D_out = the core's memory / I/O mux, GENIE_DI = the Z80's data input.
//
// pclk trace: one entry per pclk, circular, frozen 256 pclk after the first time the ROM stall falls with the
// game running (the first resume after a cache miss); entry `trig` (summary slot 1) is the pclk of that fall:
//   half 0  {A[15:0], rom_do[7:0], GENIE_DI[7:0], MCycle[2:0], ce_cpu}
//   half 1  {IR[7:0], TState[2:0], run, rom_stall, stall_r, rom_ok, q_cnt[1:0], MCycles[2:0], TStates[2:0],
//            ISet[1:0], Read_To_Reg_r[4:0], ALU_Op_r[3:0], Save_ALU_r, M1_n}
//
// Summary:
//   slot 0  {crc16[15:0], fills[7:0], status[18:0]}   CRC-16/CCITT of the bytes replayed to the core at load
//           status = {6'd0, pclk_frozen, pclk_triggered, main_full, reset_again, fill_err, seq_err, crc_done,
//                     alt, stall_r, core_reset, idle, bad, loaded}
//   slot 1  {replayed[21:0], main_entries[8:0], trig[8:0], 3'd0}
//   slot 2  {vdp_regs[15:0], cram0[11:0], cram16[11:0], 3'd0}
//   slot 3  osd[44:2]   from vid_system_top: {first[15:0], last[15:0], count[7:0], seen, 2'd0}
//   slot 4  {pc[15:0], sp[15:0], 11'd0}   live
module gg_trace (
    input             clk,                // pclk
    input             clear,              // a game load starts (RAM cart hold)
    input             core_reset, loaded, bad, idle, stall_r, rom_ok, alt, run,
    input             rom_stall,          // the RAM cart's raw stall
    input      [1:0]  q_cnt,
    input             ce_cpu,
    input      [15:0] pc, sp,
    input      [7:0]  acc, f,
    input             halt, iff1,
    input      [21:0] rom_a,
    input      [7:0]  rom_do,
    input      [79:0] core,               // system.vhd dbg_out
    input             ram_we,
    input             dl_en,
    input      [24:0] dl_a,
    input      [7:0]  dl_d,
    input      [15:0] vdp_regs,
    input      [11:0] cram0, cram16,
    input             fill_err,
    input      [7:0]  fills,
    input      [44:0] osd,
    input             rd_tog,             // toggles at every read of the bench word (hclk)
    output reg [47:0] word = 48'd0
);
    wire [7:0] ir = core[7:0];
    wire [2:0] mcycle = core[10:8], tstate = core[13:11], mcycles = core[16:14], tstates = core[19:17];
    wire [1:0] iset = core[21:20];
    wire [4:0] rtr = core[26:22];
    wire [3:0] alu = core[30:27];
    wire       save_alu = core[31], intcycle = core[35];
    wire [15:0] a = core[51:36];
    wire [7:0] genie_di = core[59:52], d_out = core[67:60];
    wire       cart_sel = core[75], bootloader_n = core[74], irq_n = core[73], m1_n = core[72],
               mreq_n = core[71], iorq_n = core[70], rd_n = core[69], wr_n = core[68];

    wire game = loaded && !core_reset && !idle;

    // ---- replay check ----
    reg [15:0] crc = 16'hFFFF;
    reg [21:0] replayed = 22'd0;
    reg        seq_err = 1'b0, crc_done = 1'b0, reset_again = 1'b0, was_running = 1'b0;
    function [15:0] crc_byte;
        input [15:0] c;
        input [7:0] d;
        integer i;
        reg [15:0] r;
        begin
            r = c ^ {d, 8'd0};
            for (i = 0; i < 8; i = i + 1) r = r[15] ? ({r[14:0], 1'b0} ^ 16'h1021) : {r[14:0], 1'b0};
            crc_byte = r;
        end
    endfunction

    // ---- main trace ----
    reg [35:0] mm [0:1023];
    reg [143:0] snap = 144'd0;
    reg [8:0]  n = 9'd0;                  // entries written (256 = full)
    reg [2:0]  wpart = 3'd4;              // 0..3 writing, 4 idle
    reg [4:0]  delta = 5'd0;
    reg        mm_we = 1'b0;
    reg [9:0]  mm_wa = 10'd0;
    reg [35:0] mm_wd = 36'd0;
    always @(posedge clk) if (mm_we) mm[mm_wa] <= mm_wd;

    // ---- pclk trace ----
    reg [35:0] pa [0:511];
    reg [35:0] pb [0:511];
    reg [8:0]  pw = 9'd0, trig = 9'd0;
    reg        ptrig = 1'b0, pfrozen = 1'b0, stall_prev = 1'b0;
    reg [8:0]  post = 9'd0;
    reg [19:0] ptime = 20'd0;
    reg        p_we = 1'b0;
    reg [8:0]  p_wa = 9'd0;
    reg [35:0] pa_wd = 36'd0, pb_wd = 36'd0;
    always @(posedge clk) if (p_we) begin pa[p_wa] <= pa_wd; pb[p_wa] <= pb_wd; end

    always @(posedge clk) begin
        mm_we <= 1'b0;
        p_we <= 1'b0;
        if (clear) begin
            crc <= 16'hFFFF; replayed <= 22'd0; seq_err <= 1'b0; crc_done <= 1'b0; reset_again <= 1'b0; was_running <= 1'b0;
            n <= 9'd0; wpart <= 3'd4; delta <= 5'd0;
            pw <= 9'd0; ptrig <= 1'b0; pfrozen <= 1'b0; stall_prev <= 1'b0; ptime <= 20'd0;
        end else begin
            if (dl_en) begin
                crc <= crc_byte(crc, dl_d);
                replayed <= replayed + 1'b1;
                if (dl_a[21:0] != replayed) seq_err <= 1'b1;
            end
            if (loaded) crc_done <= 1'b1;
            if (game) was_running <= 1'b1;
            if (was_running && core_reset) reset_again <= 1'b1;

            // main trace: the snapshot is taken at the enable, then written in 4 pclk (enables are >= 9 pclk apart)
            if (delta != 5'd31) delta <= delta + 1'b1;
            if (game && ce_cpu) begin
                delta <= 5'd0;
                if (!n[8]) begin
                    snap <= {rom_a, rom_stall, stall_r, rom_ok, bootloader_n, cart_sel, ram_we, iff1, irq_n, intcycle, delta,
                             sp, acc, f, iorq_n, rd_n, wr_n, halt,
                             rom_do, d_out, genie_di, ir, tstate, mreq_n,
                             pc, a, mcycle, m1_n};
                    wpart <= 3'd0;
                end
            end else if (wpart != 3'd4) begin
                mm_we <= 1'b1; mm_wa <= {n[7:0], wpart[1:0]};
                mm_wd <= wpart == 3'd0 ? snap[35:0] : wpart == 3'd1 ? snap[71:36] : wpart == 3'd2 ? snap[107:72] : snap[143:108];
                wpart <= wpart + 1'b1;
                if (wpart == 3'd3) n <= n + 1'b1;
            end

            // pclk trace
            if (game && !pfrozen) begin
                p_we <= 1'b1; p_wa <= pw;
                pa_wd <= {a, rom_do, genie_di, mcycle, ce_cpu};
                pb_wd <= {ir, tstate, run, rom_stall, stall_r, rom_ok, q_cnt, mcycles, tstates, iset, rtr, alu, save_alu, m1_n};
                pw <= pw + 1'b1;
                stall_prev <= rom_stall;
                if (ptime != 20'hFFFFF) ptime <= ptime + 1'b1;
                if (!ptrig && ((stall_prev && !rom_stall) || ptime == 20'hFFFFF)) begin
                    ptrig <= 1'b1; trig <= pw; post <= 9'd255;
                end else if (ptrig) begin
                    if (post == 9'd0) pfrozen <= 1'b1;
                    else post <= post - 1'b1;
                end
            end
        end
    end

    // ---- read-out: one word per read of the bench word ----
    reg [2:0]  tog_s = 3'd0;
    reg [11:0] cur = 12'd0;
    reg [35:0] mm_q = 36'd0, pa_q = 36'd0, pb_q = 36'd0;
    wire [9:0] pcur = cur[9:0];           // position inside the pclk region (cur - 1024)
    always @(posedge clk) begin
        tog_s <= {tog_s[1:0], rd_tog};
        if (clear) cur <= 12'd0;
        else if (tog_s[2] != tog_s[1]) cur <= (cur == 12'd2055) ? 12'd0 : cur + 1'b1;
        mm_q <= mm[cur[9:0]];
        pa_q <= pa[pcur[9:1]];
        pb_q <= pb[pcur[9:1]];
    end
    wire [18:0] status = {6'd0, pfrozen, ptrig, n[8], reset_again, fill_err, seq_err, crc_done,
                          alt, stall_r, core_reset, idle, bad, loaded};
    reg [42:0] summary;
    always @(*) begin
        case (cur[2:0])
        3'd0: summary = {crc, fills, status};
        3'd1: summary = {replayed, n, trig, 3'd0};
        3'd2: summary = {vdp_regs, cram0, cram16, 3'd0};
        3'd3: summary = osd[44:2];
        3'd4: summary = {pc, sp, 11'd0};
        default: summary = 43'd0;
        endcase
    end
    always @(posedge clk) begin
        if (cur[11]) word <= {2'b10, cur[2:0], summary};
        else if (cur[10]) word <= {2'b01, pcur[9:1], pcur[0], pcur[0] ? pb_q : pa_q};
        else word <= {2'b00, cur[9:2], cur[1:0], mm_q};
    end
endmodule
