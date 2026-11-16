module ST7785_panel_master(
    input               appear_off,
    input               gClk,
    input               nRST,

    input               hClk,
    input       [17:0]  hColorPixel,
    input               lcd_on,
    input               hVsync,
    input               hHsync,
    input               hValid,
    input               LCD_EN,
    output  reg         LCD_DE,
    output  reg         LCD_HSYNC,
    output  reg         LCD_VSYNC,
    output  reg         LCD_GENLOCK,
    output  reg [5:0]   LCD_DB,
    // 18.32: rows the panel waited for (each one was a shaken row before) and the longest wait in gClk cycles
    output  reg [15:0]  gWaitRows = 16'd0,
    output  reg [7:0]   gEndHolds = 8'd0,   // 18.32 bench: panel frames that held for the core's next frame
    output  reg [7:0]   gFrames = 8'd0,     // 18.32 bench: panel frames that ran out before the core's (an extra VSYNC)
    output  reg [7:0]   hBadRows = 8'd0     // 18.32 bench: core rows that did not bring 160 pixels
);
    
    parameter       H_Lw             = 16'd30; 
    parameter       H_Pixel_Valid    = 16'd720; 
    parameter       H_FrontPorch     = 16'd129;
    parameter       H_BackPorch      = 16'd32;  

    parameter       PixelForHS       = H_Lw + H_Pixel_Valid + H_FrontPorch + H_BackPorch;
    
    parameter       V_Lw             = 16'd2;
    parameter       V_Pixel_Valid    = 16'd144;
    parameter       V_FrontPorch     = 16'd2; // was 64
    parameter       V_BackPorch      = 16'd10;

    parameter       PixelForVS       = V_Pixel_Valid + V_FrontPorch + V_BackPorch;

    reg                 gLcdOn_r1;
    reg                 gLcdOn_r2;
    always@(posedge gClk)
    begin
        gLcdOn_r1 <= lcd_on;
        gLcdOn_r2 <= gLcdOn_r1;
    end

    reg                 gVs_r1;
    reg                 gVs_r2;
    always@(posedge gClk)
    begin
        gVs_r1 <= hVsync;
        gVs_r2 <= gVs_r1;
    end

    reg hgVs_r1;
    reg hgVs_r2;
    always@(posedge hClk)
    begin
        hgVs_r1 <= hVsync;
        hgVs_r2 <= hgVs_r1;
    end
    wire hGbVsync = hgVs_r1 & ~hgVs_r2;
    
    reg [15:0] hs_sr;
    always@(posedge hClk)
        hs_sr <= {hs_sr[14:0], hHsync};
    wire gbhsync = hs_sr[15] & ~hs_sr[14];
    reg valid_r1;
    always@(posedge hClk)
        valid_r1 <= hValid;

    // 18.32 (owner 2026-10-27, players' reports: a game played from PSRAM shook on the panel, Super Mario Land on moving
    // platforms, the menu too): the scanout below runs on its own clock, about 4 rows behind the core, locked to the
    // core's frame start only. A game in PSRAM stops the core on every cache miss (speedcontrol stall); when the stops
    // in one frame add up to ~4 rows, the scanout passed the rows still being written and showed the ring's old bytes
    // (rows shifted / doubled). Now a row's scanout waits until the core has finished that row. A cartridge never
    // stalls, so its rows are always there: nothing changes for it.
    reg [7:0] hRowsDone = 8'd0;
    wire hRowEnd;                       // a core row is in the ring (its 160th pixel, or a short row closed)
    always@(posedge hClk)
        if (hGbVsync) hRowsDone <= 8'd0;
        else if (hRowEnd && hRowsDone != 8'd255) hRowsDone <= hRowsDone + 1'd1;
    reg [7:0] hRowsGray = 8'd0;
    always@(posedge hClk) hRowsGray <= hRowsDone ^ (hRowsDone >> 1);
    reg [7:0] gRowsGray_r1 = 8'd0, gRowsGray_r2 = 8'd0;
    always@(posedge gClk) begin gRowsGray_r1 <= hRowsGray; gRowsGray_r2 <= gRowsGray_r1; end
    reg [7:0] gRowsDone;
    integer gi;
    always@(*) begin
        gRowsDone[7] = gRowsGray_r2[7];
        for (gi = 6; gi >= 0; gi = gi - 1) gRowsDone[gi] = gRowsDone[gi + 1] ^ gRowsGray_r2[gi];
    end
        

    wire [17:0] color_pixelp;
    
    localparam DEPTH = 1024;
    reg [17:0] lineBuffer [DEPTH-1:0];
    reg [17:0]  lineBuffer_q;

    reg [9:0]   lineBuffer_wa;
    reg [7:0]   lineBuffer_wrCount;
    reg [9:0]   lineBuffer_ra;
    reg [7:0]   lineBuffer_rdCount;
    
    // 18.32 (players' reports: a game played from PSRAM shook sideways by 1-2 pixels below some row): a row used to
    // end at the core's mode-3 fall + 16 hClk, but the row's last pixel leaves the core one core step after that fall.
    // A RAM cart stall in between (it stops the core, not hClk) brought that pixel after the row's end: it opened the
    // next row, whose own last pixel was then dropped, and every row below sat one pixel to the side until the next
    // frame. Now a row ends with its 160th pixel; the mode-3 fall only closes a row that really came short (a core held
    // mid-row), and pads its place in the ring so the rows below stay where they belong.
    wire [7:0]  hShortBy = 8'd160 - lineBuffer_wrCount;
    wire        hShortRow = gbhsync && (lineBuffer_wrCount != 8'd0) && (lineBuffer_wrCount < 8'd152);
    wire [10:0] hWaPad = {1'b0, lineBuffer_wa} + {3'd0, hShortBy};
    assign hRowEnd = (hValid && (lineBuffer_wrCount == 8'd159)) || hShortRow;
    always@(posedge hClk)
    begin
        if(hGbVsync)
        begin
            lineBuffer_wrCount <= 'd0;
            lineBuffer_wa      <= 'd0;
        end
        else
            if(hValid)
            begin
                lineBuffer_wrCount  <= (lineBuffer_wrCount == 8'd159) ? 8'd0 : lineBuffer_wrCount + 1'd1;
                if(lineBuffer_wa < DEPTH - 1'd1)
                    lineBuffer_wa       <= lineBuffer_wa + 1'd1;
                else
                    lineBuffer_wa       <= 'd0;
            end
            else
                if(gbhsync && lineBuffer_wrCount != 8'd0)
                begin
                    hBadRows <= hBadRows + 1'd1;    // 18.32 bench (wraps): rows whose end came before their last pixel
                    if (hShortRow)
                    begin
                        lineBuffer_wrCount <= 'd0;
                        lineBuffer_wa      <= (hWaPad >= DEPTH) ? hWaPad - DEPTH : hWaPad[9:0];
                    end
                end
    end
    
    reg         [11:0]  H_PixelCount;
    reg         [11:0]  V_PixelCount;
    reg         [15:0]  frameCount;
    wire  LCD_DEI =    ( H_PixelCount > H_BackPorch + H_Lw ) && ( H_PixelCount <= H_Pixel_Valid + H_BackPorch + H_Lw ) && (V_PixelCount < V_Pixel_Valid + V_Lw + V_FrontPorch) && (V_PixelCount >= V_Lw + V_FrontPorch);      

    always@(posedge hClk)
        if(hValid)
            lineBuffer[lineBuffer_wa]  <=  hColorPixel;

    reg [1:0] phase;
    reg [7:0] hoffset;
    localparam OFFSET = 41;
    reg pGbVsync;
    reg pGbVsync_r1;
    wire pGbVsyncRising = pGbVsync & ~pGbVsync_r1;
    always@(posedge gClk)
    begin
        pGbVsync <= hVsync;
        pGbVsync_r1 <= pGbVsync;
    end
    
    always@(posedge gClk)
    begin
        if(pGbVsyncRising)
        begin
            lineBuffer_ra       <= 'd0;
            lineBuffer_rdCount  <= 'd0;
            phase               <= 'd2;
        end
        else
            if(~LCD_HSYNC)
            begin
                phase <= 'd2;
                hoffset <= 'd0;
                lineBuffer_rdCount <= 'd0;
            end
            else
            begin
                if(phase < 3'd2)
                    phase <= phase + 1'd1;
                else
                    phase <= 'd0;

                if((LCD_DEI) && LCD_VSYNC && (phase == 'd2))
                    if(hoffset < OFFSET)
                        hoffset <= hoffset + 1'd1;
                    
                if((LCD_DEI) && (V_PixelCount >= V_Lw + V_FrontPorch) && (phase == 'd1))
                begin
                    if(hoffset >= OFFSET)
                        if(lineBuffer_rdCount <= 8'd159)
                        begin
                            if(lineBuffer_ra < DEPTH - 1'd1)
                                lineBuffer_ra <= lineBuffer_ra + 1'd1;
                            else
                                lineBuffer_ra <= 'd0;
                            lineBuffer_rdCount <= lineBuffer_rdCount + 1'd1;
                        end
                end
            end
    end
                
    always@(posedge gClk)
        lineBuffer_q <= lineBuffer[lineBuffer_ra];

    assign color_pixelp = lineBuffer_q[17:0];

    localparam FINE_OFFSET = 11'd418;
    reg [10:0] fineDelay;
    // LCD scanout is several rows behind the emulator
    // Need to delay the falling edge of lcd_on
    //
    // When we get falling edge of lcd_on, scanout remaining rows
    // then enter 
    localparam [15:0] WAIT_MAX = 16'd20000;    // ~22 rows of the panel's line time
    reg [15:0] gWait = 16'd0;
    reg gGiveUp = 1'b0;                         // a core that stopped (paused, held for a load): no more waits this frame
    wire [11:0] nextV = V_PixelCount + 1'b1;
    // the panel's frame (158 rows) ran out before the core's next frame start (154 rows + its stalls): hold the last
    // porch row until it comes (before 18.32 the panel started a frame of its own, cut a moment later: two VSYNCs)
    wire endWanted = (nextV >= PixelForVS + V_Lw);
    wire rowWanted = (nextV >= V_Lw + V_FrontPorch) && (nextV < V_Pixel_Valid + V_Lw + V_FrontPorch);
    wire [7:0] nextRow = nextV - (V_Lw + V_FrontPorch);
    reg lcd_on_aligned;
    reg lcd_on_aligned_r1;
    reg lcd_on_aligned_r2;
    wire frameDone = (V_PixelCount >= V_Pixel_Valid + V_Lw + V_FrontPorch)&&~LCD_HSYNC;    
    always@(posedge gClk)
        if((gVs_r1 & ~gVs_r2)|(lcd_on_aligned_r1 & ~lcd_on_aligned_r2))
            fineDelay <= 'd0;
        else
            if(fineDelay != FINE_OFFSET)
                fineDelay <= fineDelay + 1'd1;

    reg delayed;
    always@(posedge gClk)
        delayed <= (fineDelay == (FINE_OFFSET - 1'd1)) ? 1'd1 : 1'd0;

    always@(posedge gClk)
    begin
        if((V_PixelCount == 0) || frameDone)
        lcd_on_aligned <= lcd_on;
        lcd_on_aligned_r1 <= lcd_on_aligned;
        lcd_on_aligned_r2 <= lcd_on_aligned_r1;
    end

    always @(posedge gClk)begin
        if( !nRST || delayed || ~lcd_on_aligned) begin
            V_PixelCount      <=  0;
            H_PixelCount      <=  'd0;
            frameCount        <=  16'd0;
            if (gWait != 0 && endWanted) gEndHolds <= gEndHolds + 1'd1;   // the hold ends here
            gWait             <=  'd0;
            gGiveUp           <=  1'b0;
        end
        else 
            if(  H_PixelCount == PixelForHS )
            begin
                // the next row (V + 1) shows core row V + 1 - (V_Lw + V_FrontPorch): wait (HSYNC high, DE low) until the
                // core has finished it; at most WAIT_MAX cycles (a core held for a load must not stop the panel)
                if (~gGiveUp && (gWait != WAIT_MAX) && ((rowWanted && (gRowsDone < nextRow + 8'd1)) || endWanted))
                begin
                    gWait <= gWait + 1'd1;
                end
                else
                begin
                    if (gWait == WAIT_MAX) gGiveUp <= 1'b1;
                    if (gWait != 0) begin
                        if (endWanted) gEndHolds <= gEndHolds + 1'd1;
                        else gWaitRows <= gWaitRows + 1'd1;
                    end
                    gWait             <=  'd0;
                    V_PixelCount      <=  V_PixelCount + 1'b1;
                    H_PixelCount      <=  'd0;
                end
            end
        else if(  V_PixelCount >= (PixelForVS + V_Lw) ) begin
            V_PixelCount      <=  'd0;
            H_PixelCount      <=  'd0;
            frameCount        <=  frameCount + 1'd1;
            gFrames <= gFrames + 1'd1;
            end
        else begin
            H_PixelCount      <=  H_PixelCount + 1'b1;
        end
    end
      
    reg LCD_DEI_r1;
    reg LCD_DEI_r2;
    reg LCD_VSYNC_r1;
    localparam BLACK = 0;
    always@(posedge gClk)
    begin
        LCD_DEI_r1 <= LCD_DEI;
        LCD_DEI_r2 <= LCD_DEI_r1;
        LCD_DE <= LCD_DEI_r2;
        LCD_HSYNC <= H_PixelCount < H_Lw ? 1'b0 : 1'b1;
        LCD_VSYNC <= (V_PixelCount >= 0)&&(V_PixelCount < V_Lw)  ? 1'b0 : 1'b1;
        LCD_VSYNC_r1 <= LCD_VSYNC;
        if(LCD_VSYNC & ~LCD_VSYNC_r1)
            LCD_GENLOCK <= ~LCD_GENLOCK;

        if((hoffset >= OFFSET) && lcd_on_aligned)
        begin
            if(LCD_EN)
            begin
                // when the unit should appear off during an emulator core reset or
                // when the FPGA is powered with switch off, want to keep LCD running
                // like normal (to prevent weird effects) but look off.  Writing
                // black pixels and turning backlight off seems to do the trick.  
                // Could possibly do this in a higher level module, but don't
                // want to get signals out of sync.
                if(phase == 2)
                    LCD_DB <= appear_off ? BLACK : color_pixelp[17:12]; // Blue
                if(phase == 1)
                    LCD_DB <= appear_off ? BLACK : color_pixelp[11:6]; // Green
                if(phase == 0)
                    LCD_DB <= appear_off ? BLACK : color_pixelp[5:0]; // Red

            end
            else
            begin
                LCD_DB          <= {6{1'b1}};
            end
        end
        else
        begin
            LCD_DB <= 'd0;
        end
    end

endmodule