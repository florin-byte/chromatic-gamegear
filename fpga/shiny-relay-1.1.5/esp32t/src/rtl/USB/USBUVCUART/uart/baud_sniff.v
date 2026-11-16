// baud_sniff.v (cartspeed1, 18.27): the USB bridge follows esptool's CHANGE_BAUDRATE on its
// ESP32 side, so a host that cannot change its own rate on an open port (Web Serial: a reopen
// sets DTR + RTS, which resets the ESP32 here) still gets the flash stub at a fast rate. USB CDC
// has no real rate on the host side: only the bridge's UART towards the ESP32 changes.
//
//   host -> ESP32: a SLIP frame decoding to exactly 16 bytes: 00 0F 08 00 <chk 4> <new baud 4 LE> <old baud 4>
//                  (request, op CHANGE_BAUDRATE, size 8): the new rate is pending (dropped after ~2 s)
//   ESP32 -> host: a SLIP frame decoding to 10..16 bytes starting 01 0F (the response): at its
//                  closing C0 (status byte, decoded byte 8, = 0) the bridge switches to the pending rate (a supported one only)
//   back to the host's line coding (SET_LINE_CODING) on reset, on any change of DTR/RTS (every
//   port open / close, every ESP32 reset), while RTS is on (the ESP32 held in reset), and by a
//   CHANGE_BAUDRATE to 115200.
// Everything in pClk. One byte per *_val pulse (usb_fifo's dval; the UART's RX_DATA_VAL).
module baud_slip_dec (
    input            clk,
    input            rst,
    input            val,
    input      [7:0] din,
    output reg       fend,         // pulse: a frame ended (its closing C0); flen and the fields below are its own
    output reg [4:0] flen,         // decoded bytes of that frame, saturates at 31
    output reg [7:0] b0, b1, b2, b3,
    output reg [31:0] w2           // decoded bytes 8..11, little endian
);
    reg       esc;
    reg [4:0] len;
    wire [7:0] d = esc ? (din == 8'hDC ? 8'hC0 : din == 8'hDD ? 8'hDB : din) : din;
    always @(posedge clk) begin
        fend <= 1'b0;
        if (rst) begin
            esc <= 1'b0; len <= 5'd0; flen <= 5'd0;
        end else if (val) begin
            if (din == 8'hC0) begin
                if (len != 5'd0) begin fend <= 1'b1; flen <= len; end
                len <= 5'd0; esc <= 1'b0;
            end else if (!esc && din == 8'hDB) begin
                esc <= 1'b1;
            end else begin
                esc <= 1'b0;
                case (len)
                    5'd0: b0 <= d;
                    5'd1: b1 <= d;
                    5'd2: b2 <= d;
                    5'd3: b3 <= d;
                    5'd8: w2[7:0] <= d;
                    5'd9: w2[15:8] <= d;
                    5'd10: w2[23:16] <= d;
                    5'd11: w2[31:24] <= d;
                    default: ;
                endcase
                if (len != 5'd31) len <= len + 5'd1;
            end
        end
    end
endmodule

module baud_sniff #(
    parameter [26:0] PEND_CYCLES = 27'd120000000      // ~2 s at 60 MHz
)(
    input             clk,
    input             rst,
    input             h_val,          // host -> ESP32 byte
    input      [7:0]  h_byte,
    input             e_val,          // ESP32 -> host byte
    input      [7:0]  e_byte,
    input      [1:0]  ctl_sig,        // {RTS, DTR}
    output reg        ovr_on,
    output reg [31:0] ovr_rate
);
    wire       hf, ef;
    wire [4:0] h_flen, e_flen;
    wire [7:0] h0, h1, h2, h3, e0, e1, e2, e3;
    wire [31:0] hw, ew;
    baud_slip_dec u_h (.clk(clk), .rst(rst), .val(h_val), .din(h_byte), .fend(hf), .flen(h_flen), .b0(h0), .b1(h1), .b2(h2), .b3(h3), .w2(hw));
    baud_slip_dec u_e (.clk(clk), .rst(rst), .val(e_val), .din(e_byte), .fend(ef), .flen(e_flen), .b0(e0), .b1(e1), .b2(e2), .b3(e3), .w2(ew));

    function supported(input [31:0] r);
        supported = r == 32'd115200 || r == 32'd460800 || r == 32'd921600 || r == 32'd1500000 || r == 32'd2000000;
    endfunction

    wire is_cmd  = hf && h_flen == 5'd16 && h0 == 8'h00 && h1 == 8'h0F && h2 == 8'h08 && h3 == 8'h00;
    wire is_resp = ef && e_flen >= 5'd10 && e_flen <= 5'd16 && e0 == 8'h01 && e1 == 8'h0F && ew[7:0] == 8'h00;   // status 0 (stub and ROM: the first data byte)

    reg [1:0]  ctl_q;
    reg        pend;
    reg [31:0] pend_rate;
    reg [26:0] pend_t;
    wire clear = rst || (ctl_sig != ctl_q) || ctl_sig[1];
    always @(posedge clk) begin
        ctl_q <= ctl_sig;
        if (clear) begin
            ovr_on <= 1'b0; ovr_rate <= 32'd115200; pend <= 1'b0; pend_t <= 27'd0;
        end else begin
            if (pend) begin
                if (pend_t == PEND_CYCLES) pend <= 1'b0; else pend_t <= pend_t + 27'd1;
            end
            if (is_cmd) begin
                pend <= supported(hw); pend_rate <= hw; pend_t <= 27'd0;
            end else if (is_resp && pend) begin
                pend <= 1'b0;
                ovr_rate <= pend_rate;
                ovr_on <= pend_rate != 32'd115200;
            end
        end
    end
endmodule
