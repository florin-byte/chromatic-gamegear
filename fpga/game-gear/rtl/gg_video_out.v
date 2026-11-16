// Retimes the 64 us GG rows to the panel's 109 us row cadence. The producer
// finishes 144 rows in ~9.2 ms; scanout takes ~15.7 ms. The maximum backlog
// is below 64 rows. No framebuffer, panel, OSD or USB interface changes.
module gg_video_out(
    input pclk, hclk, reset, ce_pix, input [11:0] color, input hb, vb,
    output reg [14:0] lcd_data = 0, output reg lcd_clkena = 1,
    output reg [1:0] lcd_mode = 0, output reg lcd_vsync = 1
);
    localparam DEPTH = 10240; // 64 * 160 pixels of 12 bits, inferred dual-clock block RAM
    reg [11:0] pixels [0:DEPTH-1];
    reg pending = 0, prev_vb = 1, frame = 0;
    reg [13:0] wa = 0;
    reg [14:0] written = 0;
    // VDP color is registered on ce_pix. Blanking after that same edge belongs
    // to that color; capture on the following pclk, even if ROM then stalls.
    always @(posedge pclk) begin
        pending <= ce_pix;
        if (reset) begin pending <= 0; prev_vb <= 1; wa <= 0; written <= 0; frame <= 0; end
        else if (pending) begin
            prev_vb <= vb;
            if (prev_vb && !vb) begin frame <= !frame; written <= 0; wa <= 0; end
            if (!hb && !vb) begin
                pixels[wa] <= color;
                wa <= wa == DEPTH-1 ? 14'd0 : wa + 1'b1;
                written <= written + 1'b1;
            end
        end
    end
    // These are related, synchronous PLL clocks. Counts/payload cross only
    // through registers; producer data precedes a row-ready decision by a row.
    reg frame_h = 0, frame_seen = 0;
    reg [14:0] written_h = 0;
    reg [14:0] consumed = 0, row_target = 160;
    reg [13:0] ra = 0;
    reg [11:0] pixel_q;
    reg [7:0] col = 0, row = 144;
    reg [10:0] timer = 0;
    reg [1:0] phase = 0;
    reg active = 0;
    always @(posedge hclk) begin
        frame_h <= frame;
        written_h <= written;
        pixel_q <= pixels[ra];
        lcd_clkena <= 0;
        if (reset) begin
            frame_h <= 0; frame_seen <= 0; lcd_data <= 0;
            lcd_clkena <= 1; lcd_vsync <= 1; lcd_mode <= 0;
            ra <= 0; consumed <= 0; row <= 144; active <= 0; timer <= 0;
        end else if (frame_h != frame_seen) begin
            frame_seen <= frame_h;
            lcd_vsync <= 1; lcd_mode <= 0;
            ra <= 0; consumed <= 0; row <= 0; row_target <= 160;
            active <= 0; timer <= 0;
        end else begin
            lcd_vsync <= 0;
            if (timer != 0) timer <= timer - 1'b1;
            if (!active && row < 144 && timer == 0 && written_h >= row_target) begin
                active <= 1; col <= 0; phase <= 0; lcd_mode <= 2'b11;
                timer <= 1823; // 1824 hclk = 108.72 us between row starts
            end else if (active) begin
                phase <= phase + 1'b1;
                if (phase == 3) begin
                    lcd_data <= {pixel_q[11:8],pixel_q[11],pixel_q[7:4],pixel_q[7],pixel_q[3:0],pixel_q[3]}; lcd_clkena <= 1;
                    ra <= ra == DEPTH-1 ? 14'd0 : ra + 1'b1;
                    consumed <= consumed + 1'b1;
                    if (col == 159) begin active <= 0; row <= row + 1'b1; row_target <= row_target + 160; end
                    else col <= col + 1'b1;
                end
            end else lcd_mode <= 0;
        end
    end
`ifdef VERILATOR
    always @(posedge pclk) if (!reset && written > consumed + DEPTH)
        begin $display("FATAL: GG video buffer overflow"); $finish; end
    always @(posedge hclk) if (!reset && frame_h != frame_seen && row != 144)
        begin $display("FATAL: GG output frame did not finish before the next input frame"); $finish; end
`endif
endmodule
