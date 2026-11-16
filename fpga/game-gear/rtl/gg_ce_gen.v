// Fractional VDP clock enable at 10.738635 MHz from the 2^25 Hz pclk.
module gg_ce_gen(input clk, reset, run, output ce_vdp, ce_pix, ce_cpu, ce_sp);
    reg [24:0] acc = 0;
    reg [2:0] phase = 0;
    reg sprite = 0;
    wire [25:0] sum = {1'b0, acc} + 26'd10738635;
    assign ce_vdp = run && sum[25];
    // phase 0 is tick 1, as in HandheldSms.
    assign ce_pix = ce_vdp && !phase[0];
    assign ce_cpu = ce_vdp && (phase == 0 || phase == 3);
    assign ce_sp = run && sprite;
    always @(posedge clk) begin
        if (reset) begin acc <= 0; phase <= 0; sprite <= 0; end
        else if (run) begin
            acc <= sum[24:0];
            sprite <= !sprite;
            if (sum[25]) phase <= phase == 5 ? 3'd0 : phase + 1'b1;
        end
    end
endmodule
