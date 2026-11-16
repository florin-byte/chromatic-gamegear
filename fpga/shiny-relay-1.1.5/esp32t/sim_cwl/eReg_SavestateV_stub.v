// Simulation stub for the VHDL eReg_SavestateV: no savestate bus, the
// register always reads back its reset default.
module eReg_SavestateV #(
    parameter index = 0, parameter Adr = 0, parameter upper = 0, parameter lower = 0,
    parameter [63:0] def = 64'd0
) (
    input         clk,
    input  [63:0] BUS_Din,
    input  [9:0]  BUS_Adr,
    input         BUS_wren,
    input         BUS_rst,
    output [63:0] BUS_Dout,
    input  [upper:lower] Din,
    output [upper:lower] Dout
);
    assign BUS_Dout = 64'd0;
    assign Dout = def[upper:lower];
endmodule
