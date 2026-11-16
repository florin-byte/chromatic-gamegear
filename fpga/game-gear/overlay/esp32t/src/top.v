// top.v


module top #(
    parameter ISSIMU=0,
    // USB capture default: 1 = 320x288, 0 = native 160x144.
    // The host can explicitly select either resolution regardless of this.
    parameter UVC_DEFAULT_SCALE_2X=1'b1
)
(
    output              ADC_SEL,
    output              AUD_BCLK,
    output              AUD_DIN,
    output              AUD_MCLK,
    output              AUD_RESET,
    output              AUD_WCLK,

    input               BTN_A,
    input               BTN_B,
    input               BTN_DPAD_DOWN,
    input               BTN_DPAD_LEFT,
    input               BTN_DPAD_RIGHT,
    input               BTN_DPAD_UP,
    input               BTN_MENU,
    input               BTN_SEL,
    input               BTN_START,

    output  [15:0]      CART_A,
    output              CART_CLK,
    output              CART_CS,
    inout   [7:0]       CART_D,
    output              CART_RD,
    inout               CART_RST,
    output              CART_WR,
    output              CART_DATA_DIR_E,
    output              CART_CTRL_OE, // active-high cartridge shifter enable (new boards)
    output              CART_PWR_EN,
    input               CART_DET,
    input               CART_AUDIN,

    input               CLK_FPGA,       // 33.55432MHz
    input               CLK_27MHz,
    input               CLK_24MHz,

    output reg          ESP32_EN,

    output              SDIO_LS,
    input               POWER_ON_FPGA,
    output              POWER_DOWN_IO,
//    input               VBUS_DET,  // unused pin

    output reg          ESP32_IO0,

    output              I2S_BCLK,       // D16 IO33
    input               I2S_WS,         // D15 IO25 CC
    input               I2S_DIN,        // D14 IO26
    input               I2S_DOUT,       // D13 IO27
    input               ESP32_MCU_D12,  // D12 IO9
    output              ESP32_MCU_D11,  // D11 IO10 CC
    input               QSPI_CS,        // CS D10 IO5 CC
    input               QSPI_CLK,       // CLK D9 IO18 CC
    inout               QSPI_MOSI,      // D D8 IO23   (1.1.0: driven by the FPGA in a window read)
    inout               QSPI_MISO,      // Q D7 IO19
    inout               QSPI_WP,        // WP D6 IO22
    inout               QSPI_HD,        // HD D5 IO21 CC
    // Shiny Relay 1.1.0: the FPGA's own boot flash on the MSPI pins (user I/O after configuration)
    output              FLASH_MCLK,
    output              FLASH_MCSN,
    output              FLASH_MOSI,
    input               FLASH_MISO,
    output              FLASH_MD2,      // /WP: high
    output              FLASH_MD3,      // /HOLD: high
    output  reg         ESP32_MCU_D4,   // RXD
    input               ESP32_MCU_D3,   // TXD

    output              FPGA_LED_EN,
    output  reg         FPGA_LED_R,
    output  reg         FPGA_LED_G,
    output  reg         FPGA_LED_B,


    input               IR_RX,
    output              IR_LED,

    output              LCD_PWM,

    output [5:0]        LCD_DB,
    output              LCD_DOTCLK,
    output              LCD_ENABLE,
    output              LCD_HSYNC,
    output              LCD_RESET,
    output              LCD_SPI_CSX,
    output              LCD_SPI_SCLK,
    output              LCD_SPI_SDA,
    input               LCD_TE,
    output              LCD_VSYNC,

    inout               LINK_CLK,
    output              LINK_CLK_DIR_LV, // controls data flow in level shifter    
    input               LINK_IN,
    output              LINK_OUT,
    input               LINK_SD,  // changed from output to input, 
//     output              LINK_SD,
    output              LINK_SD_DIR_LV,  // 


    output              PS_CE_N,
    output              PS_CLK,
    inout   [7:0]       PS_DQ,
    inout               PS_DQS,

    inout               SCL,
    inout               SDA,

//    input               USBC_FLIP,  // unused signal
    inout               usb_dxp_io,
    inout               usb_dxn_io,
    input               usb_rxdp_i,
    input               usb_rxdn_i,
    output              usb_pullup_en_o,
    inout               usb_term_dp_io,
    inout               usb_term_dn_io,

    input               VBAT_ADC_P,
    input               VBAT_ADC_N,

    input               VERSION_DET, // discriminates between versions
    input               VERSION_DET2,
    input               DISPLAY_ID,
    output              CHG_EN_FPGA
);


// ---------------------------------------------------------------
// 'POWER_ON_FPGA` high when chromatic powered off USB but
// power switch off.  
//
// In this state, need to enable the USB charger if LiPo battery
// connected
// ---------------------------------------------------------------


    wire [7:0] temperature;
    wire emulator_was_reset; // signal from emulator core, indicating a reset took place
    wire appear_off = emulator_was_reset | POWER_ON_FPGA; // turn stuff off , alternatively could set CART_RST = ~POWER_ON_FPGA ...

    // Cartridge power and isolation are sequenced below, independently of
    // emulator/menu resets and USB activity.

// SD link not actively used, configure so that FPGA side is always an input
// nothing connected to SD card => open (high impedance)
// L (1'b0): B to A  
// H (1'b1): A to B
//
// A-side connected to FPGA, B-side connected to upstream components.
    assign LINK_SD_DIR_LV = 1'b0; 
////////////// 

    assign POWER_DOWN_IO = 1'bZ;
    assign SDIO_LS = 1'd1;

    wire    BIST_failed;
    wire    BIST_finished;

    assign FPGA_LED_EN = !POWER_ON_FPGA;

    wire lock_o;

    wire fClk;
    wire pClk;
    wire hClk;
    wire gClk;
    wire xClk;


    Gowin_PLL u_Gowin_PLL(
        .reset(1'd0),//input reset
        .clkout0(fClk), //output clkout0 ~150MHz
        .clkout1(pClk), //output clkout1 ~33.554MHz
        .clkout2(hClk), //output clkout2 ~16.777MHz
        .clkout3(gClk), //output clkout3 ~8.388MHz
        .clkout4(xClk), //output clkout4 ~75MHz
//        .clkout5(hdmiclk), //output clkout4 ~75MHz
        .lock(lock_o), //output lock
        .clkin(CLK_FPGA) //input clkin
    );

    reg [22:0] secondCounter = 'd0;
    reg secondEna;
    reg halfSecondEna;
    reg [16:0] percentCounter = 'd0;
    reg percentEna;

    always@(posedge gClk) begin
        percentEna <= 1'b0;
        if (percentCounter == 83886) begin
            percentEna     <= 1'b1;
            percentCounter <= 17'd0;
        end else begin
            percentCounter <= percentCounter + 1'd1;
        end

        secondEna      <= 1'b0;
        halfSecondEna  <= 1'b0;
        if (secondCounter == 4194303) begin
            halfSecondEna  <= 1'b1;
        end
        if (secondCounter == 8388607) begin
            secondEna      <= 1'b1;
            halfSecondEna  <= 1'b1;
            secondCounter  <= 23'd0;
            percentCounter <= 17'd0;
        end else begin
            secondCounter <= secondCounter + 1'd1;
        end

    end


// ================================================================================
// Latch Cart RST. 
// 
// Monitor the high-impedance CART_RST signal.  Should stay high ordinarily.  
// Reset the Gameboy core in emu_system_top, if CART_RST goes low as it could be
// imply an issue. 
//
// If CART_RST deasserts, latch it and maintain an active low reset for 256 clock
// cycles.  This "latched_cart_rst_n" feeds the gameboy core reset.
// =================================================================================
`define ENABLE_CART_RST_MONITOR
`ifdef ENABLE_CART_RST_MONITOR

//
//    reg [7:0] rst_counter;
    reg [3:0] rst_counter;
    reg       timeout_done;
    reg [1:0] cart_rst_sync;
    reg       latched_cart_rst_n;
    always@(posedge xClk or negedge CART_RST) begin
        if(!CART_RST) begin
            timeout_done <= 1'b0;
            cart_rst_sync <= 2'b00;
            rst_counter <= 4'h0;
        end else begin
            if (rst_counter < 4'hF/*8'hff*/) begin
                rst_counter <= rst_counter + 1;
            end else begin
                timeout_done <= 1'b1;
            end
            cart_rst_sync <= {cart_rst_sync[0],timeout_done};
        end
    end
    assign latched_cart_rst_n = cart_rst_sync[1];


`else 
    wire latched_cart_rst_n = 1'b1;
`endif

//    localparam HALF_SECOND_XCLK_TIMER = 37_500_000;

    wire low_battery;
    wire boot_rom_enabled;
    wire LED_Green;
    wire LED_Red;
    wire LED_Yellow;
    wire LED_White;
    wire [7:0]  pmic_sys_status;



    always@(posedge xClk) begin

        if (~lock_o) begin 
            FPGA_LED_R <= 1'b1;
            FPGA_LED_B <= 1'b1;
            FPGA_LED_G <= 1'b1;  
        end
        else begin 

                // normal operations - switch on and/or not connected over USB
                if (LED_White) begin
                    FPGA_LED_R <= 1'd0; //1'd0;
                    FPGA_LED_B <= 1'd0; //1'd0;
                    FPGA_LED_G <= 1'd0;//1'd0;
                end else if (LED_Green) begin
                    FPGA_LED_R <= 1'd1;
                    FPGA_LED_B <= 1'd1;
                    FPGA_LED_G <= 1'd0;
                end else if (LED_Yellow) begin
                    FPGA_LED_R <= 1'd0;
                    FPGA_LED_B <= 1'd1;
                    FPGA_LED_G <= secondCounter[4];
                end else if (LED_Red) begin
                    FPGA_LED_R <= 1'd0;
                    FPGA_LED_B <= 1'd1;
                    FPGA_LED_G <= 1'd1;
                end else begin
                    FPGA_LED_R <= 1'd1;
                    FPGA_LED_B <= 1'd1;
                    FPGA_LED_G <= 1'd1;
                end

        end
    end

    wire [15:0]       hWrBurstQ;
    wire [15:0]       hWrBurstQ2;
    wire              hHsync;
    wire              hVsync;

    wire gb_lcd_clkena;
    wire [23:0] fb_dbg;                 // 18.32 bench (mem_system_top hFbDbg)
    wire [14:0] gb_lcd_data;
    wire [1:0] gb_lcd_mode;
    wire gb_lcd_on;
    wire gb_lcd_vsync;
    wire LCD_INIT_DONE;

    wire              hGBNewLine;
    wire [22:0]       hGBAddress;
    wire              hGBWrite;
    wire [15:0]       hGBData;
    wire capture_valid, capture_frame_start, capture_line_end;


    reg LCD_VSYNC_r1;
    always@(posedge gClk)
        LCD_VSYNC_r1 <= LCD_VSYNC;

    reg memrst = 1'd1;

    reg LCD_EN1;
    reg LCD_EN0;
    reg LCD_EN;
    wire qMenuInit;
    wire LCD_BACKLIGHT_INIT;
    always@(posedge gClk or posedge memrst) begin
        if(memrst) begin
            LCD_EN <= 1'd0;
            LCD_EN0 <= 1'd0;
            LCD_EN1 <= 1'd0;
        end else begin
            if(LCD_VSYNC&~LCD_VSYNC_r1) begin
                LCD_EN0 <= LCD_INIT_DONE & LCD_BACKLIGHT_INIT;
                LCD_EN1 <= LCD_EN0;
                LCD_EN  <= LCD_EN1;
            end
            // synthesis translate_off
            LCD_EN  <= 1'd1;
            // synthesis translate_on
        end
    end

    wire [31:0] debug_system;
    wire [15:0] system_control;
    wire [17:0] capture_pixel;
    wire menuDisabled;
    wire hDrawOSD;
    vid_system_top #(ISSIMU)
    u_vid_system_top(
        .appear_off (appear_off),
        .gClk(gClk),
        .hClk(hClk),
        .pClk(pClk),
        .reset(memrst ),    

        .BTN_MENU(menuDisabled),

        .LCD_DB(LCD_DB),  
        .capture_valid(capture_valid),
        .capture_frame_start(capture_frame_start),
        .capture_line_end(capture_line_end),
        .capture_pixel(capture_pixel),
        .LCD_DOTCLK(LCD_DOTCLK),
        .LCD_ENABLE(LCD_ENABLE),
        .LCD_HSYNC(LCD_HSYNC),
        .LCD_EN(LCD_EN),
        .LCD_RESET(LCD_RESET), 
        .LCD_SPI_CSX(LCD_SPI_CSX),
        .LCD_SPI_SCLK(LCD_SPI_SCLK),
        .LCD_SPI_SDA(LCD_SPI_SDA),
        .LCD_TE(LCD_TE),
        .LCD_VSYNC(LCD_VSYNC),
        .LCD_GENLOCK(),

        .frameBlendEnable(system_control[1]),
        .colorCorrectionEnableLCD(system_control[2]),
        .colorCorrectionEnableUVC(system_control[3]),
        .voltageLow(low_battery),
        .lowBattDispMode(system_control[14:13]),
        .showTimer(1'b0), //system_control[8]),
        .runTimer(system_control[9]),
        .resetTimer(system_control[10]),
        .gSecondEna(secondEna),
        .gPercentEna(percentEna),
        .debug_system(debug_system),
        .debug_system_on(1'b0),

        .hDrawOSD(hDrawOSD),
        .gPanelDbg(rc_dbg),
        .hGBNewLine(hGBNewLine),
        .hGBAddress(hGBAddress),
        .hGBWrite(hGBWrite),
        .hGBData(hGBData),

        .hHsync(hHsync),
        .hVsync(hVsync),
        .hWrBurstQ(hWrBurstQ),
        .hWrBurstQ2(hWrBurstQ2),

        .LCD_INIT_DONE(LCD_INIT_DONE),
        .gb_lcd_clkena(gb_lcd_clkena), 
        .gb_lcd_mode(gb_lcd_mode),
        .gb_lcd_on(gb_lcd_on),   
        .gb_lcd_vsync(gb_lcd_vsync), 
        .gb_lcd_data(gb_lcd_data)
    );



    wire [15:0] left, right;
    wire [7:0]  volume;
    wire        hHeadphones;

    aud_system_top_8_16 u_aud_system_top(     
        .VERSION_DET (VERSION_DET),
        .appear_off (appear_off),
        .gClk(gClk),
        .hClk(hClk),
        .reset_n(lock_o),
        .left(left),
        .right(right),

        .AUD_BCLK(AUD_BCLK),
        .AUD_DIN(AUD_DIN),
        .AUD_DOUT(1'b0), // Codec return audio is unused.
        .AUD_MCLK(AUD_MCLK),
        .AUD_RESET(AUD_RESET),
        .AUD_WCLK(AUD_WCLK),

        .software_mute(system_control[0]),
        .pmic_sys_status(pmic_sys_status),
        .volume(volume),
        .temperature(temperature),
        .hHeadphones(hHeadphones),
        .SCL(SCL),
        .SDA(SDA),
        .POWER_ON_FPGA (POWER_ON_FPGA)
//        .use_16_bit (use_16_bit)
    );    

// =====================================================================
// Previously, both of these clocked processes used "xClk".
// Using the higher frequency "fClk" domain removed recovery timing
// violations without impacting functionality
// =====================================================================
    reg [17:0] CART_DET_sr;
    always@(posedge fClk)
        CART_DET_sr <= {CART_DET_sr[16:0], CART_DET};

    // CART_DET = 0 (no cart inserted)
    always@(posedge fClk or negedge lock_o)
        if(~lock_o)
            memrst <= 1'd1;
        else
            memrst <= CART_DET_sr[17:2] == 16'h7FFF || CART_DET_sr[17:2] == 16'h8000;

`ifdef OPEN_BUILD
    // Open-source flow: B1 is not a clock-capable pin, so nextpnr would route
    // QSPI_CLK on general fabric (skew, hold violations). Put it on a global.
    wire QSPI_CLK_g;
    BUFG u_qspi_clk_bufg(.I(QSPI_CLK), .O(QSPI_CLK_g));
`else
    wire QSPI_CLK_g = QSPI_CLK;
`endif

    // cartspeed1: the QSPI byte stream for the express cart ring (cart_xs.v)
    wire        xs_q_addr_ready, xs_q_byte_valid;
    wire [31:0] xs_q_address;
    wire  [7:0] xs_q_byte;

    // Shiny Relay 1.1.0: the RAM cart (ramcart.v) and the QSPI read window
    wire        rc_x_req, rc_x_rnw, rc_x_write_next, rc_x_done, rc_x_dout_valid;
    wire [22:0] rc_x_addr;
    wire [10:0] rc_x_len;
    wire [15:0] rc_x_din, rc_x_dout;
    wire  [2:0] rc_density;
    wire  [8:0] q_win_addr;
    wire [15:0] q_win_data;
    wire [15:0] xs_fwin;         // 18.31: FREAD's window (cart_xs)
    wire        xs_fwin_sel;
    wire [15:0] rc_win_data;
    // 18.31: the window of the last FREAD / RREAD; a tagged one answers its tag at word 0 (the address compare
    // registered on the QSPI clock beside the window BSRAMs' registered reads)
    wire        xs_wtag_on;
    wire [15:0] xs_wtag;
    reg         q_win_w0 = 1'b0;
    always @(negedge QSPI_CLK_g) q_win_w0 <= (q_win_addr == 9'd0);   // = the windows' read clock (qspi_clk_n)
    assign q_win_data = (xs_wtag_on && q_win_w0) ? xs_wtag : xs_fwin_sel ? xs_fwin : rc_win_data;
    wire        q_out_en;
    wire  [3:0] q_out;
    assign QSPI_MOSI = q_out_en ? q_out[0] : 1'bz;
    assign QSPI_MISO = q_out_en ? q_out[1] : 1'bz;
    assign QSPI_WP   = q_out_en ? q_out[2] : 1'bz;
    assign QSPI_HD   = q_out_en ? q_out[3] : 1'bz;
    wire        rc_on, rc_hold, rc_stall, rc_rd, rc_wr, rc_busy, rc_dirty, rc_dirty_clr, rc_inval_tog, rc_mreset_tog;
    wire  [7:0] rc_q, rc_di, rc_rom_mask;
    wire [15:0] rc_a;
    wire rc_lin, rc_lin_ram;
    wire [21:0] rc_lin_a;
    wire  [2:0] rc_mbc;
    wire  [3:0] rc_ram_mask;
    wire        rc_ram_has, rc_rtc_has, rc_wread_tog, rc_wread_done_tog, rc_rtc_set;
    wire [22:0] rc_wread_addr;
    wire [10:0] rc_wread_len;
    wire [39:0] rc_rtc_in, rc_rtc_out;
    wire [47:0] rc_dbg;
    wire        fl_sck, fl_cs_n, fl_mosi;
    assign FLASH_MCLK = fl_sck;
    assign FLASH_MCSN = fl_cs_n;
    assign FLASH_MOSI = fl_mosi;
    assign FLASH_MD2  = 1'b1;
    assign FLASH_MD3  = 1'b1;

    mem_system_top #(ISSIMU)
    u_mem_system_top
    (
        .xClk(xClk),
        .fClk(fClk),
        .hClk(hClk),
        .reset (memrst), 
        .QSPI_CLK(QSPI_CLK_g),
        .QSPI_MOSI(QSPI_MOSI),
        .QSPI_MISO(QSPI_MISO),
        .QSPI_CS(QSPI_CS),
        .QSPI_WP(QSPI_WP),
        .QSPI_HD(QSPI_HD),

        .PS_CE_N(PS_CE_N),
        .PS_CLK(PS_CLK),
        .PS_DQ(PS_DQ),
        .PS_DQS(PS_DQS),

        .BIST_failed(BIST_failed),
        .BIST_finished(BIST_finished),
        .qAddrReady(xs_q_addr_ready),
        .qAddressOut(xs_q_address),
        .qByteValid(xs_q_byte_valid),
        .qByte(xs_q_byte),
        .qMenuInit(qMenuInit),
        .hGBNewLine(hGBNewLine),
        .hGBAddress(hGBAddress),
        .hGBWrite(hGBWrite),
        .hGBData(hGBData),

        // mm_burst_read_to_stream
        .hValid(gb_lcd_clkena),
        .hHsync(gb_lcd_mode[1]),
        .hVsync(gb_lcd_vsync),
        .hWrBurstQ(hWrBurstQ),
        .hWrBurstQ2(hWrBurstQ2),
        .hFbDbg(fb_dbg),
        .xRcReq(rc_x_req),
        .xRcRnW(rc_x_rnw),
        .xRcAddr(rc_x_addr),
        .xRcLen(rc_x_len),
        .xRcDin(rc_x_din),
        .xRcWriteNext(rc_x_write_next),
        .xRcDone(rc_x_done),
        .xRcDoutValid(rc_x_dout_valid),
        .xRcDout(rc_x_dout),
        .ramDensity(rc_density),
        .qWinAddr(q_win_addr),
        .qWinData(q_win_data),
        .qOutEn(q_out_en),
        .qOut(q_out)
    );
    // 18.32 bench (RDBG, op 35): [15:0] rows the panel waited for (vid_system_top, gClk); the RAM cart's stalls per core
    // frame (hClk, vsync to vsync, in units of 16 hClk: a row is 114): [23:16] frames over 2 rows, [31:24] over 4 rows
    // (the panel's slack: such a frame shook before 18.32), [47:32] the most in one frame
    reg [19:0] hStallFrame = 20'd0;
    reg [15:0] hStallMax = 16'd0;
    reg  [7:0] hOver2 = 8'd0, hOver4 = 8'd0;
    reg        hVs_r = 1'b0;
    always @(posedge hClk) begin
        hVs_r <= gb_lcd_vsync;
        if (gb_lcd_vsync & ~hVs_r) begin
            if (hStallFrame[19:4] > hStallMax) hStallMax <= hStallFrame[19:4];
            if (hStallFrame[19:4] > 16'd228 && hOver2 != 8'hFF) hOver2 <= hOver2 + 1'd1;
            if (hStallFrame[19:4] > 16'd456 && hOver4 != 8'hFF) hOver4 <= hOver4 + 1'd1;
            hStallFrame <= 20'd0;
        end else if (rc_stall && hStallFrame != 20'hFFFFF) hStallFrame <= hStallFrame + 1'd1;
    end
    reg [47:0] rc_dbg_g2 = 48'd0;
    // a= {end holds, rows waited}, fills= {menu lines late, blend lines late}, flags= {frame pixels dropped, late last pixels}
    wire [102:0] rc_x_diag;
    wire [47:0] gg_dbg;   // CG-117 diag: the Game Gear core's state instead of the panel counters
    always @(posedge gClk) rc_dbg_g2 <= gg_dbg;   // was: {fb_dbg[23:16], rc_dbg[39:32], fb_dbg[15:0], rc_dbg[23:16], rc_dbg[7:0]};   // all wrap; flags low byte = rows whose last pixel came late

    wire IR_RX_FILTER;

    wire lcd_on_int;
    wire lcd_off_overwrite;

    wire [8:0] MCU_buttons;
    
    //////////////////////////////////////////////// 
    // apply debounce logic to menu button
//    wire BTN_MENU_ored = BTN_MENU & ~MCU_buttons[8]; // BTN_MENU is low active


    wire nBTN_MENU_filtered;
    wire nBTN_MENU = ~BTN_MENU;
    button_debouncer debouncer_MENU     (gClk, nBTN_MENU     , nBTN_MENU_filtered     );
    wire BTN_MENU_filtered = ~nBTN_MENU_filtered & ~MCU_buttons[8];
    //////////////////////////////////////////////// 
    wire BTN_A_filtered;
    wire BTN_B_filtered;
    wire BTN_DPAD_DOWN_filtered;
    wire BTN_DPAD_LEFT_filtered;
    wire BTN_DPAD_RIGHT_filtered;
    wire BTN_DPAD_UP_filtered;
    wire BTN_SEL_filtered;
    wire BTN_START_filtered;

    button_debouncer debouncer_A         (gClk, BTN_A         , BTN_A_filtered         );
    button_debouncer debouncer_B         (gClk, BTN_B         , BTN_B_filtered         );
    button_debouncer debouncer_DPAD_DOWN (gClk, BTN_DPAD_DOWN , BTN_DPAD_DOWN_filtered );
    button_debouncer debouncer_DPAD_LEFT (gClk, BTN_DPAD_LEFT , BTN_DPAD_LEFT_filtered );
    button_debouncer debouncer_DPAD_RIGHT(gClk, BTN_DPAD_RIGHT, BTN_DPAD_RIGHT_filtered);
    button_debouncer debouncer_DPAD_UP   (gClk, BTN_DPAD_UP   , BTN_DPAD_UP_filtered   );
    button_debouncer debouncer_SEL       (gClk, BTN_SEL       , BTN_SEL_filtered       );
    button_debouncer debouncer_START     (gClk, BTN_START     , BTN_START_filtered     );

    wire [63:0] paletteBGIn;
    wire [63:0] paletteOBJ0In;
    wire [63:0] paletteOBJ1In;
    wire [2:0]  gbc_color_temp;
    wire gbc_mode;
    wire [63:0] gpd;



    wire cartridge_ready;
    wire [15:0] core_cart_a;
    wire [7:0] core_cart_d_in, core_cart_d_out;
    wire core_cart_clk, core_cart_cs, core_cart_rd, core_cart_wr;
    wire core_cart_data_dir_e;
    reg [1:0] cartridge_reset_sync = 2'b00;
    always @(posedge hClk or negedge lock_o)
        if (!lock_o) cartridge_reset_sync <= 2'b00;
        else cartridge_reset_sync <= {cartridge_reset_sync[0], 1'b1};

    // Debug (ESP32 link cmd 5, `cwl cartcycle`): power the cartridge off for
    // ~1 s, as the power switch would, so a flash cart (EZ-Flash Junior)
    // returns to its menu; a GB reset then boots it. A soft reset or an FPGA
    // reload keeps the cartridge powered and replays the loaded game.
    wire link_h_cart_cycle;
    // cartflash1-engine (DRAFT, test bitstreams): cart bus engine (rtl/BSP/cart_engine.v)
    wire        link_h_eng_allow;          // link cmd 9 ENGINE_ALLOW from the ESP32
    wire        eng_active, eng_clk, eng_cs, eng_rd, eng_wr, eng_dir_e, eng_sel, eng_busy;
    wire [15:0] eng_a;
    wire  [7:0] eng_d_out, eng_do;
    wire link_h_boot_alt;          // cartflash1: link cmd 10, boot image select
    reg [24:0] cart_off_cnt = 25'd0;
    always @(posedge hClk or negedge lock_o)
        if (!lock_o) cart_off_cnt <= 25'd0;
        else if (link_h_cart_cycle) cart_off_cnt <= 25'd16777215;
        else if (cart_off_cnt != 25'd0) cart_off_cnt <= cart_off_cnt - 25'd1;

    cartridge_interface u_cartridge_interface (
        .clk(hClk),
        .reset_n(cartridge_reset_sync[1]),
        .cartridge_enable(~POWER_ON_FPGA && cart_off_cnt == 25'd0 && ~rc_on),   // 1.1.0: off while the RAM cart plays
        .version_detect(VERSION_DET),
        .cartridge_ready(cartridge_ready),
        // cartflash1-engine: the cart bus engine owns the pins during a session
        .core_a(eng_active ? eng_a : core_cart_a),
        .core_clk(eng_active ? eng_clk : core_cart_clk),
        .core_cs(eng_active ? eng_cs : core_cart_cs),
        .core_rd(eng_active ? eng_rd : core_cart_rd),
        .core_wr(eng_active ? eng_wr : core_cart_wr),
        .core_d_out(eng_active ? eng_d_out : core_cart_d_out),
        .core_d_in(core_cart_d_in),
        .core_data_dir_e(eng_active ? eng_dir_e : core_cart_data_dir_e),
        .CART_A(CART_A),
        .CART_CLK(CART_CLK),
        .CART_CS(CART_CS),
        .CART_RD(CART_RD),
        .CART_WR(CART_WR),
        .CART_D(CART_D),
        .CART_RST(CART_RST),
        .CART_DATA_DIR_E(CART_DATA_DIR_E),
        .CART_CTRL_OE(CART_CTRL_OE),
        .CART_PWR_EN(CART_PWR_EN)
    );

    // CWL1 network link wires
    wire       link_h_net_en;
    wire       link_h_master_reply_pend;
    wire [7:0] link_h_master_reply_byte;
    wire       link_h_master_reply_drop;
    wire       link_h_barrier_en;
    wire       link_h_master_reply_ack;
    wire       link_h_slave_xfer_pend;
    wire [7:0] link_h_slave_xfer_byte;
    wire       link_h_slave_xfer_ack;
    wire       link_h_standing_en;
    wire       link_h_slave_withdraw_pend;
    wire       link_h_slave_withdraw_ack;
    wire       link_h_ev_valid;
    wire [2:0] link_h_ev_type;
    wire [7:0] link_h_ev_byte;

    // network window: GB bus (hClk) and system monitor channel 11 (gClk)
    wire [15:0] nw_cpu_addr;
    wire  [7:0] nw_cpu_do;
    wire        nw_cpu_rd_n, nw_cpu_wr_stb, nw_sel;
    wire  [7:0] nw_do;
    wire        nw_g_tx_valid, nw_g_tx_taken, nw_g_esp_awake, nw_g_unlocked;
    wire  [7:0] nw_g_tx_len, nw_g_tx_byte, nw_g_tx_bytepos;
    wire       link_g_ev_valid;
    wire [2:0] link_g_ev_type;
    wire [7:0] link_g_ev_byte;
    wire       link_g_ev_taken;
    wire       link_g_cmd_valid;
    wire [7:0] link_g_cmd;
    wire [7:0] link_g_cmd_byte;
    wire       link_g_net_en;
    wire       link_g_esp_may_sleep;

    emu_system_top u_emu_system_top(
        .o_emulator_reset (emulator_was_reset),
        .gg_dbg(gg_dbg),
        .x_diag(rc_x_diag),
        .hclk(hClk),
        .pclk(pClk),
        .fclk(fClk),
        .xclk(xClk), 
        .reset_n(~memrst),//lock_o),
        .POWER_GOOD(cartridge_ready | rc_on),   // 1.1.0: the RAM cart needs no slot power
        .boot_alt(link_h_boot_alt),   // cartflash1: alternate boot image (link cmd 10)

        .customPaletteEna(paletteBGIn[63]),
        .paletteOff(system_control[12]),
        .paletteBGIn(paletteBGIn),
        .paletteOBJ0In(paletteOBJ0In),
        .paletteOBJ1In(paletteOBJ1In),
        .gbc_color_temp(gbc_color_temp),
        .gbc_mode(gbc_mode),
        .gpd(gpd),

        .BTN_NODIAGONAL(system_control[11]),
        .BTN_A(BTN_A_filtered | MCU_buttons[3]),
        .BTN_B(BTN_B_filtered | MCU_buttons[2]),
        .BTN_DPAD_DOWN(BTN_DPAD_DOWN_filtered | MCU_buttons[7]),
        .BTN_DPAD_LEFT(BTN_DPAD_LEFT_filtered | MCU_buttons[6]),
        .BTN_DPAD_RIGHT(BTN_DPAD_RIGHT_filtered | MCU_buttons[5]),
        .BTN_DPAD_UP(BTN_DPAD_UP_filtered | MCU_buttons[4]),

        .BTN_MENU(~BTN_MENU_filtered), // debounce menu button too
//         .BTN_MENU(~BTN_MENU_ored),

        .BTN_SEL(BTN_SEL_filtered | MCU_buttons[1]),
        .BTN_START(BTN_START_filtered | MCU_buttons[0]),
        .MENU_CLOSED(menuDisabled),

        .CART_A(core_cart_a),
        .CART_CLK(core_cart_clk),
        .CART_CS(core_cart_cs),
        .CART_D_IN(core_cart_d_in),
        .CART_D_OUT(core_cart_d_out),
        .CART_RD(core_cart_rd),
        .CART_WR(core_cart_wr),
        .CART_DATA_DIR_E(core_cart_data_dir_e),

        .IR_RX(IR_RX),
        .IR_LED(IR_LED),    

        .LINK_CLK(LINK_CLK),
        .LINK_CLK_DIR_LV (LINK_CLK_DIR_LV), // control direction of clock through core
        .LINK_IN(LINK_IN),
        .LINK_OUT(LINK_OUT),

        // CWL1 network link
        .net_en                (link_h_net_en),
        .net_master_reply_pend (link_h_master_reply_pend),
        .net_master_reply_byte (link_h_master_reply_byte),
        .net_master_reply_drop (link_h_master_reply_drop),
        .net_barrier_en        (link_h_barrier_en),
        .net_master_reply_ack  (link_h_master_reply_ack),
        .net_slave_xfer_pend   (link_h_slave_xfer_pend),
        .net_slave_xfer_byte   (link_h_slave_xfer_byte),
        .net_slave_xfer_ack    (link_h_slave_xfer_ack),
        .net_standing_en       (link_h_standing_en),
        .net_slave_withdraw_pend (link_h_slave_withdraw_pend),
        .net_slave_withdraw_ack  (link_h_slave_withdraw_ack),
        .net_ev_valid          (link_h_ev_valid),
        .net_ev_type           (link_h_ev_type),
        .net_ev_byte           (link_h_ev_byte),
        .nw_cpu_addr           (nw_cpu_addr),
        .nw_cpu_do             (nw_cpu_do),
        .nw_cpu_rd_n           (nw_cpu_rd_n),
        .nw_cpu_wr_stb         (nw_cpu_wr_stb),
        .nw_sel                (nw_sel | eng_sel),
        .nw_do                 (eng_sel ? eng_do : nw_do),

        .lcd_on_int(lcd_on_int),
        .lcd_off_overwrite(lcd_off_overwrite),

        .boot_rom_enabled(boot_rom_enabled),

        // audio
        .left(left),
        .right(right),
        // video
        .LCD_INIT_DONE(LCD_INIT_DONE),
        .gb_lcd_clkena(gb_lcd_clkena),
        .gb_lcd_mode(gb_lcd_mode),
        .gb_lcd_on(gb_lcd_on),
        .gb_lcd_vsync(gb_lcd_vsync),
        .gb_lcd_data(gb_lcd_data),   

        .latched_cart_rst_n (latched_cart_rst_n),
        .rc_on              (rc_on),
        .rc_hold            (rc_hold),
        .rc_q               (rc_q),
        .rc_stall           (rc_stall),
        .rc_a               (rc_a),
        .rc_lin             (rc_lin),
        .rc_lin_ram         (rc_lin_ram),
        .rc_lin_a           (rc_lin_a),
        .rc_rd              (rc_rd),
        .rc_wr              (rc_wr),
        .rc_di              (rc_di)
    );

    reg UART_TXD;
    wire UART_RXD;
    wire PHY_CLKOUT;
    wire usblocked;
    always@(posedge PHY_CLKOUT or negedge usblocked)
    begin
        if(~usblocked)
        begin
            UART_TXD     <= 1'd1;
            ESP32_MCU_D4 <= 1'd1;
        end
        else
        begin
            UART_TXD     <= ESP32_MCU_D3;
            ESP32_MCU_D4 <= UART_RXD;
        end
    end
    wire UART_DTR;
    wire UART_RTS;
    wire [1:0] DTRRTS = {UART_DTR, UART_RTS};



    reg [11:0] ESP_BOOT_DELAY_COUNTER = 0;
    reg [7:0] ESP_BOOT_DELAY_SHIFT = 0;


    reg ESP32_EN_INT = 1;
    reg ESP32_IO0_INT = 1;

    // 8MHz clock
    always@(posedge gClk) begin

        ESP32_IO0 <= ESP32_IO0_INT;

        ESP_BOOT_DELAY_COUNTER <= ESP_BOOT_DELAY_COUNTER + 1'b1;

        if(ESP_BOOT_DELAY_COUNTER == 0) begin
            ESP_BOOT_DELAY_SHIFT <= {ESP_BOOT_DELAY_SHIFT[6:0], ESP32_EN_INT};
            ESP32_EN <= ESP_BOOT_DELAY_SHIFT[7];
           end

        if(~ESP32_EN_INT) begin
            ESP_BOOT_DELAY_SHIFT <= 8'b0;
            ESP32_EN <= 0;
        end
    end

    always@(posedge PHY_CLKOUT or negedge usblocked)
    begin
        if(~usblocked)
        begin
            ESP32_EN_INT <= 1'd1;
            ESP32_IO0_INT <= 1'd1;
        end
        else
        begin
            // 18.28: reset only for RTS on with DTR off (the classic auto-reset circuit), not for both on:
            // a port opens with DTR + RTS on (Web Serial on every system), and that no longer restarts the
            // ESP32, so a page can reopen at a faster rate with the flash stub still running. esptool's
            // reset (DTR 0 RTS 1, then DTR 1 RTS 0) is unchanged: EN rises ~3.9 ms after the release
            // (ESP_BOOT_DELAY_SHIFT), long after the boot pin is low.
            ESP32_EN_INT <= ~(UART_RTS & ~UART_DTR);
            ESP32_IO0_INT <= (DTRRTS == 2'b00);
        end
    end

    wire clk24;
    wire [7:0] debugs;

    reg [23:0] usbinitcnt;
    reg usbrst = 1'd1;

    // 8388607 = 1s
    always@(posedge gClk or negedge lock_o)
        if(~lock_o)
        begin
            usbinitcnt <= 'd0;
            usbrst     <= 1'd1;
        end
        else
            if(usbinitcnt < 8388607)
            begin
                usbinitcnt <= usbinitcnt + 1'd1;
                usbrst <= 1'd1;
            end
            else
                usbrst <= 1'd0;

    usbuvcuart_top #(.DEFAULT_SCALE_2X(UVC_DEFAULT_SCALE_2X)) u_usb_top(
        .CLK_24MHz(CLK_24MHz),
        .ERST(usbrst | POWER_ON_FPGA),   // hold in reset while switch in off position
        .pClk(PHY_CLKOUT),
        .usblocked(usblocked),
        .hClk(gClk),

        .UART_TXD(UART_RXD), // output
        .UART_RXD(UART_TXD), // input
        .UART_CTS(1'b0), // Active-low CTS: hardware flow control unused.
        .E_UART_DTR(UART_DTR), // used for ESP32_EN
        .E_UART_RTS(UART_RTS), // used for ESP32_IO0 (bootloader select)

        .left(left),
        .right(right),

        .video_clk(hClk),
        .video_valid(capture_valid),
        .video_frame_start(capture_frame_start),
        .video_line_end(capture_line_end),
        .video_pixel(capture_pixel),
        .debugs(debugs),
        .playerNum({4'd0, system_control[7:4]}),
        .usb_dxp_io(usb_dxp_io),
        .usb_dxn_io(usb_dxn_io),
        .usb_rxdp_i(usb_rxdp_i),
        .usb_rxdn_i(usb_rxdn_i),
        .usb_pullup_en_o(usb_pullup_en_o),
        .usb_term_dp_io(usb_term_dp_io),
        .usb_term_dn_io(usb_term_dn_io)
    );

    wire [13:0] hAdcValue_r1;
    wire hAdcReq_ext;
    wire hAdcReady_r1;
    adc_wrap u_adc_wrap(
        .clk(gClk),
        .reset_n(lock_o),
        .hAdcReq_ext(hAdcReq_ext),
        .hAdcValue_r1(hAdcValue_r1),
        .hAdcReady_r1(hAdcReady_r1),
        .VBAT_ADC_P(VBAT_ADC_P),
        .VBAT_ADC_N(VBAT_ADC_N)
    );

    wire [7:0]  uart_tx_data;
    wire        uart_tx_busy;
    wire        uart_tx_val;

    wire [15:0] uart_rx_data;
    wire        uart_rx_val;

//////////////////////////////////////////////// 
// modify based on addition of debounce logic
//    wire menu_gated = qMenuInit&(CART_DET_sr[6:3]==4'b1111) ? BTN_MENU_ored : 1'b1;
    wire menu_gated = qMenuInit&((CART_DET_sr[6:3]==4'b1111) | rc_on) ? BTN_MENU_filtered : 1'b1;   // 1.1.0: the menu with no cart in, a RAM cart playing
////////////////////////////////////////////////

// --------------------------------------------------------------
// Enable the USB charger when the Chromatic is powered with LiPo,
// 
// however, need to wait for charger to be fully configured (I2C)
// --------------------------------------------------------------
    
   wire powered_by_lipo;
   assign CHG_EN_FPGA = 1'b1; //powered_by_lipo;


    system_monitor u_system_monitor(
        .appear_off (appear_off),
        .clk(gClk),
        .reset(~lock_o),
        .BTN_A(BTN_A_filtered),
        .BTN_B(BTN_B_filtered),
        .BTN_DPAD_DOWN(BTN_DPAD_DOWN_filtered),
        .BTN_DPAD_LEFT(BTN_DPAD_LEFT_filtered),
        .BTN_DPAD_RIGHT(BTN_DPAD_RIGHT_filtered),
        .BTN_DPAD_UP(BTN_DPAD_UP_filtered),
        .BTN_MENU(menu_gated),
        .BTN_SEL(BTN_SEL_filtered),
        .BTN_START(BTN_START_filtered),
        .menuDisabled(menuDisabled),
        .LCD_BACKLIGHT_INIT(LCD_BACKLIGHT_INIT),
        .LCD_INIT_DONE(LCD_INIT_DONE & ~boot_rom_enabled),
        .LCD_PWM(LCD_PWM),
        .hAdcReq_ext(hAdcReq_ext),
        .hAdcValue_r1(hAdcValue_r1),
        .hAdcReady_r1(hAdcReady_r1),
        .ADC_SEL(ADC_SEL),
        .hButtons(9'd0),
        .MCU_buttons(MCU_buttons),
        .hVolume(volume[6:0]),
        .pmic_sys_status(pmic_sys_status),
        .temperature(temperature),
        .hHeadphones(hHeadphones),
        .gSecondEna(secondEna),
        .gHalfSecondEna(halfSecondEna),
        .debug_system(debug_system),
        .low_battery(low_battery),
        .LED_Green(LED_Green),
        .LED_Red(LED_Red),
        .LED_Yellow(LED_Yellow),
        .LED_White(LED_White),
        .system_control(system_control),
        .paletteBGIn(paletteBGIn),
        .paletteOBJ0In(paletteOBJ0In),
        .paletteOBJ1In(paletteOBJ1In),
        .gbc_color_temp(gbc_color_temp),
        .gbc_mode(gbc_mode),
        .gpd(gpd),
        .uart_rx_data(uart_rx_data[7:0]),
        .uart_rx_val(uart_rx_val),
        .uart_tx_busy(uart_tx_busy),
        .uart_tx_data(uart_tx_data),
        .uart_tx_val(uart_tx_val),
  
        .link_ev_valid  (link_g_ev_valid),
        .link_ev_type   (link_g_ev_type),
        .link_ev_byte   (link_g_ev_byte),
        .link_ev_taken  (link_g_ev_taken),
        .link_cmd_valid (link_g_cmd_valid),
        .link_cmd       (link_g_cmd),
        .link_cmd_byte  (link_g_cmd_byte),
        .link_net_en    (link_g_net_en),
        .link_esp_may_sleep (link_g_esp_may_sleep),
        .nw_tx_valid    (nw_g_tx_valid),
        .nw_tx_len      (nw_g_tx_len),
        .nw_tx_byte     (nw_g_tx_byte),
        .nw_tx_bytepos  (nw_g_tx_bytepos),
        .nw_tx_taken    (nw_g_tx_taken),
        .nw_esp_awake   (nw_g_esp_awake),
        .xs_tx_valid    (xs_g_tx_valid),
        .xs_tx_len      (xs_g_tx_len),
        .xs_tx_byte     (xs_g_tx_byte),
        .xs_tx_bytepos  (xs_g_tx_bytepos),
        .xs_tx_taken    (xs_g_tx_taken),
        .VERSION_DET (VERSION_DET),
        .POWERED_BY_LIPO (powered_by_lipo)
    );

    // CWL1 network link: GB core (hClk) <-> system monitor UART (gClk)
    link_net_cdc u_link_net_cdc (
        .gclk                (gClk),
        .g_reset             (~lock_o),
        .hclk                (hClk),
        .h_reset             (~lock_o),
        .g_cmd_valid         (link_g_cmd_valid),
        .g_cmd               (link_g_cmd),
        .g_cmd_byte          (link_g_cmd_byte),
        .g_ev_valid          (link_g_ev_valid),
        .g_ev_type           (link_g_ev_type),
        .g_ev_byte           (link_g_ev_byte),
        .g_ev_taken          (link_g_ev_taken),
        .g_net_en            (link_g_net_en),
        .g_esp_may_sleep     (link_g_esp_may_sleep),
        .h_net_en            (link_h_net_en),
        .h_master_reply_pend (link_h_master_reply_pend),
        .h_master_reply_byte (link_h_master_reply_byte),
        .h_master_reply_drop (link_h_master_reply_drop),
        .h_barrier_en        (link_h_barrier_en),
        .h_master_reply_ack  (link_h_master_reply_ack),
        .h_slave_xfer_pend   (link_h_slave_xfer_pend),
        .h_slave_xfer_byte   (link_h_slave_xfer_byte),
        .h_slave_xfer_ack    (link_h_slave_xfer_ack),
        .h_cart_cycle        (link_h_cart_cycle),
        .h_standing_en       (link_h_standing_en),
        .h_slave_withdraw_pend (link_h_slave_withdraw_pend),
        .h_slave_withdraw_ack  (link_h_slave_withdraw_ack),
        .h_eng_allow         (link_h_eng_allow),
        .h_boot_alt          (link_h_boot_alt),
        .h_ev_valid          (link_h_ev_valid),
        .h_ev_type           (link_h_ev_type),
        .h_ev_byte           (link_h_ev_byte)
    );

    cart_engine u_cart_engine (
        .clk        (hClk),
        .reset      (emulator_was_reset),
        .allow      (link_h_eng_allow),
        .cpu_addr   (nw_cpu_addr),
        .cpu_do     (nw_cpu_do),
        .cpu_rd_n   (nw_cpu_rd_n),
        .cpu_wr_stb (nw_cpu_wr_stb),
        .sel        (eng_sel),
        .do_        (eng_do),
        .active     (eng_active),
        .e_a        (eng_a),
        .e_clk      (eng_clk),
        .e_cs       (eng_cs),
        .e_rd       (eng_rd),
        .e_wr       (eng_wr),
        .e_d_out    (eng_d_out),
        .e_dir_e    (eng_dir_e),
        .cart_d_in  (core_cart_d_in),
        .x_session  (xs_session),
        .x_hold     (xs_hold),
        .x_bus_req  (xs_bus_req),
        .x_bus_wr   (xs_bus_wr),
        .x_bus_a    (xs_bus_a),
        .x_bus_d    (xs_bus_d),
        .x_bus_done (xs_bus_done),
        .x_bus_q    (xs_bus_q),
        .x_tcfg     (xs_tcfg),
        .x_t_setup  (xs_t_setup),
        .x_t_pulse  (xs_t_pulse),
        .x_t_hold   (xs_t_hold),
        .x_t_access (xs_t_access),
        .x_t_gap    (xs_t_gap),
        .busy       (eng_busy),
        .n_cycles   ()
    );

    // cartspeed1: the express cart stream (cart_xs.v). Data ESP32 -> QSPI -> ring -> engine;
    // control on UART address 0x12, status on system monitor channel 12 (address 0x0C).
    wire        xs_session, xs_hold, xs_bus_req, xs_bus_wr, xs_bus_done, xs_tcfg;
    wire [15:0] xs_bus_a;
    wire  [7:0] xs_bus_d, xs_bus_q;
    wire  [4:0] xs_t_setup, xs_t_pulse, xs_t_hold, xs_t_access, xs_t_gap;
    wire        xs_g_tx_valid, xs_g_tx_taken;
    wire  [7:0] xs_g_tx_len, xs_g_tx_byte, xs_g_tx_bytepos;
    wire        xs_qspi_clk_n = ~QSPI_CLK_g;
    // 18.31 (fpgacomp1): the boot flash layout's guards (headers 0x000-0x3BF for the full design, the recovery
    // header at 0x7C0 and the recovery design 0x000800-0x05BFFF out of reach) and the flash read window
    wire        rc_wread_off;
    wire        rc_cw_ready, rc_cw_go;      // 18.31: FCOPY's words into PSRAM through the RAM cart's port
    wire [22:0] rc_cw_addr;
    wire [15:0] rc_cw_data;
    cart_xs #(.FLASH_W_BASE(24'h05C000), .FLASH_H_TOP(24'h0007C0), .HDRERASE_OK(1'b0), .HAS_FCOPY(1'b1)) u_cart_xs (
        .qspi_clk_n     (xs_qspi_clk_n),
        .q_addr_ready   (xs_q_addr_ready),
        .q_address      (xs_q_address),
        .q_byte_valid   (xs_q_byte_valid),
        .q_byte         (xs_q_byte),
        .gclk           (gClk),
        .g_reset        (~lock_o),
        .g_uart_rx_data (uart_rx_data[7:0]),
        .g_uart_rx_val  (uart_rx_val),
        .g_tx_valid     (xs_g_tx_valid),
        .g_tx_len       (xs_g_tx_len),
        .g_tx_bytepos   (xs_g_tx_bytepos),
        .g_tx_byte      (xs_g_tx_byte),
        .g_tx_taken     (xs_g_tx_taken),
        .hclk           (hClk),
        .h_reset        (emulator_was_reset),
        .h_allow        (link_h_eng_allow),
        .h_eng_busy     (eng_busy & ~xs_hold),
        .x_session      (xs_session),
        .x_hold         (xs_hold),
        .x_bus_req      (xs_bus_req),
        .x_bus_wr       (xs_bus_wr),
        .x_bus_a        (xs_bus_a),
        .x_bus_d        (xs_bus_d),
        .x_bus_done     (xs_bus_done),
        .x_bus_q        (xs_bus_q),
        .x_tcfg         (xs_tcfg),
        .x_t_setup      (xs_t_setup),
        .x_t_pulse      (xs_t_pulse),
        .x_t_hold       (xs_t_hold),
        .x_t_access     (xs_t_access),
        .x_t_gap        (xs_t_gap),
        .f_sck          (fl_sck),
        .f_cs_n         (fl_cs_n),
        .f_mosi         (fl_mosi),
        .f_miso         (FLASH_MISO),
        .r_on           (rc_on),
        .r_hold         (rc_hold),
        .r_mbc          (rc_mbc),
        .r_rom_mask     (rc_rom_mask),
        .r_ram_mask     (rc_ram_mask),
        .r_ram_has      (rc_ram_has),
        .r_rtc_has      (rc_rtc_has),
        .r_inval_tog    (rc_inval_tog),
        .r_mreset_tog   (rc_mreset_tog),
        .r_dirty_clr    (rc_dirty_clr),
        .r_dirty        (rc_dirty),
        .r_busy         (rc_busy),
        .r_wread_tog    (rc_wread_tog),
        .r_wread_addr   (rc_wread_addr),
        .r_wread_len    (rc_wread_len),
        .r_wread_done_tog(rc_wread_done_tog),
        .r_rtc_set      (rc_rtc_set),
        .r_rtc_in       (rc_rtc_in),
        .r_rtc_out      (rc_rtc_out),
        .r_dbg          (rc_dbg_g2),         // 18.32 bench: {frames, longest wait, rows the panel waited for} (was 48'd0 from 18.30)             // 18.30: the RAM cart bench tap is out of the build (it loaded the core address bus: pclk -> hclk setup)
        .r_density      (rc_density),
        .h_cart_in      (CART_DET_sr[6]),
        .q_win_addr     (q_win_addr),
        .q_fwin         (xs_fwin),
        .h_fwin_sel     (xs_fwin_sel),
        .h_wtag_on      (xs_wtag_on),
        .h_wtag         (xs_wtag),
        .r_wread_off    (rc_wread_off),
        .r_cw_ready     (rc_cw_ready),
        .r_cw_go        (rc_cw_go),
        .r_cw_addr      (rc_cw_addr),
        .r_cw_data      (rc_cw_data)
    );

    // Shiny Relay 1.1.0: the RAM cart
    ramcart u_ramcart (
        .hclk             (hClk),
        .h_on             (rc_on),
        .h_mbc            (rc_mbc),
        .h_rom_mask       (rc_rom_mask),
        .h_ram_mask       (rc_ram_mask),
        .h_ram_has        (rc_ram_has),
        .h_rtc_has        (rc_rtc_has),
        .h_inval_tog      (rc_inval_tog),
        .h_mreset_tog     (rc_mreset_tog),
        .h_busy           (rc_busy),
        .lin              (rc_lin),
        .lin_ram          (rc_lin_ram),
        .lin_a            (rc_lin_a),
        .a                (rc_a),
        .rd               (rc_rd),
        .wr               (rc_wr),
        .di               (rc_di),
        .q                (rc_q),
        .stall            (rc_stall),
        .h_dirty          (rc_dirty),
        .h_dbg            (),
        .x_diag           (rc_x_diag),
        .h_diag_run       (gg_dbg[47]),      // loaded
        .h_dirty_clr      (rc_dirty_clr),
        .h_rtc_set        (rc_rtc_set),
        .h_rtc_in         (rc_rtc_in),
        .h_rtc_out        (rc_rtc_out),
        .h_wread_tog      (rc_wread_tog),
        .h_wread_addr     (rc_wread_addr),
        .h_wread_len      (rc_wread_len),
        .h_wread_off      (rc_wread_off),
        .h_wread_done_tog (rc_wread_done_tog),
        .h_cw_ready       (rc_cw_ready),
        .h_cw_go          (rc_cw_go),
        .h_cw_addr        (rc_cw_addr),
        .h_cw_data        (rc_cw_data),
        .qclk             (xs_qspi_clk_n),
        .q_win_addr       (q_win_addr),
        .q_win_data       (rc_win_data),
        .xclk             (xClk),
        .x_req            (rc_x_req),
        .x_rnw            (rc_x_rnw),
        .x_addr           (rc_x_addr),
        .x_len            (rc_x_len),
        .x_din            (rc_x_din),
        .x_write_next     (rc_x_write_next),
        .x_done           (rc_x_done),
        .x_dout_valid     (rc_x_dout_valid),
        .x_dout           (rc_x_dout)
    );

    // Network window (NETWORK_WINDOW.md): GB CPU mailbox at FF58-FF5F <-> ESP32.
    // The UART RX byte stream is sniffed for address 0x11 next to the stock
    // packet wrapper; FPGA -> ESP32 packets go out on system monitor channel 11.
    net_window u_net_window (
        .hclk           (hClk),
        .h_reset        (emulator_was_reset),
        .h_cpu_addr     (nw_cpu_addr),
        .h_cpu_do       (nw_cpu_do),
        .h_cpu_rd_n     (nw_cpu_rd_n),
        .h_cpu_wr_stb   (nw_cpu_wr_stb),
        .h_sel          (nw_sel),
        .h_do           (nw_do),
        .gclk           (gClk),
        .g_reset        (~lock_o),
        .g_uart_rx_data (uart_rx_data[7:0]),
        .g_uart_rx_val  (uart_rx_val),
        .g_tx_valid     (nw_g_tx_valid),
        .g_tx_len       (nw_g_tx_len),
        .g_tx_bytepos   (nw_g_tx_bytepos),
        .g_tx_byte      (nw_g_tx_byte),
        .g_tx_taken     (nw_g_tx_taken),
        .g_link_busy    (link_g_net_en),
        .g_esp_awake    (nw_g_esp_awake),
        .g_unlocked     (nw_g_unlocked),
        .g_crc_errors   ()
    );

    UART2
    #(.CLK_FREQ(30'd8388608))
    u_UART2
    (
        .CLK(gClk), // clock
        .RST(~lock_o), // reset
        // UART INTERFACE
        .UART_TXD(ESP32_MCU_D11), //output
        .UART_RXD(ESP32_MCU_D12), //input
        .UART_RTS(), //output // when UART_RTS = 0, UART This Device Ready to receive.
        .UART_CTS(1'd0), //input// when UART_CTS = 0, UART Opposite Device Ready to receive.
        // UART Control Reg
        .BAUD_RATE(32'd115200), //input 32
        .PARITY_BIT(8'd0), // input 8
        .STOP_BIT(8'd0), // input 8
        .DATA_BITS(8'd8), // input 8
        // USER DATA INPUT INTERFACE
        .TX_DATA({8'd0, uart_tx_data}), //input 16
        .TX_DATA_VAL(uart_tx_val), //input 1 when TX_DATA_VAL = 1, data on TX_DATA will be transmit, DATA_SEND can set to 1 only when BUSY = 0
        .TX_BUSY(uart_tx_busy), //output when BUSY = 1 transiever is busy, you must not set DATA_SEND to 1
        // USER FIFO CONTROL INTERFACE
        .RX_DATA(uart_rx_data), //output 16
        .RX_DATA_VAL(uart_rx_val)//output
    );

    assign I2S_BCLK = menuDisabled;

endmodule
