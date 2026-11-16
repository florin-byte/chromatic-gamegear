// mm_burst_write.v

// Converts QSPI memory mapped transfer into a burst write to PSRAM
module mm_burst_write(
    input           QSPI_CLK,
    input           QSPI_CS,
    input   [31:0]  qAddress,
    input           qDataValid,
    input   [15:0]  qData,
    
    input           xClk,
    input           xRdEn,
    input           xRamReady,
    output  reg     xMcuReqWrite,
    
    output  [15:0]  xDout,
    output  [22:0]  xAddress
);

    assign xAddress = qAddress[22:0];

    // cartspeed1: a transaction whose address has bit 31 set carries cart data for the express
    // ring (cart_xs.v): nothing goes into the FIFO and no PSRAM burst follows it. qAddress is
    // stable from the end of the address phase until the next transaction's address phase,
    // so its bit 31 is steady around the CS rise this module acts on.
    // 1.1.0: bit 30 = a read of the RAM cart's window (qspi_slave.v): no PSRAM burst either
    wire qCart = qAddress[31] | qAddress[30];
    reg [1:0] xCart_sr = 2'b00;
    always@(posedge xClk)
        xCart_sr <= {xCart_sr[0], qCart};

`ifdef OPEN_BUILD
    // Open-source flow: the inverted clock would leave its LUT on general
    // routing (skew, hold violations); put it on a global buffer.
    wire qspi_clk_n;
    BUFG u_qspi_clk_n_bufg(.I(~QSPI_CLK), .O(qspi_clk_n));
`else
    wire qspi_clk_n = ~QSPI_CLK;
`endif

    wire fifo_empty;
    wire fifo_full;
    wire fifo_almost_full;
    fifo1k u_fifo1k(
        .Data(qData), //input [15:0] Data
        .WrReset(1'd0), //input WrReset
        .RdReset(1'd0), //input RdReset
        .WrClk(qspi_clk_n), //input WrClk
        .RdClk(xClk), //input RdClk
        .WrEn(qDataValid & ~qCart), //input WrEn
        .RdEn(xRdEn | xMcuReqWrite), //input RdEn
        .Q(xDout), //output [15:0] Q
        .Almost_Full(fifo_almost_full), //output Almost_Full
        .Empty(fifo_empty), //output Empty
        // Full is on the write clock domain
        // Won't update when QSPI CS is high
        .Full(fifo_full) //output Full
    );
    
    reg [3:0] xCS_sr;
    always@(posedge xClk)
        xCS_sr <= {xCS_sr[2:0], QSPI_CS};
    
    always@(posedge xClk)
    begin
        xMcuReqWrite <= 1'd0;
        if(xCS_sr[3:2] == 2'b01)
            xMcuReqWrite <= xRamReady & ~xCart_sr[1];
    end
    
endmodule
