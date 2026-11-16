// Simulation stand-in for audio_filter.v (the real one uses Gowin DSP primitives).
module audio_filter(input reset, input clk, input [15:0] core_l, input [15:0] core_r,
                    output [15:0] filter_l, output [15:0] filter_r);
    assign filter_l = core_l;
    assign filter_r = core_r;
endmodule
