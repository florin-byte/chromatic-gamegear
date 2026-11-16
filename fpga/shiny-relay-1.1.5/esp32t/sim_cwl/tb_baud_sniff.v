// tb_baud_sniff.v (cartspeed1 18.27): the USB bridge's CHANGE_BAUDRATE follower (uart/baud_sniff.v)
// and the rate table of the bridge UART (uart/uart.v, its VHDL TX/RX replaced by stubs that expose
// the divider: no VHDL simulator here).
//   iverilog -g2012 -o tbb tb_baud_sniff.v ../src/rtl/USB/USBUVCUART/uart/baud_sniff.v ../src/rtl/USB/USBUVCUART/uart/uart.v && vvp tbb
`timescale 1ns/1ps
module UART_TX(input CLK, input RST, output UART_TXD, input [7:0] PARITY_BIT, input [7:0] STOP_BIT, input [7:0] DATA_BITS,
               input [23:0] DIVIDER_VALUE, input [1:0] DIV_CORRECT, input [15:0] DATA_IN, input DATA_SEND, output BUSY);
    assign UART_TXD = 1'b1; assign BUSY = 1'b0;
endmodule
module UART_RX #(parameter P_PARITY_BIT = "odd")(input CLK, input RST, input UART_RXD, input [7:0] PARITY_BIT, input [7:0] STOP_BIT,
               input [7:0] DATA_BITS, input [23:0] DIVIDER_VALUE, input [1:0] DIV_CORRECT, output [15:0] DATA_OUT, output DATA_VLD, output FRAME_ERROR);
    assign DATA_OUT = 16'd0; assign DATA_VLD = 1'b0; assign FRAME_ERROR = 1'b0;
endmodule

module tb_baud_sniff;
    reg clk = 0;
    always #8.333 clk = ~clk;                       // 60 MHz
    reg rst = 1;
    reg h_val = 0, e_val = 0;
    reg [7:0] h_byte = 0, e_byte = 0;
    reg [1:0] ctl = 2'b00;
    wire ovr_on;
    wire [31:0] ovr_rate;
    baud_sniff #(.PEND_CYCLES(27'd2000)) dut (.clk(clk), .rst(rst), .h_val(h_val), .h_byte(h_byte), .e_val(e_val), .e_byte(e_byte),
        .ctl_sig(ctl), .ovr_on(ovr_on), .ovr_rate(ovr_rate));
    wire [31:0] line_rate = 32'd115200;
    wire [31:0] rate = ovr_on ? ovr_rate : line_rate;

    integer ok = 0, bad = 0;
    task check(input cond, input [8*64-1:0] what);
        begin if (cond) ok = ok + 1; else begin bad = bad + 1; $display("FAIL: %0s (ovr_on %0d rate %0d)", what, ovr_on, ovr_rate); end end
    endtask

    // raw bytes, one valid pulse each, a few clocks apart (the USB FIFO / UART pace)
    task hraw(input [7:0] b); begin @(posedge clk); h_byte <= b; h_val <= 1; @(posedge clk); h_val <= 0; repeat (3) @(posedge clk); end endtask
    task eraw(input [7:0] b); begin @(posedge clk); e_byte <= b; e_val <= 1; @(posedge clk); e_val <= 0; repeat (3) @(posedge clk); end endtask
    // SLIP-escaped payload byte
    task hb(input [7:0] b); begin if (b == 8'hC0) begin hraw(8'hDB); hraw(8'hDC); end else if (b == 8'hDB) begin hraw(8'hDB); hraw(8'hDD); end else hraw(b); end endtask
    task eb(input [7:0] b); begin if (b == 8'hC0) begin eraw(8'hDB); eraw(8'hDC); end else if (b == 8'hDB) begin eraw(8'hDB); eraw(8'hDD); end else eraw(b); end endtask
    task hw32(input [31:0] w); begin hb(w[7:0]); hb(w[15:8]); hb(w[23:16]); hb(w[31:24]); end endtask

    // esptool CHANGE_BAUDRATE: C0 | 00 0F 08 00 | chk 4 | new 4 | old 4 | C0
    task cmd(input [7:0] op, input [15:0] size, input [31:0] newb, input [31:0] oldb, input integer extra, input [31:0] chk);
        integer i;
        begin
            hraw(8'hC0); hb(8'h00); hb(op); hb(size[7:0]); hb(size[15:8]); hw32(chk); hw32(newb); hw32(oldb);
            for (i = 0; i < extra; i = i + 1) hb(8'h55);
            hraw(8'hC0);
        end
    endtask
    // response: C0 | 01 op size16 | val 4 | data (n bytes: status first) | C0
    task resp(input [7:0] op, input integer n, input [7:0] status);
        integer i;
        begin
            eraw(8'hC0); eb(8'h01); eb(op); eb(n[7:0]); eb(8'h00); eb(8'h00); eb(8'hC0); eb(8'hDB); eb(8'h00);
            eb(status); for (i = 1; i < n; i = i + 1) eb(8'h00);
            eraw(8'hC0);
        end
    endtask

    // the bridge UART's rate table (divider = floor(60e6*32/baud)[28:5])
    reg [31:0] u_rate = 32'd115200;
    UART #(.CLK_FREQ(30'd60000000)) u_uart (.CLK(clk), .RST(rst), .UART_TXD(), .UART_RXD(1'b1), .UART_RTS(), .UART_CTS(1'b0),
        .BAUD_RATE(u_rate), .PARITY_BIT(8'd0), .STOP_BIT(8'd0), .DATA_BITS(8'd8), .TX_DATA(16'd0), .TX_DATA_VAL(1'b0), .TX_BUSY(),
        .RX_DATA(), .RX_DATA_VAL());
    task divchk(input [31:0] r, input [23:0] want, input [1:0] corr);
        begin
            u_rate = r; repeat (3) @(posedge clk);
            check(u_uart.divider_value == want && u_uart.div_correct == corr, "uart divider");
            if (!(u_uart.divider_value == want)) $display("  rate %0d: divider %0d corr %0d (want %0d)", r, u_uart.divider_value, u_uart.div_correct, want);
        end
    endtask

    initial begin
        repeat (5) @(posedge clk); rst <= 0; repeat (5) @(posedge clk);
        check(!ovr_on && rate == 115200, "idle: host rate");

        // 1. a plain switch to 921600: nothing until the response's closing C0
        cmd(8'h0F, 16'd8, 32'd921600, 32'd115200, 0, 0);
        repeat (5) @(posedge clk);
        check(!ovr_on, "command alone does not switch");
        eraw(8'hC0); eb(8'h01); eb(8'h0F); eb(8'h02); eb(8'h00); eb(0); eb(0); eb(0); eb(0); eb(8'h00); eb(8'h00);
        repeat (5) @(posedge clk);
        check(!ovr_on, "response without its closing C0 does not switch");
        eraw(8'hC0); repeat (3) @(posedge clk);
        check(ovr_on && rate == 921600, "switched to 921600 at the closing C0");

        // 2. a ctl change (port close / open) clears it
        ctl <= 2'b01; repeat (3) @(posedge clk);
        check(!ovr_on && rate == 115200, "DTR change clears");
        // 3. 2000000 with escapes in the frame (checksum field holds C0 and DB), stub response (10 bytes)
        cmd(8'h0F, 16'd8, 32'd2000000, 32'd115200, 0, 32'hDBC0_C0DB);
        resp(8'h0F, 2, 8'h00);
        repeat (3) @(posedge clk);
        check(ovr_on && rate == 2000000, "2000000 through escapes");
        // 4. RTS on (the ESP32 held in reset) clears, and holds it cleared
        ctl <= 2'b11; repeat (3) @(posedge clk);
        check(!ovr_on, "RTS clears");
        cmd(8'h0F, 16'd8, 32'd921600, 32'd115200, 0, 0); resp(8'h0F, 2, 8'h00); repeat (3) @(posedge clk);
        check(!ovr_on, "no switch while RTS is on");
        ctl <= 2'b01; repeat (3) @(posedge clk);
        // 5. ROM-style response (12 bytes) to 1500000
        cmd(8'h0F, 16'd8, 32'd1500000, 32'd0, 0, 0); resp(8'h0F, 4, 8'h00); repeat (3) @(posedge clk);
        check(ovr_on && rate == 1500000, "1500000 on a ROM-length response");
        // 6. back to 115200 by CHANGE_BAUDRATE
        cmd(8'h0F, 16'd8, 32'd115200, 32'd1500000, 0, 0); resp(8'h0F, 2, 8'h00); repeat (3) @(posedge clk);
        check(!ovr_on && rate == 115200, "CHANGE_BAUDRATE to 115200 clears");
        // 7. wrong op, wrong size, wrong length: no switch
        cmd(8'h0E, 16'd8, 32'd921600, 32'd115200, 0, 0); resp(8'h0F, 2, 8'h00); repeat (3) @(posedge clk);
        check(!ovr_on, "op 0E ignored");
        cmd(8'h0F, 16'd12, 32'd921600, 32'd115200, 4, 0); resp(8'h0F, 2, 8'h00); repeat (3) @(posedge clk);
        check(!ovr_on, "size 12 ignored");
        cmd(8'h0F, 16'd8, 32'd921600, 32'd115200, 1, 0); resp(8'h0F, 2, 8'h00); repeat (3) @(posedge clk);
        check(!ovr_on, "17-byte frame ignored");
        // 8. unsupported rate: ignored
        cmd(8'h0F, 16'd8, 32'd3000000, 32'd115200, 0, 0); resp(8'h0F, 2, 8'h00); repeat (3) @(posedge clk);
        check(!ovr_on, "3000000 ignored");
        // 9. response of another op, or a failed status: no switch; the right one after still does
        cmd(8'h0F, 16'd8, 32'd460800, 32'd115200, 0, 0);
        resp(8'h0D, 2, 8'h00); repeat (3) @(posedge clk);
        check(!ovr_on, "response of op 0D ignored");
        resp(8'h0F, 2, 8'h01); repeat (3) @(posedge clk);
        check(!ovr_on, "failed status ignored");
        resp(8'h0F, 2, 8'h00); repeat (3) @(posedge clk);
        check(ovr_on && rate == 460800, "460800 after the right response");
        ctl <= 2'b00; repeat (3) @(posedge clk);
        // 10. no response in time: dropped
        cmd(8'h0F, 16'd8, 32'd921600, 32'd115200, 0, 0);
        repeat (2100) @(posedge clk);
        resp(8'h0F, 2, 8'h00); repeat (3) @(posedge clk);
        check(!ovr_on, "pending dropped after the timeout");
        // 11. a response too short / too long
        cmd(8'h0F, 16'd8, 32'd921600, 32'd115200, 0, 0);
        resp(8'h0F, 1, 8'h00); repeat (3) @(posedge clk);
        check(!ovr_on, "9-byte response ignored");
        resp(8'h0F, 9, 8'h00); repeat (3) @(posedge clk);
        check(!ovr_on, "17-byte response ignored");
        resp(8'h0F, 2, 8'h00); repeat (3) @(posedge clk);
        check(ovr_on && rate == 921600, "the 10-byte one after still switches");
        // 12. reset clears
        rst <= 1; repeat (2) @(posedge clk); rst <= 0; repeat (2) @(posedge clk);
        check(!ovr_on, "reset clears");
        // 13. console text and other SLIP traffic both ways: nothing
        hraw("c"); hraw("a"); hraw("r"); hraw("t"); hraw(8'h0A); eraw("C"); eraw("B"); eraw(8'h0A);
        cmd(8'h0F, 16'd8, 32'd921600, 32'd115200, 0, 0); eraw("x"); eraw(8'h0A); repeat (3) @(posedge clk);
        check(!ovr_on, "text is not a response");
        resp(8'h0F, 2, 8'h00); repeat (3) @(posedge clk);
        check(ovr_on, "the response after text");

        // the UART rate table
        divchk(32'd115200, 24'd520, 2'd3);
        divchk(32'd460800, 24'd130, 2'd0);
        divchk(32'd921600, 24'd65, 2'd0);
        divchk(32'd1500000, 24'd40, 2'd0);
        divchk(32'd2000000, 24'd30, 2'd0);
        divchk(32'd3000000, 24'd520, 2'd3);

        $display("tb_baud_sniff: %0d ok, %0d failed", ok, bad);
        if (bad == 0) $display("ALL PASSED");
        $finish;
    end
endmodule
