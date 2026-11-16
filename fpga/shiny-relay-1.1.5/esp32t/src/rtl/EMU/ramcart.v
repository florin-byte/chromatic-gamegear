// ramcart.v: a game held in PSRAM, played as the cartridge (Shiny Relay 1.1.0, FPGA 18.30).
//
// With `h_on` the core's cartridge reads and writes are served here instead of by the cart slot
// (emu_system_top keeps the slot's bus quiet and the cart powered off). The ESP32 loads the game
// into PSRAM over QSPI (mm_burst_write) and the save RAM next to it, then turns this on through
// cart_xs (UART op RCFG) with the core held in reset.
//
// PSRAM map (byte addresses; the frame buffers own the first 128 KiB):
//   RAM_BASE 0x040000  save RAM, up to 128 KiB (16 banks of 8 KiB)
//   ROM_BASE 0x100000  the game, up to 4 MiB (256 banks of 16 KiB)
//
// The mappers (bank registers as MiSTer's mappers/mbc*.v): 0 none, 1 MBC1, 2 MBC2, 3 MBC3 (+ the
// clock), 5 MBC5. The byte at an address comes from a 4 KiB direct-mapped cache (128 lines of 32
// bytes) in block RAM; a miss stops the core's clock enables (`stall`, speedcontrol.vhd) until
// the line is in. The cache is looked up in the clock after the address appears, which the core
// never needs sooner (its next clock enable is two hclk later at the earliest), so a hit never
// stops the core. Save RAM writes go to the cached line and on to PSRAM (one 16-bit word: the
// PSRAM has no byte mask), allocating the line first; the core waits for each (writes are rare).
//
// PSRAM side (xclk): one request at a time from hclk (toggle handshake): FILL a line, WRITE a
// word, WREAD up to 1 KiB into the read window (QSPI reads it back: qspi_slave.v), INVAL the tags.
module ramcart #(
    parameter [22:0] RAM_BASE = 23'h040000,
    parameter [22:0] ROM_BASE = 23'h100000
) (
    input              hclk,
    // ---------------- configuration (hclk, cart_xs RCFG) ----------------
    input              h_on,
    input       [2:0]  h_mbc,
    input       [7:0]  h_rom_mask,         // ROM banks - 1
    input       [3:0]  h_ram_mask,         // RAM banks - 1
    input              h_ram_has,
    input              h_rtc_has,
    input              h_inval_tog,        // a toggle: drop every cached line (after a new load)
    output             h_busy,             // an invalidate / request is still running
    // ---------------- the core's cart bus (hclk) ----------------
    input      [15:0]  a,
    input              rd,
    input              wr,
    input       [7:0]  di,
    output reg  [7:0]  q,
    output             stall,
    output reg         h_dirty = 1'b0,     // a save RAM write since the last clear
    output     [47:0]  h_dbg,              // bench: {flags, fills, the core's address}
    input              h_dirty_clr,        // pulse
    // ---------------- the clock (MBC3) ----------------
    input              h_rtc_set,          // pulse: load the live clock from h_rtc_in
    input      [39:0]  h_rtc_in,           // {DH, DL, H, M, S}
    output     [39:0]  h_rtc_out,
    // ---------------- read window (hclk request, QSPI clock read) ----------------
    input              h_wread_tog,        // a toggle: read h_wread_len bytes from h_wread_addr
    input      [22:0]  h_wread_addr,       // even
    input      [10:0]  h_wread_len,        // bytes, even, 2..1024, within one 1 KiB row
    input              h_wread_off,        // 18.31: 1 = into the window from word 1 (word 0 is cart_xs's tag)
    output reg         h_wread_done_tog = 1'b0,
    // ---------------- 18.31: words from cart_xs FCOPY (boot flash -> PSRAM), only while the RAM cart is off ----------------
    output             h_cw_ready,
    input              h_cw_go,            // pulse (while h_cw_ready): write h_cw_data at h_cw_addr
    input      [22:0]  h_cw_addr,
    input      [15:0]  h_cw_data,
    input              qclk,
    input       [8:0]  q_win_addr,         // 16-bit words
    output reg [15:0]  q_win_data = 16'd0,
    // ---------------- PSRAM port (xclk) ----------------
    input              xclk,
    output reg         x_req = 1'b0,
    output reg         x_rnw = 1'b1,
    output reg [22:0]  x_addr = 23'd0,
    output reg [10:0]  x_len = 11'd0,
    output reg [15:0]  x_din = 16'd0,
    input              x_write_next,
    input              x_done,
    input              x_dout_valid,
    input      [15:0]  x_dout
);
    // =====================================================================
    // mapper registers (hclk)
    // =====================================================================
    reg        ram_en = 1'b0;
    reg  [8:0] rom_bank = 9'd1;          // MBC5: 9 bits; others fewer
    reg  [1:0] mbc1_hi = 2'd0;
    reg        mbc1_mode = 1'b0;
    reg  [3:0] ram_bank = 4'd0;          // MBC3: 08-0C select the clock
    reg        rtc_sel = 1'b0;
    reg        latch_last = 1'b0;

    // the clock: live and latched {DH, DL, H, M, S}; DH = {carry, halt, 5'd0, day8}
    reg  [5:0] rs = 6'd0, rm = 6'd0;
    reg  [4:0] rh = 5'd0;
    reg  [8:0] rday = 9'd0;
    reg        rhalt = 1'b0, rcarry = 1'b0;
    reg [39:0] rlat = 40'd0;
    reg [23:0] rsub = 24'd0;             // hclk = 16.777216 MHz = 2^24 Hz
    assign h_rtc_out = {rcarry, rhalt, 5'd0, rday[8], rday[7:0], 3'd0, rh, 2'd0, rm, 2'd0, rs};

    wire        rom_sel  = ~a[15];
    wire        cram_sel = (a[15:13] == 3'b101);

    // the bank of a ROM address (a[14] = the switchable half)
    reg  [8:0] rbank;
    always @(*) begin
        case (h_mbc)
        3'd1:    rbank = a[14] ? {2'd0, mbc1_hi, (rom_bank[4:0] == 5'd0) ? 5'd1 : rom_bank[4:0]}
                               : (mbc1_mode ? {2'd0, mbc1_hi, 5'd0} : 9'd0);
        3'd2:    rbank = a[14] ? {5'd0, (rom_bank[3:0] == 4'd0) ? 4'd1 : rom_bank[3:0]} : 9'd0;
        3'd3:    rbank = a[14] ? {1'b0, (rom_bank[7:0] == 8'd0) ? 8'd1 : rom_bank[7:0]} : 9'd0;
        3'd5:    rbank = a[14] ? rom_bank : 9'd0;
        default: rbank = a[14] ? 9'd1 : 9'd0;
        endcase
    end
    wire [3:0] cbank = (h_mbc == 3'd1) ? (mbc1_mode ? {2'd0, mbc1_hi} : 4'd0) :
                       (h_mbc == 3'd2) ? 4'd0 : ram_bank;
    wire [21:0] rom_off  = {rbank[7:0] & h_rom_mask, a[13:0]};
    wire [16:0] cram_off = (h_mbc == 3'd2) ? {8'd0, a[8:0]} : {cbank & h_ram_mask, a[12:0]};
    wire [22:0] pa = rom_sel ? (ROM_BASE + {1'b0, rom_off}) : (RAM_BASE + {6'd0, cram_off});

    // what a cart RAM read means
    wire cram_open = h_ram_has & (ram_en | (h_mbc == 3'd0));   // a plain ROM+RAM cart: always open
    wire rtc_read  = (h_mbc == 3'd3) & h_rtc_has & rtc_sel;

    // =====================================================================
    // the cache (hclk side)
    // =====================================================================
    reg  [22:0] pa_w = 23'd0;            // the save RAM write in progress
    reg   [7:0] wd = 8'd0;
    reg   [2:0] ws = 3'd0;               // write state
    localparam W_IDLE = 3'd0, W_LOOK = 3'd1, W_CHECK = 3'd2, W_MERGE = 3'd3, W_POST = 3'd4, W_WAIT = 3'd5, W_FILL = 3'd6;

    wire [22:0] la = (ws != W_IDLE) ? pa_w : pa;     // the address being looked up
    wire [15:0] dq;                                  // data word (registered read)
    wire [11:0] tq;                                  // {valid, tag}
    reg  [22:0] la_r = 23'd0;
    reg         d_we = 1'b0;
    reg  [15:0] d_wdata = 16'd0;
    always @(posedge hclk) la_r <= la;
    wire tag_ok = tq[11] & (tq[10:0] == la_r[22:12]);
    wire hit = tag_ok & (la_r == la);

    // requests to the PSRAM side: one at a time
    localparam K_FILL = 2'd0, K_WRITE = 2'd1, K_WREAD = 2'd2, K_INVAL = 2'd3;
    reg        h_req_tog = 1'b0;
    reg  [1:0] h_req_kind = K_FILL;
    reg [22:0] h_req_addr = 23'd0;
    reg [10:0] h_req_len = 11'd0;
    reg [15:0] h_req_data = 16'd0;
    reg        h_req_busy = 1'b0;
    reg        x_done_tog = 1'b0;
    reg  [2:0] h_done_sync = 3'b000;
    reg        h_done_seen = 1'b0;
    wire       h_done_edge = (h_done_sync[2] != h_done_seen);
    reg  [2:0] h_inval_sync = 3'b000, h_wread_sync = 3'b000;
    reg        h_inval_seen = 1'b0, h_wread_seen = 1'b0, h_init = 1'b0;
    reg        inval_pend = 1'b0, wread_pend = 1'b0, wread_run = 1'b0, fill_run = 1'b0;
    assign h_busy = inval_pend | h_req_busy | wread_pend;
    assign h_cw_ready = !h_on && !h_req_busy && ws == W_IDLE && !inval_pend && !wread_pend;
    reg [15:0] n_fill = 16'd0;

    wire want = h_on & rd & (rom_sel | (cram_sel & cram_open & ~rtc_read));
    assign stall = h_on & ((want & ~hit) | (ws != W_IDLE) | inval_pend);
    assign h_dbg = {rd, wr, want, hit, tag_ok, stall, h_req_busy, inval_pend, 1'b0, ws, h_mbc, 1'b0, n_fill, a};

    // the write side, one register stage in: the core's write strobe reaches here through its whole
    // combinational CPU (the tool times cart data -> IR -> address -> cart_wr), and a write commits
    // at the end of its cycle anyway
    reg        wr_q = 1'b0, wr_r = 1'b0;
    reg [15:0] a_q = 16'd0, wa = 16'd0;
    reg  [7:0] di_q = 8'd0, wdi = 8'd0;

    task req(input [1:0] k, input [22:0] ad, input [10:0] ln, input [15:0] dt);
        begin h_req_kind <= k; h_req_addr <= ad; h_req_len <= ln; h_req_data <= dt; h_req_tog <= ~h_req_tog; h_req_busy <= 1'b1; end
    endtask

    always @(posedge hclk) begin
        h_done_sync  <= {h_done_sync[1:0], x_done_tog};
        h_inval_sync <= {h_inval_sync[1:0], h_inval_tog};
        h_wread_sync <= {h_wread_sync[1:0], h_wread_tog};
        d_we <= 1'b0;
        if (h_dirty_clr) h_dirty <= 1'b0;

        // ---- the clock ticks (MBC3) ----
        if (h_rtc_set) begin
            {rcarry, rhalt} <= {h_rtc_in[39], h_rtc_in[38]}; rday <= {h_rtc_in[32], h_rtc_in[31:24]};
            rh <= h_rtc_in[20:16]; rm <= h_rtc_in[13:8]; rs <= h_rtc_in[5:0]; rsub <= 24'd0;
        end else if (!rhalt) begin
            rsub <= rsub + 24'd1;
            if (rsub == 24'hFFFFFF) begin
                if (rs != 6'd59) rs <= rs + 6'd1;
                else begin
                    rs <= 6'd0;
                    if (rm != 6'd59) rm <= rm + 6'd1;
                    else begin
                        rm <= 6'd0;
                        if (rh != 5'd23) rh <= rh + 5'd1;
                        else begin
                            rh <= 5'd0;
                            if (rday == 9'd511) begin rday <= 9'd0; rcarry <= 1'b1; end
                            else rday <= rday + 9'd1;
                        end
                    end
                end
            end
        end

        if (!h_init) begin
            h_init <= 1'b1; h_done_seen <= h_done_sync[2]; h_inval_seen <= h_inval_sync[2]; h_wread_seen <= h_wread_sync[2];
        end else begin
            if (h_inval_sync[2] != h_inval_seen) begin h_inval_seen <= h_inval_sync[2]; inval_pend <= 1'b1; end
            if (h_wread_sync[2] != h_wread_seen) begin h_wread_seen <= h_wread_sync[2]; wread_pend <= 1'b1; end
            if (h_done_edge) begin
                h_done_seen <= h_done_sync[2]; h_req_busy <= 1'b0; fill_run <= 1'b0;
                if (wread_run) begin wread_run <= 1'b0; h_wread_done_tog <= ~h_wread_done_tog; end
                if (h_req_kind == K_INVAL) inval_pend <= 1'b0;
                if (h_req_kind == K_FILL) n_fill <= n_fill + 16'd1;
            end

            // ---- the core's writes: taken at the end of the write cycle ----
            wr_q <= wr; a_q <= a; di_q <= di;
            wr_r <= wr_q;
            if (wr_q) begin wa <= a_q; wdi <= di_q; end
            if (h_on && wr_r && !wr_q) begin
                if (!wa[15]) begin
                    case (h_mbc)
                    3'd1: case (wa[14:13])
                          2'd0: ram_en <= (wdi[3:0] == 4'hA);
                          2'd1: rom_bank[4:0] <= wdi[4:0];
                          2'd2: mbc1_hi <= wdi[1:0];
                          2'd3: mbc1_mode <= wdi[0];
                          endcase
                    3'd2: if (!wa[14]) begin
                              if (wa[8]) rom_bank[3:0] <= wdi[3:0];
                              else ram_en <= (wdi[3:0] == 4'hA);
                          end
                    3'd3: case (wa[14:13])
                          2'd0: ram_en <= (wdi[3:0] == 4'hA);
                          2'd1: rom_bank[7:0] <= wdi[7:0];
                          2'd2: begin rtc_sel <= wdi[3]; ram_bank <= {2'd0, wdi[1:0]}; end
                          2'd3: begin
                              latch_last <= wdi[0];
                              if (!latch_last && wdi[0]) rlat <= h_rtc_out;
                          end
                          endcase
                    3'd5: case (wa[14:12])
                          3'd0, 3'd1: ram_en <= (wdi[3:0] == 4'hA);
                          3'd2: rom_bank[7:0] <= wdi;
                          3'd3: rom_bank[8] <= wdi[0];
                          3'd4, 3'd5: ram_bank <= wdi[3:0];
                          default: ;
                          endcase
                    default: ;
                    endcase
                end else if (wa[15:13] == 3'b101 && ram_en && h_ram_has) begin
                    if ((h_mbc == 3'd3) && h_rtc_has && rtc_sel) begin
                        case (ram_bank[2:0])
                        3'd0: begin rs <= wdi[5:0]; rsub <= 24'd0; end
                        3'd1: rm <= wdi[5:0];
                        3'd2: rh <= wdi[4:0];
                        3'd3: rday[7:0] <= wdi;
                        3'd4: begin rday[8] <= wdi[0]; rhalt <= wdi[6]; rcarry <= wdi[7]; end
                        default: ;
                        endcase
                    end else begin
                        // the save RAM byte: its PSRAM address with the banks as they are now
                        pa_w <= RAM_BASE + {6'd0, (h_mbc == 3'd2) ? {8'd0, wa[8:0]} : {cbank & h_ram_mask, wa[12:0]}};
                        wd <= (h_mbc == 3'd2) ? {4'hF, wdi[3:0]} : wdi;
                        ws <= W_LOOK;
                        h_dirty <= 1'b1;
                    end
                end
            end

            // ---- a save RAM write: the line in, the byte merged, the word out ----
            case (ws)
            W_LOOK:  ws <= W_CHECK;                                         // la = pa_w now
            W_CHECK: if (la_r == pa_w) begin
                         if (tag_ok) ws <= W_MERGE;
                         else if (!h_req_busy) begin req(K_FILL, {pa_w[22:5], 5'd0}, 11'd32, 16'd0); fill_run <= 1'b1; ws <= W_FILL; end
                     end
            W_FILL:  if (!h_req_busy) ws <= W_LOOK;
            W_MERGE: begin
                         d_wdata <= pa_w[0] ? {wd, dq[7:0]} : {dq[15:8], wd};
                         d_we <= 1'b1;
                         ws <= W_POST;
                     end
            W_POST:  if (!h_req_busy) begin req(K_WRITE, {pa_w[22:1], 1'b0}, 11'd2, d_wdata); ws <= W_WAIT; end
            W_WAIT:  if (!h_req_busy) ws <= W_IDLE;
            default: ;
            endcase

            // ---- requests: a miss first, then an invalidate, then a window read ----
            if (!h_req_busy && ws == W_IDLE) begin
                if (inval_pend)
                    req(K_INVAL, 23'd0, 11'd0, 16'd0);
                else if (h_on && want && !hit && la_r == la)
                    begin req(K_FILL, {la[22:5], 5'd0}, 11'd32, 16'd0); fill_run <= 1'b1; end
                else if (wread_pend) begin
                    wread_pend <= 1'b0; wread_run <= 1'b1;
                    req(K_WREAD, h_wread_addr, h_wread_len, 16'd0);
                end
                else if (h_cw_go && !h_on)
                    req(K_WRITE, {h_cw_addr[22:1], 1'b0}, 11'd2, h_cw_data);
            end

            // leaving RAM cart mode: the mapper back to power-on
            if (!h_on) begin
                ram_en <= 1'b0; rom_bank <= 9'd1; mbc1_hi <= 2'd0; mbc1_mode <= 1'b0; ram_bank <= 4'd0;
                rtc_sel <= 1'b0; ws <= W_IDLE;
            end
        end
    end

    // ---- what the core reads: from the registered block RAM outputs, no clock of delay ----
    always @(*) begin
        if (!h_on || !rd) q = 8'hFF;
        else if (rom_sel) q = la_r[0] ? dq[15:8] : dq[7:0];
        else if (!cram_open) q = 8'hFF;
        else if (rtc_read) begin
            case (ram_bank[2:0])
            3'd0: q = {2'b00, rlat[5:0]};
            3'd1: q = {2'b00, rlat[13:8]};
            3'd2: q = {3'b000, rlat[20:16]};
            3'd3: q = rlat[31:24];
            3'd4: q = rlat[39:32];
            default: q = 8'hFF;
            endcase
        end
        else if (h_mbc == 3'd2) q = {4'hF, la_r[0] ? dq[11:8] : dq[3:0]};
        else q = la_r[0] ? dq[15:8] : dq[7:0];
    end

    // =====================================================================
    // block RAMs
    // =====================================================================
    // data: 2048 x 16, port A hclk read + write (save RAM merge), port B xclk write (fills)
    reg [15:0] dmem [0:2047];
    reg [15:0] dq_r = 16'd0;
    assign dq = dq_r;
    always @(posedge hclk) begin
        // normal mode (the GW5A's DPB has no read-before-write): no read in the write's clock
        if (d_we) dmem[la[11:1]] <= d_wdata;              // la = pa_w while a write runs
        else dq_r <= dmem[la[11:1]];
    end
    reg        xd_we = 1'b0;
    reg [10:0] xd_wa = 11'd0;
    reg [15:0] xd_wd = 16'd0;
    always @(posedge xclk) if (xd_we) dmem[xd_wa] <= xd_wd;

    // tags: 128 x 12, written by xclk, read by hclk
    reg [11:0] tmem [0:127];
    reg [11:0] tq_r = 12'd0;
    assign tq = tq_r;
    always @(posedge hclk) tq_r <= tmem[la[11:5]];
    reg        xt_we = 1'b0;
    reg  [6:0] xt_wa = 7'd0;
    reg [11:0] xt_wd = 12'd0;
    always @(posedge xclk) if (xt_we) tmem[xt_wa] <= xt_wd;

    // the read window: 512 x 16, written by xclk, read on the QSPI clock
    reg [15:0] wmem [0:511];
    reg        xw_we = 1'b0;
    reg  [8:0] xw_wa = 9'd0;
    reg [15:0] xw_wd = 16'd0;
    always @(posedge xclk) if (xw_we) wmem[xw_wa] <= xw_wd;
    always @(posedge qclk) q_win_data <= wmem[q_win_addr];

    // =====================================================================
    // PSRAM side (xclk)
    // =====================================================================
    reg  [2:0] x_req_sync = 3'b000;
    reg        x_req_seen = 1'b0, x_init = 1'b0;
    reg  [2:0] xs = 3'd0;
    localparam X_IDLE = 3'd0, X_RUN = 3'd1, X_TAG = 3'd2, X_INVAL = 3'd3, X_DONE = 3'd4;
    reg  [1:0] xk = 2'd0;
    reg [22:0] xa = 23'd0;
    reg  [8:0] xi = 9'd0;                 // words received
    always @(posedge xclk) begin
        x_req_sync <= {x_req_sync[1:0], h_req_tog};
        x_req <= 1'b0; xd_we <= 1'b0; xt_we <= 1'b0; xw_we <= 1'b0;
        if (!x_init) begin x_init <= 1'b1; x_req_seen <= x_req_sync[2]; end
        else case (xs)
        X_IDLE: if (x_req_sync[2] != x_req_seen) begin
                    // h_req_* are steady from their toggle until the done toggle returns
                    x_req_seen <= x_req_sync[2];
                    xk <= h_req_kind; xa <= h_req_addr; xi <= 9'd0;
                    if (h_req_kind == K_INVAL) begin xs <= X_INVAL; xt_wa <= 7'd0; end
                    else begin
                        x_req <= 1'b1; x_rnw <= (h_req_kind != K_WRITE);
                        x_addr <= h_req_addr; x_len <= h_req_len; x_din <= h_req_data;
                        xs <= X_RUN;
                    end
                end
        X_RUN:  begin
                    if (x_dout_valid) begin
                        xi <= xi + 9'd1;
                        if (xk == K_FILL) begin xd_we <= 1'b1; xd_wa <= {xa[11:5], xi[3:0]}; xd_wd <= x_dout; end
                        else begin xw_we <= 1'b1; xw_wa <= xi + {8'd0, h_wread_off}; xw_wd <= x_dout; end
                    end
                    if (x_done) xs <= (xk == K_FILL) ? X_TAG : X_DONE;
                end
        X_TAG:  begin xt_we <= 1'b1; xt_wa <= xa[11:5]; xt_wd <= {1'b1, xa[22:12]}; xs <= X_DONE; end
        X_INVAL: begin
                    xt_we <= 1'b1; xt_wd <= 12'd0;
                    if (xt_we) xt_wa <= xt_wa + 7'd1;
                    if (xt_we && xt_wa == 7'd127) xs <= X_DONE;
                end
        X_DONE: begin x_done_tog <= ~x_done_tog; xs <= X_IDLE; end
        default: xs <= X_IDLE;
        endcase
    end
endmodule
