// Simulation of the complete replacement and the unmodified RAM-cart cache/PSRAM protocol.
module tb_gg(input pclk, hclk, gclk, reset_n, rc_on, rc_hold, start, inval, btn_a, menu_closed,
 output [5:0] lcd_db, output lcd_de, lcd_hs, lcd_vs, output p_vs, output lcd_init_done,
 output gb_newline, output [22:0] gb_addr, output [47:0] panel_dbg,
 output valid, output [14:0] pixel, output [1:0] mode, output vsync, core_reset,
 output x_req, x_rnw, output [22:0] x_addr, output [10:0] x_len,
 output [15:0] x_din, input x_done, x_dout_valid, input [15:0] x_dout,
 output rom_wait, output running, output [14:0] buffered, output [47:0] gg_dbg);
 wire [102:0] x_diag;
 wire rc_lin, rc_lin_ram, rc_rd, rc_wr, rc_stall;
 wire [21:0] rc_lin_a;
 wire [15:0] rc_a;
 wire [7:0] rc_q, rc_di;
 assign rom_wait = emu.loaded && rc_stall;
 assign running = emu.loaded;
 assign buffered = emu.video_out.written - emu.video_out.consumed;
 emu_system_top emu (
 .gg_dbg(gg_dbg), .x_diag(x_diag),
 .hclk(hclk),
 .pclk(pclk),
 .fclk(pclk),
 .xclk(pclk),
 .reset_n(reset_n),
 .boot_alt(0),
 .POWER_GOOD(1'b1),
 .customPaletteEna(0),
 .paletteBGIn(0),
 .paletteOBJ0In(0),
 .paletteOBJ1In(0),
 .gbc_color_temp(0),
 .paletteOff(0),
 .gbc_mode(),
 .gpd(),
 .BTN_NODIAGONAL(0),
 .BTN_A(btn_a),
 .BTN_B(0),
 .BTN_DPAD_DOWN(0),
 .BTN_DPAD_LEFT(0),
 .BTN_DPAD_RIGHT(0),
 .BTN_DPAD_UP(0),
 .BTN_MENU(0),
 .BTN_SEL(0),
 .BTN_START(start),
 .MENU_CLOSED(menu_closed),
 .boot_rom_enabled(),
 .CART_A(),
 .CART_CLK(),
 .CART_CS(),
 .CART_D_IN(0),
 .CART_D_OUT(),
 .CART_RD(),
 .CART_WR(),
 .CART_DATA_DIR_E(),
 .IR_RX(0),
 .IR_LED(),
 .LINK_CLK(),
 .LINK_CLK_DIR_LV(),
 .LINK_IN(0),
 .LINK_OUT(),
 .net_en(0),
 .net_master_reply_pend(0),
 .net_master_reply_byte(0),
 .net_master_reply_drop(0),
 .net_barrier_en(0),
 .net_master_reply_ack(),
 .net_slave_xfer_pend(0),
 .net_slave_xfer_byte(0),
 .net_slave_xfer_ack(),
 .net_standing_en(0),
 .net_slave_withdraw_pend(0),
 .net_slave_withdraw_ack(),
 .net_ev_valid(),
 .net_ev_type(),
 .net_ev_byte(),
 .nw_cpu_addr(),
 .nw_cpu_do(),
 .nw_cpu_rd_n(),
 .nw_cpu_wr_stb(),
 .nw_sel(0),
 .nw_do(0),
 .left(),
 .right(),
 .lcd_on_int(),
 .lcd_off_overwrite(),
 .LCD_INIT_DONE(lcd_init_done),
 .gb_lcd_clkena(valid),
 .gb_lcd_data(pixel),
 .gb_lcd_mode(mode),
 .gb_lcd_on(),
 .gb_lcd_vsync(vsync),
 .latched_cart_rst_n(1'b1),
 .rc_on(rc_on),
 .rc_hold(rc_hold),
 .rc_q(rc_q),
 .rc_stall(rc_stall),
 .rc_lin(rc_lin), .rc_lin_ram(rc_lin_ram),
 .rc_lin_a(rc_lin_a),
 .rc_a(rc_a),
 .rc_rd(rc_rd),
 .rc_wr(rc_wr),
 .rc_di(rc_di),
 .o_emulator_reset(core_reset));
 // The stock display path: color correction, OSD mix, frame buffer addressing and the panel master.
 assign p_vs = vid.u_ST7785_panel_master.pGbVsync & ~vid.u_ST7785_panel_master.pGbVsync_r1;
 vid_system_top #(1) vid (
  .appear_off(core_reset), .gClk(gclk), .hClk(hclk), .pClk(pclk), .reset(!reset_n), .BTN_MENU(1'b1),
  .LCD_DB(lcd_db), .capture_valid(), .capture_frame_start(), .capture_line_end(), .capture_pixel(),
  .LCD_DOTCLK(), .LCD_ENABLE(lcd_de), .LCD_HSYNC(lcd_hs), .LCD_RESET(), .LCD_SPI_CSX(), .LCD_SPI_SCLK(),
  .LCD_SPI_SDA(), .LCD_TE(1'b0), .LCD_EN(1'b1), .LCD_GENLOCK(), .LCD_VSYNC(lcd_vs),
  .frameBlendEnable(1'b0), .colorCorrectionEnableLCD(1'b0), .colorCorrectionEnableUVC(1'b0), .voltageLow(1'b0),
  .lowBattDispMode(2'd0), .showTimer(1'b0), .runTimer(1'b0), .resetTimer(1'b0), .gSecondEna(1'b0),
  .gPercentEna(1'b0), .debug_system(32'd0), .debug_system_on(1'b0),
  .hGBNewLine(gb_newline), .hGBAddress(gb_addr), .hGBWrite(), .hGBData(), .hHsync(), .hVsync(),
  .hWrBurstQ(16'd0), .hWrBurstQ2(16'd0), .hDrawOSD(), .gPanelDbg(panel_dbg), .LCD_INIT_DONE(lcd_init_done),
  .gb_lcd_clkena(valid), .gb_lcd_data(pixel), .gb_lcd_mode(mode), .gb_lcd_on(1'b1), .gb_lcd_vsync(vsync));
 ramcart cart (.x_diag(x_diag), .h_diag_run(gg_dbg[47]), .hclk(hclk), .xclk(pclk), .h_on(rc_on), .lin(rc_lin), .lin_ram(rc_lin_ram), .lin_a(rc_lin_a),
 .h_mbc(3'd5), .h_rom_mask(8'hff), .h_ram_mask(4'd3), .h_ram_has(1'b1), .h_rtc_has(1'b0),
 .h_inval_tog(inval), .h_mreset_tog(1'b0), .h_busy(),
 .a(rc_a), .rd(rc_rd), .wr(rc_wr), .di(rc_di), .q(rc_q), .stall(rc_stall),
 .h_dirty(), .h_dbg(), .h_dirty_clr(1'b0), .h_rtc_set(1'b0), .h_rtc_in(40'd0), .h_rtc_out(),
 .h_wread_tog(1'b0), .h_wread_addr(23'd0), .h_wread_len(11'd0), .h_wread_off(1'b0), .h_wread_done_tog(),
 .h_cw_ready(), .h_cw_go(1'b0), .h_cw_addr(23'd0), .h_cw_data(16'd0),
 .qclk(hclk), .q_win_addr(9'd0), .q_win_data(),
 .x_req(x_req), .x_rnw(x_rnw), .x_addr(x_addr), .x_len(x_len), .x_din(x_din),
 .x_write_next(1'b0), .x_done(x_done), .x_dout_valid(x_dout_valid), .x_dout(x_dout));
endmodule

// Functional equivalent of xilinx_ram.vhd for the two external work/save RAMs.
// The VDP's RAMs are already included in GHDL's generated system_sim.v.
module spram #(parameter widthad_a=14, width_a=8, outdata_reg_a="UNREGISTERED")
 (input clock, input [widthad_a-1:0] address, input [width_a-1:0] data,
 input wren, output reg [width_a-1:0] q);
 reg [width_a-1:0] mem [0:(1<<widthad_a)-1];
 integer i;
 initial for(i=0;i<(1<<widthad_a);i=i+1) mem[i]=0;
 always @(posedge clock) begin
   if (wren) begin mem[address]<=data; q<=data; end
   else q<=mem[address];
 end
endmodule
