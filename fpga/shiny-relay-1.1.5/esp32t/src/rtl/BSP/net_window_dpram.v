// net_window_dpram.v
//
// Simple dual-port RAM for the network window buffers: one write port and one
// read port, each on its own clock (hClk GB core / gClk system monitor). The
// read port is registered (one clock latency), which maps onto a Gowin SDPB
// BSRAM with independent CLKA / CLKB (2 KiB per 18 Kbit block in x8 mode).
//
// No reset and no read-during-write rules are relied on: the network window
// hands each buffer from one side to the other with a toggle handshake, so the
// two ports never touch the same buffer at the same time (see NETWORK_WINDOW.md,
// "Buffer ownership").

module net_window_dpram #(
    parameter AW = 11
) (
    input               wclk,
    input               we,
    input      [AW-1:0] waddr,
    input        [7:0]  wdata,
    input               rclk,
    input      [AW-1:0] raddr,
    output reg   [7:0]  rdata
);
    reg [7:0] mem [0:(1<<AW)-1];

    always @(posedge wclk)
        if (we) mem[waddr] <= wdata;

    always @(posedge rclk)
        rdata <= mem[raddr];
endmodule
