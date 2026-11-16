// Diagnostic probes for the Game Gear core on the Chromatic (build gg4diag, review CG-111).
//
// Read with the ESP32 console command `rc dbg` (FPGA bench word of 48 bits: a = [15:0], fills = [31:16],
// flags = [47:32]). The word shows one of eight slots, about one second each:
//   word[47:46] = page, word[45] = subpage, word[44:0] = payload, frozen while the slot is shown.
//
//   0A  {crc16[15:0], fills[7:0], status[20:0]}      CRC-16/CCITT of the bytes replayed to the core at load
//   0B  {replayed[21:0], fetches[18:0], 4'd0}        bytes replayed; opcode fetches so far (saturates)
//   1A  {h64[15:0], h1024[15:0], fetches[12:0]}      CPU signatures after 64 and 1024 fetches
//   1B  {h16384[15:0], last_pc[15:0], last_op[7:0], halt, v262144, v16384, v1024, v64}
//   2A  {vdp_regs[15:0], cram0[11:0], cram16[11:0], 5'd0}
//   2B  {ram_addr[12:0], ram_expected[7:0], ram_got[7:0], ram_compares[11:0], 3'd0, ram_err}
//   3A  {h262144[15:0], sp[15:0], pc[15:3]}          live stack pointer and program counter
//   3B  osd[44:0]                                    from vid_system_top: {first[15:0], last[15:0], count[7:0], seen, 4'd0}
//
// status = {7'd0, alt, consumed_invalid, cpu_seen, reset_again, ram_err, fill_err, seq_err, crc_done,
//           ce_seen, stall_r, core_reset, idle, bad, loaded}
//
// A CPU signature is h <= rotl(h, 1) ^ pc ^ {op, op} at each opcode fetch, where op is the byte the Z80
// latched (the ROM/RAM data at the clock enable that ends T2 of an M1 cycle) and pc the program counter
// after the fetch. The whole machine stops on a ROM stall, so the values do not depend on PSRAM timing and
// can be compared with the simulation.
// The probes are cleared when a game load starts (hold), not by any other reset.
module gg_probe #(parameter SLOT_BITS = 25) (
    input             clk,                // pclk
    input             clear,              // a game load starts (RAM cart hold)
    input             core_reset, loaded, bad, idle, stall_r, alt,
    input             rom_stall,          // the RAM cart's raw stall
    input             ce_cpu, m1_n,
    input      [7:0]  cpu_di,             // what the Z80 reads from the ROM
    input      [15:0] pc, sp,
    input             halt,
    input             dl_en,
    input      [24:0] dl_a,
    input      [7:0]  dl_d,
    input      [15:0] vdp_regs,
    input      [11:0] cram0, cram16,
    input      [12:0] ram_a,
    input      [7:0]  ram_d, ram_q,
    input             ram_we,
    input             fill_err,           // from ramcart (xclk), sticky
    input      [7:0]  fills,              // from ramcart, wraps
    input      [44:0] osd,                // from vid_system_top (hclk), latched there
    output reg [47:0] word = 48'd0
);
    // ---- replay: CRC-16/CCITT (poly 1021, init FFFF, MSB first), count, order ----
    reg [15:0] crc = 16'hFFFF;
    reg [21:0] replayed = 22'd0;
    reg        seq_err = 1'b0, crc_done = 1'b0;
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

    // ---- CPU fetches ----
    reg        m1_prev = 1'b1, stall_ce = 1'b0;
    reg [7:0]  di_ce = 8'd0;
    reg [18:0] fetches = 19'd0;
    reg [15:0] h = 16'd0, h64 = 16'd0, h1024 = 16'd0, h16384 = 16'd0, h262144 = 16'd0;
    reg        v64 = 1'b0, v1024 = 1'b0, v16384 = 1'b0, v262144 = 1'b0;
    reg [15:0] last_pc = 16'd0;
    reg [7:0]  last_op = 8'd0;
    reg        consumed_invalid = 1'b0, cpu_seen = 1'b0, ce_seen = 1'b0, reset_again = 1'b0, was_loaded = 1'b0;
    wire [15:0] h_next = {h[14:0], h[15]} ^ pc ^ {di_ce, di_ce};

    // ---- work RAM read-back ----
    reg [12:0] w_a = 13'd0;
    reg [7:0]  w_d = 8'd0;
    reg        w_armed = 1'b0;
    reg [1:0]  w_age = 2'd0;
    reg [11:0] ram_compares = 12'd0;
    reg        ram_err = 1'b0;
    reg [12:0] e_a = 13'd0;
    reg [7:0]  e_exp = 8'd0, e_got = 8'd0;

    always @(posedge clk) begin
        if (clear) begin
            crc <= 16'hFFFF; replayed <= 22'd0; seq_err <= 1'b0; crc_done <= 1'b0;
            m1_prev <= 1'b1; fetches <= 19'd0; h <= 16'd0;
            v64 <= 1'b0; v1024 <= 1'b0; v16384 <= 1'b0; v262144 <= 1'b0;
            consumed_invalid <= 1'b0; cpu_seen <= 1'b0; ce_seen <= 1'b0; reset_again <= 1'b0; was_loaded <= 1'b0;
            w_armed <= 1'b0; ram_compares <= 12'd0; ram_err <= 1'b0;
        end else begin
            if (dl_en) begin
                crc <= crc_byte(crc, dl_d);
                replayed <= replayed + 1'b1;
                if (dl_a[21:0] != replayed) seq_err <= 1'b1;
            end
            if (loaded) begin crc_done <= 1'b1; was_loaded <= 1'b1; end
            if (was_loaded && core_reset) reset_again <= 1'b1;

            if (ce_cpu && !core_reset && !idle) begin
                ce_seen <= 1'b1;
                m1_prev <= m1_n;
                di_ce <= cpu_di;
                stall_ce <= rom_stall;
                if (!m1_prev && m1_n) begin          // the previous enable ended T2 of an M1 cycle
                    cpu_seen <= 1'b1;
                    h <= h_next;
                    last_pc <= pc; last_op <= di_ce;
                    if (stall_ce) consumed_invalid <= 1'b1;
                    if (!fetches[18]) fetches <= fetches + 1'b1;
                    if (fetches == 19'd63)     begin h64 <= h_next; v64 <= 1'b1; end
                    if (fetches == 19'd1023)   begin h1024 <= h_next; v1024 <= 1'b1; end
                    if (fetches == 19'd16383)  begin h16384 <= h_next; v16384 <= 1'b1; end
                    if (fetches == 19'd262143) begin h262144 <= h_next; v262144 <= 1'b1; end
                end
            end
            if (core_reset) m1_prev <= 1'b1;

            // a location written by the core must read back the same while its address stays selected
            if (ram_we && !core_reset && !idle) begin
                w_a <= ram_a; w_d <= ram_d; w_armed <= 1'b1; w_age <= 2'd0;
            end else if (w_armed && ram_a == w_a) begin
                if (w_age != 2'd2) w_age <= w_age + 1'b1;
                else begin
                    w_armed <= 1'b0;
                    if (ram_compares != 12'hFFF) ram_compares <= ram_compares + 1'b1;
                    if (ram_q != w_d && !ram_err) begin ram_err <= 1'b1; e_a <= w_a; e_exp <= w_d; e_got <= ram_q; end
                end
            end else w_age <= 2'd0;
        end
    end

    wire [20:0] status = {7'd0, alt, consumed_invalid, cpu_seen, reset_again, ram_err, fill_err, seq_err, crc_done,
                          ce_seen, stall_r, core_reset, idle, bad, loaded};

    // ---- one slot at a time, frozen while it is shown ----
    reg [SLOT_BITS+2:0] t = 0;
    wire [2:0] slot = t[SLOT_BITS+2:SLOT_BITS];
    reg [44:0] payload;
    always @(*) begin
        case (slot)
        3'd0: payload = {crc, fills, status};
        3'd1: payload = {replayed, fetches, 4'd0};
        3'd2: payload = {h64, h1024, fetches[12:0]};
        3'd3: payload = {h16384, last_pc, last_op, halt, v262144, v16384, v1024, v64};
        3'd4: payload = {vdp_regs, cram0, cram16, 5'd0};
        3'd5: payload = {e_a, e_exp, e_got, ram_compares, 3'd0, ram_err};
        3'd6: payload = {h262144, sp, pc[15:3]};
        default: payload = osd;
        endcase
    end
    always @(posedge clk) begin
        t <= t + 1'b1;
        if (t[SLOT_BITS-1:0] == 0) word <= {slot, payload};
    end
endmodule
