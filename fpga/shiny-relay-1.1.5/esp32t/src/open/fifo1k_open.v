// fifo1k_open.v
//
// Open-source drop-in for the Gowin-encrypted fifo1k IP (1024 x 16, dual
// clock, standard read mode: Q updates on the RdClk edge after RdEn).
// Callers use only Q and Empty; Full / Almost_Full are provided for the port
// list. Gray-code pointers, 2-flop synchronizers.

module fifo1k (
    input  [15:0] Data,
    input         WrReset,
    input         RdReset,
    input         WrClk,
    input         RdClk,
    input         WrEn,
    input         RdEn,
    output        Almost_Full,
    output reg [15:0] Q,
    output        Empty,
    output        Full
);
    localparam AW = 10;

    reg [15:0] mem [0:(1<<AW)-1];

    reg [AW:0] wbin = 0, wgray = 0;
    reg [AW:0] rbin = 0, rgray = 0;
    reg [AW:0] rgray_w1 = 0, rgray_w2 = 0;   // read pointer in write domain
    reg [AW:0] wgray_r1 = 0, wgray_r2 = 0;   // write pointer in read domain

    function [AW:0] g2b(input [AW:0] g);
        integer i;
        begin
            g2b[AW] = g[AW];
            for (i = AW - 1; i >= 0; i = i - 1)
                g2b[i] = g2b[i + 1] ^ g[i];
        end
    endfunction

    wire        empty_r    = (rgray == wgray_r2);

    // ---------------- write side ----------------
    wire [AW:0] wbin_next  = wbin + 1'b1;
    wire        full_w     = (wgray == {~rgray_w2[AW:AW-1], rgray_w2[AW-2:0]});
    wire [AW:0] wr_used    = wbin - g2b(rgray_w2);

    always @(posedge WrClk or posedge WrReset) begin
        if (WrReset) begin
            wbin <= 0; wgray <= 0;
            rgray_w1 <= 0; rgray_w2 <= 0;
        end else begin
            rgray_w1 <= rgray;
            rgray_w2 <= rgray_w1;
            if (WrEn && !full_w) begin
                wbin  <= wbin_next;
                wgray <= wbin_next ^ (wbin_next >> 1);
            end
        end
    end

    // Memory kept out of the reset blocks so it maps to block RAM.
    always @(posedge WrClk)
        if (WrEn && !full_w && !WrReset)
            mem[wbin[AW-1:0]] <= Data;

    always @(posedge RdClk)
        if (RdEn && !empty_r && !RdReset)
            Q <= mem[rbin[AW-1:0]];

    assign Full        = full_w;
    assign Almost_Full = wr_used >= ((1 << AW) - 2);

    // ---------------- read side ----------------
    wire [AW:0] rbin_next = rbin + 1'b1;

    always @(posedge RdClk or posedge RdReset) begin
        if (RdReset) begin
            rbin <= 0; rgray <= 0;
            wgray_r1 <= 0; wgray_r2 <= 0;
        end else begin
            wgray_r1 <= wgray;
            wgray_r2 <= wgray_r1;
            if (RdEn && !empty_r) begin
                rbin  <= rbin_next;
                rgray <= rbin_next ^ (rbin_next >> 1);
            end
        end
    end

    assign Empty = empty_r;
endmodule
