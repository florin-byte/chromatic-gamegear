// QSPI_Slave.v
//
// A transaction: 11 command bits (b10 = 1 write, b9:0 = length - 1), 32 address bits (all on MOSI),
// 3 dummy clocks, then data on 4 lines. Shiny Relay 1.1.0 (FPGA 18.30) adds the READ: command
// b10 = 0 with address b30 = 1 reads the RAM cart's read window (ramcart.v, words at address
// b9:1) with ONE MORE dummy clock than a write (4: the ESP32 still drives MOSI in the first clock
// after the 3rd, measured on unit 1: the first nibble's bit 0 read 0): the FPGA drives the 4 lines
// from the falling edge before clock 47, low byte of each word first, high nibble first (the order
// writes arrive in). A read never reaches PSRAM
// or the cart ring (qDataValid / qByteValid stay low).

module QSPI_Slave(
    input               QSPI_CLK,
    input               QSPI_CS,
    input               QSPI_MOSI,
    input               QSPI_MISO,
    input               QSPI_WP,
    input               QSPI_HD,
    
    output              qMenuInit,
    output              qDataValid,
    output      [15:0]  qData,
    output  reg [31:0]  qAddress = 'd0,
    output  reg [9:0]   qLength = 'd0,
    output  reg         qCommand = 'd0,
    // cartspeed1: the byte stream for the express cart ring (cart_xs.v)
    output              qAddrReadyO,        // one QSPI clock after the 32 address bits
    output              qByteValid,         // one QSPI clock per data byte
    output      [7:0]   qByte,
    // the read window (1.1.0)
    output  reg [8:0]   qWinAddr = 9'd0,     // word address, to the window's read port (~QSPI_CLK)
    input       [15:0]  qWinData,
    output              qOutEn,
    output      [3:0]   qOut
);

    reg qAddReady = 'd0;
    reg qLenReady = 'd0;

    reg [3:0] qPins_r1 = 'd0;
    reg [7:0] qDataByte = 'd0;
    reg qCyclePhase = 'd0;

    reg qValid = 'd0;
    wire [3:0] qPins = {
                    QSPI_HD,
                    QSPI_WP,
                    QSPI_MISO,
                    QSPI_MOSI};

    reg [7:0] qCycleCount = 'd0;

    reg qMenuInit1 = 1'd0;
    reg qMenuInit2 = 1'd0;
    assign qMenuInit = qMenuInit2;

    always@(posedge QSPI_CLK or posedge QSPI_CS)
    begin
        if(QSPI_CS)
        begin
            qCyclePhase <= 1'd0;
            qValid      <= 1'd0;
            qCycleCount <= 1'd0;
            qCommand    <= 1'd0;
            qAddReady   <= 1'd0;
            qLenReady   <= 1'd0;
        end
        else
        begin
            if(qCycleCount <= 50)
                qCycleCount   <= qCycleCount + 1'd1;
            
            // Command
            if(qCycleCount == 0)
                qCommand <= QSPI_MOSI;

            if((qCycleCount >= 1) && (qCycleCount <= 10))
                qLength <= {qLength[8:0], QSPI_MOSI};
                
            if((qCycleCount >= 11) && (qCycleCount <= 42))
                qAddress <= {qAddress[30:0], QSPI_MOSI};
                
            if(qCycleCount == 11)
                qLenReady <= 1'd1;
            else
                qLenReady <= 1'd0;

            if(qCycleCount == 43)
            begin
                qAddReady <= 1'd1;
                if(qAddress == 0) // first row
                begin
                    qMenuInit1  <=  1'd1;
                    if(qMenuInit1)
                        qMenuInit2 <= 1'd1;
                end
            end
            else
                qAddReady <= 1'd0;
                
            // QSPI data
            if(qCycleCount >= 46)
            begin
                qCyclePhase      <= ~qCyclePhase;
                if(qCyclePhase)
                begin
                    qDataByte   <= {qPins_r1,qPins};
                    qValid      <= 1'd1;
                end
                else
                begin
                    qPins_r1    <= qPins;
                    qValid      <= 1'd0;
                end
            end
        end
    end

    reg [7:0] qDataByte_r1 = 'd0;
    reg QSPI_VALID_phase = 'd0;
    always@(posedge QSPI_CLK or posedge QSPI_CS)
        if(QSPI_CS)
            QSPI_VALID_phase <= 'd0;
        else
            if(qValid)
            begin
                qDataByte_r1 <= qDataByte;
                QSPI_VALID_phase <= ~QSPI_VALID_phase;
            end

    // a write carries data in; a read's own nibbles must not
    assign qDataValid = qValid&QSPI_VALID_phase&qCommand;
    assign qAddrReadyO = qAddReady;
    assign qByteValid  = qValid&qCommand;
    assign qByte       = qDataByte;
    assign qData = {qDataByte,qDataByte_r1};

    // ---------------- the read (falling edges) ----------------
    // The drivers turn on one clock early (the last dummy clock) with nibble 0 already on the
    // pins, and nibble 0 is held through the first data clock: switching the output enable and
    // the data in the same half clock read back as an undriven bus at 40 MHz (unit 1, 18.30 t1830d).
    reg        rOn = 1'b0, rPre = 1'b0;
    reg  [1:0] rNib = 2'd0;
    reg [15:0] rCur = 16'd0;
    reg  [3:0] rOut = 4'd0;
    always@(negedge QSPI_CLK or posedge QSPI_CS)
    begin
        if(QSPI_CS)
        begin
            rOn  <= 1'd0;
            rPre <= 1'd0;
            rNib <= 2'd0;
        end
        else
        begin
            if(qCycleCount == 44)
                qWinAddr <= qAddress[9:1];          // the window's word: data on the next falling edge
            if((qCycleCount == 46) && !rOn && !qCommand && qAddress[30])
            begin
                rOn  <= 1'd1;                       // on, a clock before the ESP32 samples
                rPre <= 1'd1;
                rCur <= qWinData;
                rOut <= qWinData[7:4];
                rNib <= 2'd1;
            end
            else if(rPre)
                rPre <= 1'd0;                       // nibble 0 again: the first data clock
            else if(rOn)
            begin
                rNib <= rNib + 1'd1;
                case(rNib)
                2'd0: begin rCur <= qWinData; rOut <= qWinData[7:4]; end
                2'd1: begin rOut <= rCur[3:0]; qWinAddr <= qWinAddr + 1'd1; end
                2'd2: rOut <= rCur[15:12];
                2'd3: rOut <= rCur[11:8];
                endcase
            end
        end
    end
    assign qOutEn = rOn & ~QSPI_CS;
    assign qOut   = rOut;

endmodule
