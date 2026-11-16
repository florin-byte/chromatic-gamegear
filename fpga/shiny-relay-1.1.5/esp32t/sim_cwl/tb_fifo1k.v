// tb_fifo1k.v: the open fifo1k against the standard-mode contract.
// iverilog -g2012 -o tb_fifo1k tb_fifo1k.v ../src/open/fifo1k_open.v && vvp tb_fifo1k
`timescale 1ns/1ps
module tb_fifo1k;
    reg wclk = 0, rclk = 0;
    always #31 wclk = ~wclk;     // ~16 MHz
    always #5  rclk = ~rclk;     // 100 MHz
    reg wrst = 0, wen = 0, ren = 0;
    reg [15:0] d = 0;
    wire [15:0] q;
    wire empty, full, afull;
    fifo1k dut(.Data(d), .WrReset(wrst), .RdReset(wrst), .WrClk(wclk), .RdClk(rclk),
               .WrEn(wen), .RdEn(ren), .Almost_Full(afull), .Q(q), .Empty(empty), .Full(full));
    integer i, errors = 0;
    initial begin
        @(posedge wclk) wrst <= 1; @(posedge wclk) wrst <= 0;
        if (!empty) begin $display("FAIL: not empty after reset"); errors = errors + 1; end
        for (i = 0; i < 320; i = i + 1) begin
            @(posedge wclk) begin wen <= 1; d <= i * 7 + 3; end
        end
        @(posedge wclk) wen <= 0;
        repeat (10) @(posedge rclk);
        // Prefetch: one RdEn pulse, Q valid the cycle after.
        for (i = 0; i < 320; i = i + 1) begin
            @(posedge rclk) ren <= 1;
            @(posedge rclk) ren <= 0;
            #1 if (q !== i * 7 + 3) begin $display("FAIL: word %0d = %0d", i, q); errors = errors + 1; end
        end
        repeat (3) @(posedge rclk);
        if (!empty) begin $display("FAIL: not empty after drain"); errors = errors + 1; end
        // Reset mid-stream clears it.
        @(posedge wclk) begin wen <= 1; d <= 16'hBEEF; end
        @(posedge wclk) wen <= 0;
        @(posedge wclk) wrst <= 1; @(posedge wclk) wrst <= 0;
        repeat (5) @(posedge rclk);
        if (!empty) begin $display("FAIL: not empty after reset 2"); errors = errors + 1; end
        if (errors) $display("FAILED %0d", errors); else $display("ALL PASSED");
        $finish;
    end
endmodule
