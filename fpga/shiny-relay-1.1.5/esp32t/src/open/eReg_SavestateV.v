// eReg_SavestateV.v
//
// Verilog port of the VHDL eReg_SavestateV (rtl/EMU/CORE/bus_savestates.vhd)
// for the open-source flow: Verilog instances pass per-instance generics,
// which the GHDL synthesis plugin cannot take from a Verilog parent.
// Behaviour is identical: a savestate-bus-loadable register.

module eReg_SavestateV #(
    parameter integer index = 0,
    parameter integer Adr   = 0,
    parameter integer upper = 0,
    parameter integer lower = 0,
    parameter [63:0]  def   = 64'd0
) (
    input                    clk,
    input      [63:0]        BUS_Din,
    input      [9:0]         BUS_Adr,
    input                    BUS_wren,
    input                    BUS_rst,
    output     [63:0]        BUS_Dout,
    input      [upper:lower] Din,
    output     [upper:lower] Dout
);
    localparam [9:0] AdrI = Adr + index;

    reg [upper:lower] Dout_buffer = def[upper:lower];

    always @(posedge clk) begin
        if (BUS_rst)
            Dout_buffer <= def[upper:lower];
        else if (BUS_Adr == AdrI && BUS_wren)
            Dout_buffer <= BUS_Din[upper:lower];
    end

    assign Dout = Dout_buffer;

    wire [63:0] din_zext = Din;
    assign BUS_Dout = (BUS_Adr == AdrI) ? (din_zext << lower) : 64'd0;
endmodule
