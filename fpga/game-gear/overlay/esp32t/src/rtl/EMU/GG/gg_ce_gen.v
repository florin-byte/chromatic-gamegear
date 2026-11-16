// Fractional VDP clock enable from the 2^25 Hz pclk.
//
// The Chromatic's panel has no frame clock of its own: ST7785_panel_master restarts its scan at every
// frame start of the core and expects it exactly every 154 panel rows (154 * 1824 hclk = 561792 pclk,
// the Game Boy's 59.73 Hz). A frame start that comes early cuts a panel row short and the picture rolls.
// A Game Gear frame is 262 lines * 342 pixels * 2 VDP ticks = 179208 ticks, so the tick rate is
// 179208 / 561792 = 393 / 1232 of pclk: 10.7036 MHz, 0.32 % below the real 10.7386 MHz.
// Ticks are 3 or 4 pclk apart.
module gg_ce_gen(input clk, reset, run, hold, output ce_vdp, ce_pix, ce_cpu, ce_sp, ce_snd);
    reg [10:0] acc = 0;
    reg [2:0] phase = 0;
    reg sprite = 0;
    wire [11:0] sum = {1'b0, acc} + 12'd393;
    wire tick = sum >= 12'd1232;
    wire [11:0] wrapped = sum - 12'd1232;
    // CG-106: `hold` (the RAM cart has no byte for the address yet) stops the enables only while the next tick
    // is a CPU tick. The address changes at a CPU tick, a cache hit answers about 4 pclk later and the next CPU
    // tick is 3 ticks (about 9 pclk) away, so a hit costs no time; a miss waits at the CPU tick.
    wire go = run && !(hold && (phase == 0 || phase == 3));
    assign ce_vdp = go && tick;
    // phase 0 is tick 1, as in HandheldSms.
    assign ce_pix = ce_vdp && !phase[0];
    assign ce_cpu = ce_vdp && (phase == 0 || phase == 3);
    assign ce_sp = go && sprite;
    always @(posedge clk) begin
        if (reset) begin acc <= 0; phase <= 0; sprite <= 0; end
        else if (go) begin
            acc <= tick ? wrapped[10:0] : sum[10:0];
            sprite <= !sprite;
            if (tick) phase <= phase == 5 ? 3'd0 : phase + 1'b1;
        end
    end
    // CG-106: the sound chip's enable, the rate of ce_cpu but never stopped by `run`. A RAM cart cache miss
    // stops the core's enables for a few microseconds; with the PSG on ce_cpu its tone periods stretched
    // irregularly (lower and rough tones on the unit). The CPU's writes reach jt89 on an edge of wr_n in
    // every clock, so the two enables need no common phase.
    reg [10:0] sacc = 0;
    reg [2:0] sphase = 0;
    wire [11:0] ssum = {1'b0, sacc} + 12'd393;
    wire stick = ssum >= 12'd1232;
    wire [11:0] swrapped = ssum - 12'd1232;
    assign ce_snd = stick && (sphase == 0 || sphase == 3);
    always @(posedge clk) begin
        if (reset) begin sacc <= 0; sphase <= 0; end
        else begin
            sacc <= stick ? swrapped[10:0] : ssum[10:0];
            if (stick) sphase <= sphase == 5 ? 3'd0 : sphase + 1'b1;
        end
    end
endmodule
