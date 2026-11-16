// Single-port synchronous RAM (block RAM), write-first: the Game Gear work RAM and cartridge RAM.
module gg_ram #(parameter AW = 13) (
    input clk, input [AW-1:0] a, input [7:0] d, input we, output reg [7:0] q
);
    reg [7:0] mem [0:(1<<AW)-1];
    always @(posedge clk) begin
        if (we) begin mem[a] <= d; q <= d; end
        else q <= mem[a];
    end
endmodule
