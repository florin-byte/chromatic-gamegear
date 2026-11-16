// Simulation stand-ins for what vid_system_top.sv needs besides ST7785_panel_master.v / ST7785_init.v:
// the Gowin ODDR primitive and the VHDL overlays (battery, timer, debug), which are off in these tests.
module ODDR(output Q0, output Q1, input D0, input D1, input TX, input CLK);
    assign Q0 = CLK ? D0 : D1;
    assign Q1 = TX;
endmodule
module overlayBatteryFront(input [7:0] screenX, input [7:0] screenY, output overlayActive); assign overlayActive = 1'b0; endmodule
module overlayBatteryBack(input [7:0] screenX, input [7:0] screenY, output overlayActive); assign overlayActive = 1'b0; endmodule
module overlayTimerFront(input [7:0] screenX, input [7:0] screenY, output overlayActive); assign overlayActive = 1'b0; endmodule
module overlayTimerBack(input [7:0] screenX, input [7:0] screenY, output overlayActive); assign overlayActive = 1'b0; endmodule
module overlayTimerNumber(input [7:0] screenX, input [7:0] screenY, input [3:0] number, output overlayActive); assign overlayActive = 1'b0; endmodule
module overlayDebug(input [7:0] screenX, input [7:0] screenY, input [3:0] number, output overlayActive); assign overlayActive = 1'b0; endmodule
