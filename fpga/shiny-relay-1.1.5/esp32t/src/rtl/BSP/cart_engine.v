// cart_engine.v: a small cartridge-bus engine for writing ModRetro cart flash (lane cartflash1,
// DRAFT, test bitstreams only).
//
// Why: the GB-timed cartridge cycles of the core (cart.v) never register as flash command cycles
// on a ModRetro cart (unit 1, SST39VF1682, 2026-10-10), while the same cart is programmed from the
// same slot with static-pin cycles (FlashGBX via Fred Emmott's GPL Chromatic stack: ID BF C9).
// This engine drives the cart bus with that kind of cycle. No code is taken from any GPL tool;
// the timing follows the SST39VF168x datasheet (DS25040A) minima and FlashGBX's published
// ModRetro write recipe as facts: address + data out ~200 ns before /WE falls, /WE low ~400 ns,
// data held ~200 ns after /WE rises, CLK low and /RD high throughout, a static bus in between.
//
// Owner of the bus: the GB core, unless a SESSION is on. A session needs `allow` (link cmd 9 from
// the ESP32, only while a cart job is armed) and is ended by a GB reset or by `allow` falling.
// Registers (only while `allow`; otherwise FF60-FF67 read FF and ignore writes, as on stock):
//   FF60 CTRL  W: b0 SESSION, b1 AUTOINC (RDATA steps ADDR), b2 CLR_ERR
//              R: [7:4] = 0xA, b0 session, b1 busy, b2 timeout error, b3 verify error
//   FF61 AL / FF62 AH  address (cart bus address). A write starts a prefetch read of it.
//   FF63 RDATA R: the byte last read at ADDR; the end of this read steps ADDR (AUTOINC) and
//              starts the next prefetch, so `ldh a,[c]` loops stream bytes (and re-reading
//              without AUTOINC samples the same address again: DQ6 toggle polling). The next byte
//              is ready ~10 hclk after the read: reads 2 normal-speed M-cycles apart always work.
//   FF64 WDATA W: one write cycle of the value at ADDR (mapper registers, cart RAM, raw cmds);
//                 with AUTOINC and ADDR in A000-BFFF, ADDR steps after the write (RAM fills)
//   FF65 PROG  W: byte program: AA@AAA 55@555 A0@AAA value@ADDR, poll DQ6 until stable (timeout
//              -> timeout error), read back and compare (verify error), then ADDR += 1. A value
//              of FF only steps ADDR (an erased cell already reads FF; software checks FF bytes).
//   FF66 CMD3  W: AA@AAA 55@555 value@AAA (ID entry 90, CFI 98, ID/CFI exit F0)
//   FF67 ERASE W: AA@AAA 55@555 80@AAA AA@AAA 55@555 value@ADDR (50 sector, 30 block; 10 chip at
//              AAA), then poll DQ6 until stable (erase timeout -> timeout error)
// The unlock addresses are cart bus addresses 0x0AAA / 0x0555 (bank-0 area, so an MBC5-like mapper
// puts flash A20-A14 at 0). Writes are never issued while the chip is busy (an SST39VF168x takes
// B0 at any address as Erase-Suspend). One op may be queued while the engine is busy (e.g. with
// the prefetch an RDATA read starts); software waits for STAT.busy = 0 before the next one, and a
// second op while one is queued is dropped with the timeout error set.
//
// cartspeed1 (18.26): the bus timing is held in registers (defaults = the parameters below, the
// timing every earlier image used; the express port can change them for one session, and they
// return to the defaults whenever `allow` falls or the GB resets). A second client, the express
// stream (cart_xs.v), issues raw bus cycles through x_bus_*: it gets the bus only between ops,
// and while it holds x_hold no op of the CPU side (nor a prefetch) starts, so nothing ever lands
// between the cycles of one of its flash commands. x_session keeps the bus with the engine
// without a CPU write to CTRL (the express stream drives the cart while the writer waits).
module cart_engine #(
    parameter integer W_SETUP  = 4,      // hclk (59.6 ns): address + data before /WR falls
    parameter integer W_PULSE  = 7,      // /WR low
    parameter integer W_HOLD   = 4,      // data held after /WR rises
    parameter integer R_ACCESS = 7,      // /RD low before sampling
    parameter integer GAP      = 4,      // static bus after every cycle (>= tIDA 150 ns)
    parameter integer PROG_TMO = 8400,     // ~500 us (SST tBP max 10 us; other parts ~200 us)
    parameter integer ERASE_TMO = 67000000 // ~4 s (SST tSE/tBE max 25 ms; 64 KiB sectors of other parts: seconds)
) (
    input             clk,
    input             reset,             // GB reset domain: ends a session
    input             allow,
    // CPU bus (the network window's export from gb.v)
    input      [15:0] cpu_addr,
    input       [7:0] cpu_do,
    input             cpu_rd_n,
    input             cpu_wr_stb,
    output            sel,
    output      [7:0] do_,
    // cartridge bus, to the mux in front of cartridge_interface
    output            active,
    output reg [15:0] e_a = 16'h0000,
    output            e_clk,
    output            e_cs,
    output reg        e_rd = 1'b1,
    output reg        e_wr = 1'b1,
    output reg  [7:0] e_d_out = 8'h00,
    output reg        e_dir_e = 1'b1,     // 1 = cart drives / input, 0 = engine drives data
    input       [7:0] cart_d_in,
    // express stream port (cart_xs.v), hclk
    input             x_session,         // the express stream owns a session (needs `allow`)
    input             x_hold,            // an express op is running: no CPU-side op / prefetch starts
    input             x_bus_req,         // one bus cycle: level, held until x_bus_done
    input             x_bus_wr,          // 1 write (x_bus_a, x_bus_d), 0 read (x_bus_a)
    input      [15:0] x_bus_a,
    input       [7:0] x_bus_d,
    output reg        x_bus_done = 1'b0, // one clk: the cycle ended; x_bus_q = the byte read
    output      [7:0] x_bus_q,
    input             x_tcfg,            // one clk: take x_t_* as the bus timing
    input       [4:0] x_t_setup, x_t_pulse, x_t_hold, x_t_access, x_t_gap,
    // status (for tests / debug)
    output            busy,
    output reg [15:0] n_cycles = 16'd0
);
    // ---------------------------------------------------------------- registers
    wire       page   = allow & (cpu_addr[15:3] == 13'h1FEC);   // FF60-FF67
    wire [2:0] r      = cpu_addr[2:0];
    assign     sel    = page;
    reg        session = 1'b0, autoinc = 1'b0, err_tmo = 1'b0, err_vfy = 1'b0;
    reg [15:0] addr = 16'h0000;
    reg  [7:0] rdata = 8'hFF;
    assign     active = (session | x_session) & allow;
    // bus timing, in hclk (defaults: the parameters); 0 is taken as 1
    reg  [4:0] t_setup = W_SETUP, t_pulse = W_PULSE, t_hold = W_HOLD, t_access = R_ACCESS, t_gap = GAP;
    wire [6:0] tw_fall = {2'b0, t_setup};
    wire [6:0] tw_rise = tw_fall + t_pulse;
    wire [6:0] tw_rel  = tw_rise + t_hold;
    wire [6:0] tw_end  = tw_rel + t_gap;
    wire [6:0] tr_samp = {2'b0, t_access};
    wire [6:0] tr_end  = tr_samp + t_gap;
    reg        x_cyc = 1'b0;               // the cycle running belongs to the express port
    assign     e_clk  = 1'b0;
    // cart RAM cycles (A000-BFFF) drive /CS. FRAM latches its address on the falling /CS edge,
    // so /CS falls one hclk AFTER the address (cs_on, with /RD or the data drive), stays low
    // past /WR rising, and is high between cycles (the precharge time).
    reg        cyc_ram = 1'b0, cs_on = 1'b0;
    assign     e_cs   = ~(active & cyc_ram & cs_on);

    // ---------------------------------------------------------------- op sequencer
    localparam OP_IDLE = 3'd0, OP_PREF = 3'd1, OP_WR = 3'd2, OP_PROG = 3'd3, OP_CMD3 = 3'd4, OP_ERASE = 3'd5;
    reg  [2:0] op = OP_IDLE;
    reg  [3:0] step = 4'd0;
    reg  [7:0] val = 8'h00;              // the op's data byte
    reg        pend_pref = 1'b0;
    reg        q_valid = 1'b0;               // one queued op
    reg  [2:0] q_op = 3'd0;
    reg  [7:0] q_val = 8'h00;
    // bus cycle unit
    localparam B_IDLE = 2'd0, B_WR = 2'd1, B_RD = 2'd2;
    reg  [1:0] bus = B_IDLE;
    reg  [6:0] bt = 7'd0;                // time in the cycle
    reg        bdone = 1'b0;
    reg  [7:0] bq = 8'h00;               // byte read
    // polling
    reg        polling = 1'b0, poll_second = 1'b0;
    reg  [7:0] poll_v1 = 8'h00;
    reg [26:0] tmo = 27'd0;
    assign     busy = (op != OP_IDLE) | (bus != B_IDLE) | q_valid;
    assign     x_bus_q = bq;

    assign do_ = (r == 3'd0) ? {4'hA, err_vfy, err_tmo, busy, session} :
                 (r == 3'd1) ? addr[7:0] :
                 (r == 3'd2) ? addr[15:8] :
                 (r == 3'd3) ? rdata : 8'hFF;

    wire wr_hit = page & cpu_wr_stb;
    wire rd_act = page & (r == 3'd3) & ~cpu_rd_n;
    reg  rd_q = 1'b0;

    // one bus cycle: write (a, d) or read (a)
    task start_wr(input [15:0] a, input [7:0] d);
        begin
            bus <= B_WR; bt <= 7'd0; e_a <= a; e_d_out <= d; cyc_ram <= (a[15:13] == 3'b101);
        end
    endtask
    task start_rd(input [15:0] a);
        begin
            bus <= B_RD; bt <= 7'd0; e_a <= a; cyc_ram <= (a[15:13] == 3'b101);
        end
    endtask

    always @(posedge clk) begin
        bdone <= 1'b0;
        rd_q  <= rd_act;
        // ---------------- bus cycle unit
        x_bus_done <= 1'b0;
        case (bus)
        B_WR: begin
            bt <= bt + 7'd1;
            if (bt == 7'd0) begin e_dir_e <= 1'b0; cs_on <= 1'b1; end            // drive data; /CS (RAM) after the address
            if (bt == tw_fall) e_wr <= 1'b0;
            if (bt == tw_rise) e_wr <= 1'b1;
            if (bt == tw_rel) e_dir_e <= 1'b1;                                   // release data
            if (bt == tw_rel) cs_on <= 1'b0;                                     // /CS high for the whole gap
            if (bt == tw_end) begin bus <= B_IDLE; bdone <= ~x_cyc; x_bus_done <= x_cyc; x_cyc <= 1'b0; cyc_ram <= 1'b0; n_cycles <= n_cycles + 16'd1; end
        end
        B_RD: begin
            bt <= bt + 7'd1;
            if (bt == 7'd0) begin e_rd <= 1'b0; cs_on <= 1'b1; end
            if (bt == tr_samp) begin bq <= cart_d_in; e_rd <= 1'b1; cs_on <= 1'b0; end
            if (bt == tr_end) begin bus <= B_IDLE; bdone <= ~x_cyc; x_bus_done <= x_cyc; x_cyc <= 1'b0; cyc_ram <= 1'b0; n_cycles <= n_cycles + 16'd1; end
        end
        default: ;
        endcase

        // ---------------- express port: one cycle at a time, only between ops
        if (active && x_bus_req && !x_bus_done && bus == B_IDLE && op == OP_IDLE && !bdone && !x_cyc) begin
            x_cyc <= 1'b1;
            if (x_bus_wr) start_wr(x_bus_a, x_bus_d); else start_rd(x_bus_a);
        end
        if (x_tcfg) begin
            t_setup  <= (x_t_setup  == 5'd0) ? 5'd1 : x_t_setup;
            t_pulse  <= (x_t_pulse  == 5'd0) ? 5'd1 : x_t_pulse;
            t_hold   <= (x_t_hold   == 5'd0) ? 5'd1 : x_t_hold;
            t_access <= (x_t_access == 5'd0) ? 5'd1 : x_t_access;
            t_gap    <= (x_t_gap    == 5'd0) ? 5'd1 : x_t_gap;
        end

        // ---------------- register writes (ops start only when idle)
        if (wr_hit) begin
            case (r)
            3'd0: begin
                session <= cpu_do[0];
                autoinc <= cpu_do[1];
                if (cpu_do[2]) begin err_tmo <= 1'b0; err_vfy <= 1'b0; end
            end
            3'd1: begin addr[7:0]  <= cpu_do; pend_pref <= 1'b1; end
            3'd2: begin addr[15:8] <= cpu_do; pend_pref <= 1'b1; end
            3'd4, 3'd5, 3'd6, 3'd7: begin
                // queued (one deep): it starts as soon as the engine is idle, so an op right after
                // an RDATA read (which starts a prefetch) is never lost
                if (!active || q_valid) err_tmo <= 1'b1;                       // a second op while one waits
                else begin
                    q_valid <= 1'b1; q_val <= cpu_do;
                    q_op <= (r == 3'd4) ? OP_WR : (r == 3'd5) ? OP_PROG : (r == 3'd6) ? OP_CMD3 : OP_ERASE;
                end
            end
            default: ;
            endcase
        end
        // the end of an RDATA read: step and prefetch the next
        if (rd_q & ~rd_act) begin
            if (autoinc) addr <= addr + 16'd1;
            pend_pref <= 1'b1;
        end

        // ---------------- op sequencer (one step per finished bus cycle)
        if (active) begin
            if (op == OP_IDLE && bus == B_IDLE && !x_hold && !x_cyc && q_valid && !wr_hit) begin
                q_valid <= 1'b0; val <= q_val; step <= 4'd0; pend_pref <= 1'b0;
                if (q_op == OP_PROG && q_val == 8'hFF) begin addr <= addr + 16'd1; pend_pref <= 1'b1; end   // FF: erased already
                else op <= q_op;
            end else if (op == OP_IDLE && bus == B_IDLE && !x_hold && !x_cyc && pend_pref && !q_valid && !wr_hit && !(rd_q & ~rd_act)) begin
                pend_pref <= 1'b0; op <= OP_PREF; step <= 4'd0;
            end
            case (op)
            OP_PREF: begin
                if (step == 4'd0 && bus == B_IDLE) begin start_rd(addr); step <= 4'd1; end
                else if (step == 4'd1 && bus == B_RD && bt == tr_samp + 7'd1) rdata <= bq;   // as soon as sampled
                else if (step == 4'd1 && bdone) op <= OP_IDLE;
            end
            OP_WR: begin
                if (step == 4'd0 && bus == B_IDLE) begin start_wr(addr, val); step <= 4'd1; end
                else if (step == 4'd1 && bdone) begin
                    op <= OP_IDLE; pend_pref <= 1'b1;
                    if (autoinc && addr[15:13] == 3'b101) addr <= addr + 16'd1;   // a cart RAM write steps ADDR (RAM fill loops)
                end
            end
            OP_CMD3, OP_PROG, OP_ERASE: begin
                if (!polling && bus == B_IDLE && !bdone) begin
                    // the command cycles, one per step
                    case (step)
                    4'd0: start_wr(16'h0AAA, 8'hAA);
                    4'd1: start_wr(16'h0555, 8'h55);
                    4'd2: start_wr(16'h0AAA, op == OP_CMD3 ? val : op == OP_PROG ? 8'hA0 : 8'h80);
                    4'd3: if (op == OP_PROG) start_wr(addr, val); else if (op == OP_ERASE) start_wr(16'h0AAA, 8'hAA);
                    4'd4: if (op == OP_ERASE) start_wr(16'h0555, 8'h55);
                    4'd5: if (op == OP_ERASE) start_wr(val == 8'h10 ? 16'h0AAA : addr, val);
                    default: ;
                    endcase
                end
                if (!polling && bdone) begin
                    step <= step + 4'd1;
                    if ((op == OP_CMD3 && step == 4'd2) ) begin op <= OP_IDLE; pend_pref <= 1'b1; end
                    if ((op == OP_PROG && step == 4'd3) || (op == OP_ERASE && step == 4'd5)) begin
                        polling <= 1'b1; poll_second <= 1'b0; tmo <= 27'd0;
                    end
                end
                // DQ6 toggle polling at ADDR (program: the byte; erase: the sector)
                if (polling) begin
                    tmo <= tmo + 27'd1;
                    if (bus == B_IDLE && !bdone) start_rd(addr);
                    if (bdone) begin
                        if (!poll_second) begin poll_v1 <= bq; poll_second <= 1'b1; end
                        else if (((poll_v1 ^ bq) & 8'h40) == 8'h00) begin               // stopped toggling: done
                            polling <= 1'b0;
                            if (op == OP_PROG) begin
                                if (bq != val) err_vfy <= 1'b1;
                                addr <= addr + 16'd1;
                            end
                            op <= OP_IDLE; pend_pref <= 1'b1;
                        end else begin
                            poll_v1 <= bq;
                            if (tmo > (op == OP_PROG ? PROG_TMO : ERASE_TMO)) begin
                                err_tmo <= 1'b1; polling <= 1'b0; op <= OP_IDLE;
                            end
                        end
                    end
                end
            end
            default: ;
            endcase
        end

        if (reset || !allow) begin
            session <= 1'b0; op <= OP_IDLE; polling <= 1'b0; pend_pref <= 1'b0; q_valid <= 1'b0;
            bus <= B_IDLE; e_rd <= 1'b1; e_wr <= 1'b1; e_dir_e <= 1'b1; cyc_ram <= 1'b0; cs_on <= 1'b0;
            x_cyc <= 1'b0; x_bus_done <= 1'b0;
            t_setup <= W_SETUP; t_pulse <= W_PULSE; t_hold <= W_HOLD; t_access <= R_ACCESS; t_gap <= GAP;
        end
    end
endmodule
