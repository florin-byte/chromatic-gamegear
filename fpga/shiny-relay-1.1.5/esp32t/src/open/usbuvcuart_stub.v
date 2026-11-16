// usbuvcuart_stub.v
//
// Open-source build stand-in for usbuvcuart_top, whose USB PHY and device
// controller are Gowin-encrypted IP. USB is absent: every USB pin, including
// the D+ pull-up enable, is high-Z so the host sees no device, and
// usblocked = 0 holds top.v's ESP32 bridge in
// its idle state (ESP32_EN and ESP32_IO0 high: the ESP32 runs normally,
// UART0 lines idle high).

module usbuvcuart_top #(parameter DEFAULT_SCALE_2X=1'b1)(
    input               CLK_24MHz,
    input               ERST,
    output              pClk,
    output              usblocked,
    input               hClk,
    input               video_clk,
    input               video_valid,
    input               video_frame_start,
    input   [17:0]      video_pixel,
    input               video_line_end,

    input   [7:0]       playerNum,

    output              UART_TXD   ,
    input               UART_RXD   ,
    output              E_UART_DTR   ,
    output              E_UART_RTS   ,
    input               UART_CTS   ,

    input [15:0]        left,
    input [15:0]        right,

    output  [7:0]       debugs,
    inout               usb_dxp_io,
    inout               usb_dxn_io,
    input               usb_rxdp_i,
    input               usb_rxdn_i,
    output              usb_pullup_en_o,
    inout               usb_term_dp_io,
    inout               usb_term_dn_io
);
    assign pClk            = 1'b0;      // its flops sit in reset (usblocked = 0)
    assign usblocked       = 1'b0;
    assign UART_TXD        = 1'b1;
    assign E_UART_DTR      = 1'b1;
    assign E_UART_RTS      = 1'b1;
    assign debugs          = 8'd0;
    assign usb_pullup_en_o = 1'bz;
    assign usb_dxp_io      = 1'bz;
    assign usb_dxn_io      = 1'bz;
    assign usb_term_dp_io  = 1'bz;
    assign usb_term_dn_io  = 1'bz;
endmodule
