// mm_burst_read_to_stream.v
//
// Generates an output stream aligned with the input timings
// by performing memory mapped burst reads

module mm_burst_read_to_stream #(
    parameter base_pointer = 23'h0
)
(
    input               hClk,
    input               hVsync,
    input               hHsync,
    input               hValid,
    input               xClk,    
        
    input               xRamReady,
    input               xStreamValid,
    input       [15:0]  xStreamData,
    input               xWrBurstDone,
    output  reg         xGbReqRead,
    
    output  reg [15:0]  hWrBurstQ,
    output  reg [22:0]  xGbAddress,
    output  reg [7:0]   hLateLines = 8'd0   // 18.32 bench: lines whose first pixel came before their burst was in
);

    localparam LINE_DEPTH = 160;

    reg [15:0] lineRam [LINE_DEPTH-1:0];//23039:0];
    initial begin : init_RAM
      integer i;
      for (i = 0; i < LINE_DEPTH; i = i + 1) lineRam[i] = 16'b0;
    end
    
    reg [14:0] xLineRam_wa;
    reg [14:0] hLineRam_ra;

    reg hVsync_r1;
    always@(posedge hClk)
        hVsync_r1 <= hVsync;

    reg hHsync_r1;
    always@(posedge hClk)
        hHsync_r1 <= hHsync;
        
    wire hStartOfFrame = ~hVsync_r1 & hVsync;
    always@(posedge hClk)
        hHsync_r1 <= hHsync;
    wire hStartOfLine = ~hHsync_r1 & hHsync;

    always@(posedge hClk)
        if(hStartOfFrame)
            hLineRam_ra <= 'd0;
        else
            if(hValid)
            begin
                if (hLineRam_ra < LINE_DEPTH) 
                    hLineRam_ra <= hLineRam_ra + 1'd1;
            end
            else
                if(hStartOfLine)
                    hLineRam_ra <= 'd0;
        
    always@(posedge hClk)
        hWrBurstQ <= lineRam[hLineRam_ra];

    // 18.32 bench: the line's burst is asked for at the end of the line before (or at the frame start) and must be in
    // before the line's first pixel is read
    reg xDoneT = 1'b0;
    always@(posedge xClk) if (xWrBurstDone) xDoneT <= ~xDoneT;
    reg [2:0] hDoneT_sr = 3'd0;
    always@(posedge hClk) hDoneT_sr <= {hDoneT_sr[1:0], xDoneT};
    reg hDoneSeen = 1'b0, hChecked = 1'b0;
    always@(posedge hClk) begin
        // the line's last pixel still comes after the mode-3 fall (one core step): check only from the next line's start
        if (hStartOfFrame || (hHsync_r1 & ~hHsync)) begin hDoneSeen <= 1'b0; hChecked <= 1'b1; end
        else begin
            if (hDoneT_sr[2] != hDoneT_sr[1]) hDoneSeen <= 1'b1;
            if (hStartOfLine) hChecked <= 1'b0;
            else if (hValid && ~hChecked) begin
                hChecked <= 1'b1;
                if (~hDoneSeen) hLateLines <= hLateLines + 1'd1;   // wraps: read the change between two reads
            end
        end
    end

    always@(posedge xClk)
        if(xStreamValid)
            lineRam[xLineRam_wa] <= xStreamData;
    
    always@(posedge xClk)
    begin
        if(xStreamValid)
            xLineRam_wa <= xLineRam_wa + 1'd1;
         
        if(xWrBurstDone)
            xLineRam_wa <= 'd0;
    end

    // TODO - cleanup CDC
            
    reg [3:0] xHsync_sr;
    always@(posedge xClk)
        xHsync_sr <= {xHsync_sr[2:0], hHsync};
    wire xEndOfLine = xHsync_sr[3:2] == 2'b10;

    reg [3:0] xVsync_sr;
    always@(posedge xClk)
        xVsync_sr <= {xVsync_sr[2:0], hVsync};
    wire xStartOfFrame = xVsync_sr[3:2] == 2'b01;

    always@(posedge xClk)
    begin
        xGbReqRead <= 1'd0;
        if(xEndOfLine || xStartOfFrame)
            xGbReqRead <= xRamReady;
    end

    always@(posedge xClk)
    begin
        if(xStartOfFrame)
        begin
            xGbAddress <= base_pointer;
        end
        else
            if(xEndOfLine)
                xGbAddress <= xGbAddress + 2*LINE_DEPTH;
    end

endmodule
