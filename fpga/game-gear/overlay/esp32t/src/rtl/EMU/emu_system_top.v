// emu_system_top.v

module emu_system_top
(
    input               hclk,
    input               pclk,
    input               fclk, 
    input               xclk,
    input               reset_n,
    input               boot_alt,           // cartflash1: alternate (netboot) boot image, link cmd 10
    input               POWER_GOOD,
    
    input               customPaletteEna,
    input [63:0]        paletteBGIn,
    input [63:0]        paletteOBJ0In,
    input [63:0]        paletteOBJ1In,
    input [2:0]         gbc_color_temp,
    input               paletteOff,
    output              gbc_mode,
    output [63:0]       gpd,
    
    input               BTN_NODIAGONAL,
    input               BTN_A,
    input               BTN_B,
    input               BTN_DPAD_DOWN,
    input               BTN_DPAD_LEFT,
    input               BTN_DPAD_RIGHT,
    input               BTN_DPAD_UP,
    input               BTN_MENU,
    input               BTN_SEL,
    input               BTN_START,
    input               MENU_CLOSED,
    output              boot_rom_enabled,
    output  [15:0]      CART_A,
    output              CART_CLK,
    output              CART_CS,
    input   [7:0]       CART_D_IN,
    output  [7:0]       CART_D_OUT,
    output              CART_RD,
    output              CART_WR,
    output              CART_DATA_DIR_E,

    input               IR_RX,
    output              IR_LED,

    inout               LINK_CLK,
    output              LINK_CLK_DIR_LV,
    input               LINK_IN,
    output              LINK_OUT,
    // CWL1 network link (to link_net_cdc)
    input        net_en,
    input        net_master_reply_pend,
    input  [7:0] net_master_reply_byte,
    input        net_master_reply_drop,
    input        net_barrier_en,
    output       net_master_reply_ack,
    input        net_slave_xfer_pend,
    input  [7:0] net_slave_xfer_byte,
    output       net_slave_xfer_ack,
    input        net_standing_en,
    input        net_slave_withdraw_pend,
    output       net_slave_withdraw_ack,
    output       net_ev_valid,
    output [2:0] net_ev_type,
    output [7:0] net_ev_byte,
    // network window (to net_window in top.v)
    output [15:0] nw_cpu_addr,
    output  [7:0] nw_cpu_do,
    output        nw_cpu_rd_n,
    output        nw_cpu_wr_stb,
    input         nw_sel,
    input   [7:0] nw_do,

    output [15:0]       left,
    output [15:0]       right,

    output lcd_on_int,
    output lcd_off_overwrite,
    
    input               LCD_INIT_DONE,
    output              gb_lcd_clkena,
    output [14:0]       gb_lcd_data,
    output [1:0]        gb_lcd_mode,
    output              gb_lcd_on,
    output              gb_lcd_vsync,

    input               latched_cart_rst_n,
    // Shiny Relay 1.1.0: the RAM cart (ramcart.v in top.v) serves the core's cart bus when rc_on
    input               rc_on,
    input               rc_hold,            // hold the core in reset (a game being loaded)
    input   [7:0]       rc_q,
    input               rc_stall,
    output              rc_lin,
    output              rc_lin_ram,         // CG-107: the access is to the save RAM
    output  [21:0]      rc_lin_a,
    output  [15:0]      rc_a,
    output              rc_rd,
    output              rc_wr,
    output  [7:0]       rc_di,
    output  reg [47:0]  gg_dbg = 48'd0,     // CG-117 diag: the state read by `rc dbg`
    input       [102:0] x_diag,             // gg14diag: ramcart's PSRAM-side counters (xclk, static)
    output  wire        o_emulator_reset    // core reset should be used to paint black screen

);


    parameter SRSIZE = 15;

    reg [SRSIZE-1:0] BTN_DPAD_DOWN_sr;
    reg [SRSIZE-1:0] BTN_DPAD_UP_sr;
    reg [SRSIZE-1:0] BTN_DPAD_LEFT_sr;
    reg [SRSIZE-1:0] BTN_DPAD_RIGHT_sr;

    reg [SRSIZE-1:0] BTN_START_sr;
    reg [SRSIZE-1:0] BTN_SEL_sr;
    reg [SRSIZE-1:0] BTN_B_sr;
    reg [SRSIZE-1:0] BTN_A_sr;

    reg BTN_DPAD_DOWN_filtered;
    reg BTN_DPAD_UP_filtered;
    reg BTN_DPAD_LEFT_filtered;
    reg BTN_DPAD_RIGHT_filtered;
    reg BTN_START_filtered;
    reg BTN_SEL_filtered;
    reg BTN_B_filtered;
    reg BTN_A_filtered;
    
    reg BTN_DPAD_DOWN_filtered_dir;
    reg BTN_DPAD_UP_filtered_dir;
    reg BTN_DPAD_LEFT_filtered_dir;
    reg BTN_DPAD_RIGHT_filtered_dir;

    always@(posedge pclk)
    begin
        BTN_DPAD_DOWN_sr <= {BTN_DPAD_DOWN_sr[SRSIZE-2:0], BTN_DPAD_DOWN&~BTN_MENU&MENU_CLOSED};
        BTN_DPAD_UP_sr <= {BTN_DPAD_UP_sr[SRSIZE-2:0], BTN_DPAD_UP&~BTN_MENU&MENU_CLOSED};
        BTN_DPAD_LEFT_sr <= {BTN_DPAD_LEFT_sr[SRSIZE-2:0], BTN_DPAD_LEFT&~BTN_MENU&MENU_CLOSED};
        BTN_DPAD_RIGHT_sr <= {BTN_DPAD_RIGHT_sr[SRSIZE-2:0], BTN_DPAD_RIGHT&~BTN_MENU&MENU_CLOSED};
        BTN_START_sr <= {BTN_START_sr [SRSIZE-2:0], BTN_START&~BTN_MENU&MENU_CLOSED};
        BTN_SEL_sr <= {BTN_SEL_sr [SRSIZE-2:0], BTN_SEL&~BTN_MENU&MENU_CLOSED};
        BTN_A_sr <= {BTN_A_sr [SRSIZE-2:0], BTN_A&~BTN_MENU&MENU_CLOSED};
        BTN_B_sr <= {BTN_B_sr [SRSIZE-2:0], BTN_B&~BTN_MENU&MENU_CLOSED};

        BTN_DPAD_DOWN_filtered <= &BTN_DPAD_DOWN_sr[SRSIZE-1:1];
        BTN_DPAD_UP_filtered <= &BTN_DPAD_UP_sr[SRSIZE-1:1];
        BTN_DPAD_LEFT_filtered <= &BTN_DPAD_LEFT_sr[SRSIZE-1:1];
        BTN_DPAD_RIGHT_filtered <= &BTN_DPAD_RIGHT_sr[SRSIZE-1:1];
        BTN_START_filtered <= &BTN_START_sr[SRSIZE-1:1];
        BTN_SEL_filtered <= &BTN_SEL_sr[SRSIZE-1:1];
        BTN_A_filtered <= &BTN_A_sr[SRSIZE-1:1];
        BTN_B_filtered <= &BTN_B_sr[SRSIZE-1:1];
        
        if (BTN_NODIAGONAL) begin
            BTN_DPAD_DOWN_filtered_dir  <= BTN_DPAD_DOWN_filtered  & ~BTN_DPAD_UP_filtered & ~BTN_DPAD_LEFT_filtered_dir & ~BTN_DPAD_RIGHT_filtered_dir;
            BTN_DPAD_UP_filtered_dir    <= BTN_DPAD_UP_filtered    & ~BTN_DPAD_DOWN_filtered & ~BTN_DPAD_LEFT_filtered_dir & ~BTN_DPAD_RIGHT_filtered_dir;
            BTN_DPAD_LEFT_filtered_dir  <= BTN_DPAD_LEFT_filtered  & ~BTN_DPAD_RIGHT_filtered & ~BTN_DPAD_UP_filtered_dir & ~BTN_DPAD_DOWN_filtered_dir;
            BTN_DPAD_RIGHT_filtered_dir <= BTN_DPAD_RIGHT_filtered & ~BTN_DPAD_LEFT_filtered & ~BTN_DPAD_UP_filtered_dir & ~BTN_DPAD_DOWN_filtered_dir;
        end else begin
            BTN_DPAD_DOWN_filtered_dir  <= BTN_DPAD_DOWN_filtered  & ~BTN_DPAD_UP_filtered;
            BTN_DPAD_UP_filtered_dir    <= BTN_DPAD_UP_filtered    & ~BTN_DPAD_DOWN_filtered;
            BTN_DPAD_LEFT_filtered_dir  <= BTN_DPAD_LEFT_filtered  & ~BTN_DPAD_RIGHT_filtered;
            BTN_DPAD_RIGHT_filtered_dir <= BTN_DPAD_RIGHT_filtered & ~BTN_DPAD_LEFT_filtered;
        end
        
    end

    // Hard reset: exactly the conditions that reset the Game Boy core in the stock design. top.v turns
    // the menu off while o_emulator_reset is high (appear_off), so "no game loaded" must not be a reset:
    // the core then runs idle on a ROM of zeros (black screen), like the stock design with an empty slot.
    // reset_n comes from another clock domain (memrst, xclk2) and the RAM cart flags from hclk:
    // everything is synchronized into pclk before it reaches the core's clock enables.
    wire hard_raw = !reset_n || !POWER_GOOD || !LCD_INIT_DONE || rc_hold ||
                    (!latched_cart_rst_n && !rc_on) ||
                    (MENU_CLOSED && BTN_MENU && BTN_A && BTN_B && BTN_START && BTN_SEL);
    reg [1:0] hard_s = 2'b11, on_s = 2'b00;
    always @(posedge pclk) begin
        hard_s <= {hard_s[0], hard_raw};
        on_s <= {on_s[0], rc_on};
    end
    wire hard = hard_s[1];
    wire reload = hard || !on_s[1];       // the loader starts over
    wire loaded, bad;
    wire idle = !on_s[1] || bad;          // no Game Gear game: RAM cart off, or an image without the marker
    wire [23:0] rom_a;
    wire rom_rd;
    wire [24:0] dl_a;
    wire [7:0] dl_d;
    wire dl_en;
    wire [7:0] rom_do;
    wire dbg_settle;
    wire [95:0] hdr_rd;
    wire [7:0] stall_n;
    wire [3:0] bad_idx;
    wire [7:0] bad_val;
    // CG-107: the cartridge RAM (32 KiB, the save) is the RAM cart's save area in PSRAM, reached through the same
    // cache port as the ROM: the firmware loads /SAVES/<game>.sav there and copies it back after writes.
    wire nvram_rd, nvram_we, ld_rd;
    wire [14:0] nvram_a;
    wire [7:0] nvram_d;
    wire [21:0] ld_a;
    wire nv_sel = loaded && (nvram_rd || nvram_we);
    assign rc_lin_ram = nv_sel;
    assign rc_lin_a = nv_sel ? {7'd0, nvram_a} : ld_a;
    assign rc_rd = ld_rd || (loaded && nvram_rd);
    gg_rom_loader loader (
        .settle(dbg_settle), .bad_idx(bad_idx), .bad_val(bad_val), .hdr_rd(hdr_rd), .stall_n(stall_n),
        .clk(pclk), .reset(reload), .q(rc_q), .stall(rc_stall),
        .rom_a(rom_a), .rom_rd(rom_rd && !nvram_rd), .address(ld_a), .rd(ld_rd),
        .loaded(loaded), .bad(bad), .rom_do(rom_do), .dl_a(dl_a), .dl_d(dl_d), .dl_en(dl_en)
    );
    assign rc_lin = 1'b1;
    assign rc_a = 16'd0;
    assign rc_wr = loaded && nvram_we;
    assign rc_di = nvram_d;
    wire core_reset = hard || (!idle && !loaded);   // hard reset, or a game's ROM being replayed
    // The stall is registered: it rises with the address (after a CPU clock enable), and the next clock
    // enable is at least 3 pclk later, so one pclk of delay never lets an enable through.
    reg stall_r = 1'b1;
    always @(posedge pclk) stall_r <= rc_stall;
    // CG-117 diag (build gg12diag): `rc dbg` prints a = gg_dbg[15:0], fills = gg_dbg[31:16], flags = gg_dbg[47:32].
    //   a     = the low 16 bits of the last ROM address the core read (after the mapper)
    //   fills = {first wrong marker byte [3:0] (15 = none), ROM reads counted [3:0], bad ? the byte read : ROM address [23:16]}
    //   flags = {loaded, bad, idle, hard, core_reset, rc_on, rc_hold, reset_n, POWER_GOOD, LCD_INIT_DONE,
    //            latched_cart_rst_n, MENU_CLOSED, reloads seen [3:0]}
    reg [23:0] dbg_a = 24'd0;
    reg [3:0] dbg_resets = 4'd0, dbg_reads = 4'd0, dbg_reloads = 4'd0;
    reg dbg_cr = 1'b1, dbg_rl = 1'b1, dbg_rd = 1'b0;
    // gg14diag: the settle wait while SELECT is held (raw button, two flops)
    reg [1:0] dbg_sel = 2'b00;
    always @(posedge pclk) dbg_sel <= {dbg_sel[0], BTN_SEL};
    assign dbg_settle = dbg_sel[1];
    // `rc dbg` (gg14diag): flags = {loaded, bad, idle, hard, core_reset, rc_on, rc_hold, reset_n, POWER_GOOD,
    //   LCD_INIT_DONE, settle, MENU_CLOSED, 0, page[2:0]}; {fills, a} = 32 bits of the page (it changes every 125 ms):
    //   0..2 header bytes 3..0 / 7..4 / 11..8 as the loader read them    3 {w8, w9}   4 {w10, w11} of the fill
    //   5 {hdr_n[8:0], 3'd0, wrong[3:0], hdr_fills[3:0], 4'd0, stall_n[7:0]}
    //   6 {stray[3:0], 1'b0, max[8:0], min[8:0], 5'd0, reloads[3:0]}      7 {reads[3:0], 4'd0, ROM address[23:0]}
    reg [24:0] dbg_t = 25'd0;
    reg [31:0] dbg_page;
    always @(*) case (dbg_t[24:22])
        3'd0: dbg_page = hdr_rd[31:0];
        3'd1: dbg_page = hdr_rd[63:32];
        3'd2: dbg_page = hdr_rd[95:64];
        3'd3: dbg_page = x_diag[63:32];
        3'd4: dbg_page = x_diag[31:0];
        3'd5: dbg_page = {x_diag[80:72], 3'd0, x_diag[71:68], x_diag[67:64], 4'd0, stall_n};
        3'd6: dbg_page = {x_diag[102:99], 1'b0, x_diag[98:90], x_diag[89:81], 5'd0, dbg_reloads};
        default: dbg_page = {dbg_reads, 4'd0, dbg_a};
    endcase
    always @(posedge pclk) begin
        dbg_cr <= core_reset; dbg_rl <= reload; dbg_rd <= rom_rd;
        if (core_reset && !dbg_cr) dbg_resets <= dbg_resets + 1'b1;
        if (reload && !dbg_rl) dbg_reloads <= dbg_reloads + 1'b1;
        if (rom_rd && !dbg_rd) begin dbg_reads <= dbg_reads + 1'b1; dbg_a <= rom_a; end
        dbg_t <= dbg_t + 1'b1;
        gg_dbg <= {loaded, bad, idle, hard, core_reset, rc_on, rc_hold, reset_n, POWER_GOOD, LCD_INIT_DONE,
                   dbg_settle, MENU_CLOSED, 1'b0, dbg_t[24:22], dbg_page};
    end
    wire run = !core_reset;
    wire hold = !idle && stall_r;        // CG-106: acts on CPU ticks only (gg_ce_gen)
    wire ce_cpu, ce_vdp, ce_pix, ce_sp, ce_snd;
    gg_ce_gen enables (.clk(pclk), .reset(core_reset), .run(run), .hold(hold),
        .ce_cpu(ce_cpu), .ce_vdp(ce_vdp), .ce_pix(ce_pix), .ce_sp(ce_sp), .ce_snd(ce_snd));
    wire [8:0] vx, vy;
    wire [7:0] vcounter_cpu;
    wire [11:0] color;
    wire hb, vb, mask_column, smode_M1, smode_M2, smode_M3, smode_M4;
    wire [15:0] audio_l, audio_r;
    wire [13:0] ram_a;
    wire [7:0] ram_d, ram_q;
    wire [7:0] nvram_q = rc_q;
    wire ram_we;
    // New-data, synchronous block RAM, matching the core's existing RAM wrappers.
    // Game Gear: 8 KiB of work RAM, 8 KiB of (volatile) cartridge RAM. Block RAM is scarce on the GW5A-25.
    gg_ram #(.AW(13)) work_ram (
        .clk(pclk), .a(ram_a[12:0]), .d(ram_d), .we(ram_we && run), .q(ram_q));
    video timing (
        .clk(pclk), .ce_pix(ce_pix), .pal(1'b0), .border(1'b0), .ggres(1'b1),
        .mask_column(mask_column), .cut_mask(1'b0),
        .smode_M1(smode_M1), .smode_M2(smode_M2), .smode_M3(smode_M3), .smode_M4(smode_M4),
        .video_state_in(22'd3), .video_state_set(core_reset), .video_state_out(),
        .x(vx), .y(vy), .vcounter_cpu(vcounter_cpu), .hsync(), .vsync(), .hblank(hb), .vblank(vb));
    gg_video_out video_out (
        .pclk(pclk), .hclk(hclk), .reset(core_reset), .ce_pix(ce_pix),
        .color(idle ? 12'd0 : color), .hb(hb), .vb(vb),
        .lcd_data(gb_lcd_data), .lcd_clkena(gb_lcd_clkena),
        .lcd_mode(gb_lcd_mode), .lcd_vsync(gb_lcd_vsync));
    assign gb_lcd_on = 1'b1;
    assign lcd_on_int = 1'b1;
    assign lcd_off_overwrite = 1'b0;
    reg reset_h;
    always @(posedge hclk) reset_h <= core_reset;
    assign o_emulator_reset = reset_h;
    reg [15:0] snd_l, snd_r;
    always @(posedge hclk) begin
        snd_l <= (core_reset || idle) ? 16'd0 : audio_l;
        snd_r <= (core_reset || idle) ? 16'd0 : audio_r;
    end
    audio_filter u_audio_filter (.reset(~reset_n), .clk(hclk),
        .core_l(snd_l), .core_r(snd_r), .filter_l(left), .filter_r(right));
    assign CART_A = 16'd0;
    assign CART_CLK = 1'b1;
    assign CART_CS = 1'b1;
    assign CART_RD = 1'b1;
    assign CART_WR = 1'b1;
    assign CART_D_OUT = 8'd0;
    assign CART_DATA_DIR_E = 1'b1;
    assign LINK_CLK = 1'bz;
    assign LINK_CLK_DIR_LV = 1'b0;
    assign LINK_OUT = 1'b1;
    assign IR_LED = 1'b0;
    assign boot_rom_enabled = 1'b0;
    assign gbc_mode = 1'b1;
    assign gpd = 64'd0;
    assign net_master_reply_ack = 1'b0;
    assign net_slave_xfer_ack = 1'b0;
    assign net_slave_withdraw_ack = 1'b0;
    assign net_ev_valid = 1'b0;
    assign net_ev_type = 3'd0;
    assign net_ev_byte = 8'd0;
    assign nw_cpu_addr = 16'd0;
    assign nw_cpu_do = 8'd0;
    assign nw_cpu_rd_n = 1'b1;
    assign nw_cpu_wr_stb = 1'b0;
    // GHDL simulation has these same generics baked into system_sim.v.
`ifdef VERILATOR
    system sms (
`else
    system #(.SYSTEM_E(0), .MAX_SPPL(7), .EXTERNAL_BIOS(0)) sms (
`endif
        .clk_sys(pclk),
        .ce_cpu(ce_cpu),
        .ce_snd(ce_snd),
        .ce_vdp(ce_vdp),
        .ce_pix(ce_pix),
        .ce_sp(ce_sp),
        .turbo(1'b0),
        .gg(1'b1),
        .ggres(1'b1),
        .systeme(1'b0),
        .bios_en(1'b0),
        .ext_bios_sel(1'b0),
        .ext_bios_loaded(1'b0),
        .gg_bios_en(1'b0),
        .ext_gg_bios_loaded(1'b0),
        .GG_BIOSWEN(1'b0),
        .GG_EN(1'b1),
        .GG_CODE(129'd0),
        .GG_RESET(core_reset),
        .GG_AVAIL(),
        .gg_link_en(1'b0),
        .gg_link_in(7'd0),
        .gg_link_out(),
        .RESET_n(!core_reset),
        .rom_rd(rom_rd),
        .nvram_rd(nvram_rd),
        .rom_a(rom_a),
        .rom_do(idle ? 8'h00 : rom_do),
        .j1_up(!BTN_DPAD_UP_filtered_dir),
        .j1_down(!BTN_DPAD_DOWN_filtered_dir),
        .j1_left(!BTN_DPAD_LEFT_filtered_dir),
        .j1_right(!BTN_DPAD_RIGHT_filtered_dir),
        .j1_tl(!BTN_B_filtered),
        .j1_tr(!BTN_A_filtered),
        .j1_th(1'b1),
        .j1_start(!BTN_START_filtered),
        .j1_coin(1'b1),
        .j1_a3(!BTN_START_filtered),
        .j2_up(1'b1),
        .j2_down(1'b1),
        .j2_left(1'b1),
        .j2_right(1'b1),
        .j2_tl(1'b1),
        .j2_tr(1'b1),
        .j2_th(1'b1),
        .j2_start(1'b1),
        .j2_coin(1'b1),
        .j2_a3(1'b1),
        .pause(!BTN_START_filtered),
        .soft_reset(1'b0),
        .E0Type(2'd0),
        .E1Use(1'b0),
        .E2Use(1'b0),
        .E0(8'hff),
        .F2(8'hff),
        .F3(8'hff),
        .has_paddle(1'b0),
        .has_pedal(1'b0),
        .paddle(8'd0),
        .paddle2(8'd0),
        .pedal(8'd0),
        .sc3000_en(1'b0),
        .sc_multicart_en(1'b0),
        .sc_megacart_en(1'b0),
        .sc_cart_ram(2'd0),
        .sk1100_en(1'b0),
        .sk1100_row_sel(),
        .sk1100_row_data(12'hfff),
        .j1_tr_out(),
        .j1_th_out(),
        .j2_tr_out(),
        .j2_th_out(),
        .x(vx),
        .y(vy),
        .vcounter_cpu(vcounter_cpu),
        .color(color),
        .palettemode(1'b0),
        .mask_column(mask_column),
        .black_column(1'b0),
        .smode_M1(smode_M1),
        .smode_M2(smode_M2),
        .smode_M3(smode_M3),
        .smode_M4(smode_M4),
        .ysj_quirk(1'b0),
        .pal(1'b0),
        .region(1'b0),
        .mapper_lock(1'b0),
        .mapper_codies_force(1'b0),
        .mapper_dahjee_a_force(1'b0),
        .mapper_linear_force(1'b0),
        .mapper_zemina_force(1'b0),
        .mapper_evolution_force(1'b0),
        .evolution_gg_active(),
        .evolution_active(),
        .mapper_eeprom_out(),
        .vdp_enables(2'd0),
        .psg_enables(2'd0),
        .audioL(audio_l),
        .audioR(audio_r),
        .fm_ena(1'b0),
        .dbr(loaded),
        .sp64(1'b0),
        .ram_a(ram_a),
        .ram_d(ram_d),
        .ram_we(ram_we),
        .ram_q(ram_q),
        .nvram_a(nvram_a),
        .nvram_d(nvram_d),
        .nvram_we(nvram_we),
        .nvram_q(nvram_q),
        .encrypt(2'd0),
        .key_a(),
        .key_d(8'd0),
        .ROMCL(pclk),
        .ROMAD(dl_a),
        .ROMDT(dl_d),
        .ROMEN(dl_en),
        .BIOSWEN(1'b0),
        .z80_reg_out(),
        .z80_dir(230'd0),
        .z80_set(1'b0),
        .vdp_regs_out(),
        .vdp_regs_in(128'd0),
        .vdp_regs_set(1'b0),
        .vdp_cram_out(),
        .ss_cram_wr(1'b0),
        .ss_cram_A(5'd0),
        .ss_cram_D(12'd0),
        .ss_vram_en(1'b0),
        .ss_vram_A(15'd0),
        .ss_vram_D(),
        .ss_vram_WE(1'b0),
        .ss_vram_WA(15'd0),
        .ss_vram_WD(8'd0),
        .psg_out(),
        .psg_in(56'd0),
        .psg_set(1'b0),
        .psg_ext_out(),
        .psg2_ext_out(),
        .psg_ext_in(96'd0),
        .psg2_ext_in(96'd0),
        .psg_div_out(),
        .psg_div_in(4'd0),
        .audio_control_out(),
        .audio_control_in(11'd0),
        .audio_quiescent(),
        .audio_phase_out(),
        .audio_phase_in(1'b0),
        .audio_phase_set(1'b0),
        .audio_ext_set(1'b0),
        .mapper_out(),
        .mapper_in(64'd0),
        .mapper_set(1'b0),
        .evolution_ss_out(),
        .evolution_ss_in(96'd0),
        .evolution_ss_set(1'b0),
        .eeprom_ss_out(),
        .eeprom_ss_in(64'd0),
        .eeprom_ss_set(1'b0),
        .z80_m1_n(),
        .z80_mreq_n(),
        .z80_iset(),
        .vdp2_regs_out(),
        .vdp2_regs_in(128'd0),
        .vdp2_regs_set(1'b0),
        .vdp2_cram_out(),
        .ss_cram2_wr(1'b0),
        .ss_cram2_A(5'd0),
        .ss_cram2_D(12'd0),
        .ss_vram2_en(1'b0),
        .ss_vram2_A(15'd0),
        .ss_vram2_D(),
        .ss_vram2_WE(1'b0),
        .ss_vram2_WA(15'd0),
        .ss_vram2_WD(8'd0),
        .psg2_out(),
        .psg2_in(56'd0),
        .psg2_set(1'b0),
        .io_state_out(),
        .io_state_in(32'd0),
        .io_state_set(1'b0),
        .se_pause(1'b0),
        .ss_freeze(1'b0)
    );
endmodule
