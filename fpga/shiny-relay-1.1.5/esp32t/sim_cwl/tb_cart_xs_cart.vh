    real T_DSET = 200.0, T_DHOLD = 100.0, T_BP = 7000.0; reg [20:0] stuck_addr = 21'h1FFFFF; reg [7:0] stuck_mask = 8'hFF;
    // ---------------------------------------------------------------- the cart
    localparam SIZE = 2097152;
    reg [7:0] flash [0:SIZE-1];
    reg [7:0] fram  [0:32767];
    reg [8:0] rom_bank = 9'd1; reg [3:0] ram_bank = 4'd0; reg ram_en = 1'b0;
    integer i;
    wire [20:0] fa = A[15:14] == 2'b00 ? {7'd0, A[13:0]} : {rom_bank[6:0], A[13:0]};
    localparam F_READ = 0, F_U1 = 1, F_U2 = 2, F_ID = 3, F_CFI = 4, F_PROG = 5, F_E80 = 6, F_E1 = 7, F_E2 = 8;
    integer fs = F_READ;
    real busy_until = 0.0; reg fbusy = 0, ferase = 0, toggle = 0; reg [7:0] pdata = 8'h00; reg [20:0] paddr = 0;
    integer n_prog = 0, n_erase = 0, n_busy_writes = 0, n_suspend = 0, n_rejected = 0, n_cmds_accepted = 0, n_prot = 0;
    real id_exit_t = -1.0; integer n_tida = 0;
    reg [14:0] fram_a_lat = 15'd0; real cs_rise_t = 0.0, cs_fall_t = 0.0;
    integer n_fram_bad = 0, n_fram_wr = 0, n_fram_rd = 0, n_fram_torn = 0, n_cs_cycles = 0;
    // pulse qualification
    real we_fall_t = 0.0, d_change_t = 0.0, a_change_t = 0.0; reg pulse_ok = 1'b1; reg [7:0] d_at_fall; reg [15:0] a_at_fall;
    reg [7:0] d_prev = 8'h00; reg [15:0] a_prev = 16'h0;
    always @(DOUT or DRV) if (DRV) begin if (DOUT !== d_prev) d_change_t = $realtime; d_prev = DOUT; end
    always @(A) begin a_change_t = $realtime; a_prev = A; end
    always @(negedge WR) begin
        we_fall_t = $realtime; d_at_fall = DOUT; a_at_fall = A;
        pulse_ok = DRV && (we_fall_t - d_change_t >= T_DSET) && (CLK == 1'b0) && (RD == 1'b1);
    end
    always @(posedge CLK) if (!WR) pulse_ok = 1'b0;
    always @(negedge RD)  if (!WR) pulse_ok = 1'b0;
    reg [7:0] latched_d; reg [15:0] latched_a; reg we_rise_pending = 0;
    always @(posedge WR) begin
        if (DOUT !== d_at_fall || A !== a_at_fall || !DRV) pulse_ok = 1'b0;
        latched_d = DOUT; latched_a = A; we_rise_pending = 1;
        #(T_DHOLD);                                            // data must still be held this long
        if (!DRV || DOUT !== latched_d) pulse_ok = 1'b0;
        we_rise_pending = 0;
        mapper_write(latched_a, latched_d);                    // the mapper takes any write
        if (!latched_a[15]) begin
            if (pulse_ok) flash_write(latched_a, latched_d); else n_rejected = n_rejected + 1;
        end else if (latched_a[15:13] == 3'b101 && ram_en && !CS) begin
            n_fram_wr = n_fram_wr + 1;
            if ($realtime - 100.0 - we_fall_t < 100.0 || !pulse_ok) begin fram[fram_a_lat] = 8'hXX; n_fram_torn = n_fram_torn + 1; end   // a cut pulse tears that one byte
            else fram[fram_a_lat] = latched_d;
        end
    end
    // ---- the FRAM (32 KiB, e.g. FM18W08 / MB85R256): the address is LATCHED on the falling /CS
    // edge; needs the address stable >= 50 ns before it, /CS high >= 100 ns between accesses
    // (precharge), /CS low through the whole /WR pulse, and the pins' address unchanged while low.
    always @(negedge CS) begin
        cs_fall_t = $realtime; n_cs_cycles = n_cs_cycles + 1;
        fram_a_lat = {ram_bank[1:0], A[12:0]};
        if ($realtime - a_change_t < 50.0) begin n_fram_bad = n_fram_bad + 1; if (n_fram_bad < 4) $display("FAIL FRAM: address %0.0f ns before /CS", $realtime - a_change_t); end
        if ($realtime - cs_rise_t < 100.0 && cs_rise_t > 0.0) begin n_fram_bad = n_fram_bad + 1; if (n_fram_bad < 4) $display("FAIL FRAM: /CS high only %0.0f ns", $realtime - cs_rise_t); end
        if (A[15:13] != 3'b101) begin n_fram_bad = n_fram_bad + 1; $display("FAIL FRAM: /CS outside A000-BFFF (%04x)", A); end
    end
    always @(posedge CS) cs_rise_t = $realtime;
    always @(A) if (!CS) begin n_fram_bad = n_fram_bad + 1; if (n_fram_bad < 4) $display("FAIL FRAM: address moved while /CS low"); end
    task mapper_write(input [15:0] a, input [7:0] d);
        begin
            if (a < 16'h2000) ram_en = (d[3:0] == 4'hA);
            else if (a < 16'h3000) rom_bank[7:0] = d;
            else if (a < 16'h4000) rom_bank[8] = d[0];
            else if (a < 16'h6000) ram_bank = d[3:0];
        end
    endtask
    function prot(input [20:0] x); prot = (x >= 21'h1F0000); endfunction
    task flash_write(input [15:0] a, input [7:0] d);
        reg [20:0] f; reg u1, u2;
        begin
            f = (a[15:14] == 2'b00) ? {7'd0, a[13:0]} : {rom_bank[6:0], a[13:0]};
            u1 = (a[11:0] == 12'hAAA); u2 = (a[11:0] == 12'h555);
            if (fbusy) begin
                n_busy_writes = n_busy_writes + 1;
                if (d == 8'hB0) n_suspend = n_suspend + 1;
            end else begin
                n_cmds_accepted = n_cmds_accepted + 1;
                case (fs)
                F_READ, F_ID, F_CFI: begin
                    if (d == 8'hF0) begin if (fs != F_READ) id_exit_t = $realtime; fs = F_READ; end
                    else if (u1 && d == 8'hAA) fs = F_U1;
                end
                F_U1: fs = (u2 && d == 8'h55) ? F_U2 : F_READ;
                F_U2: begin
                    fs = F_READ;
                    if (u1) case (d)
                        8'h90: fs = F_ID;
                        8'h98: fs = F_CFI;
                        8'hA0: fs = F_PROG;
                        8'h80: fs = F_E80;
                        8'hF0: id_exit_t = $realtime;
                        default: ;
                    endcase
                end
                F_PROG: begin
                    fs = F_READ; n_prog = n_prog + 1;
                    if (prot(f)) n_prot = n_prot + 1;
                    else begin fbusy = 1; ferase = 0; paddr = f; pdata = d; busy_until = $realtime + T_BP; end
                end
                F_E80: fs = (u1 && d == 8'hAA) ? F_E1 : F_READ;
                F_E1:  fs = (u2 && d == 8'h55) ? F_E2 : F_READ;
                F_E2: begin
                    fs = F_READ;
                    if (d == 8'h50 || d == 8'h30 || (d == 8'h10 && u1)) begin
                        n_erase = n_erase + 1;
                        paddr = (d == 8'h50) ? {f[20:12], 12'h000} : (d == 8'h30) ? {f[20:16], 16'h0000} : 21'd0;
                        pdata = d;
                        if (d != 8'h10 && prot(paddr)) n_prot = n_prot + 1;
                        else begin fbusy = 1; ferase = 1; busy_until = $realtime + 60000.0; end      // 60 us (scaled)
                    end
                end
                default: fs = F_READ;
                endcase
            end
        end
    endtask
    always @(posedge clk) if (fbusy && $realtime >= busy_until) begin
        if (!ferase) flash[paddr] = flash[paddr] & pdata & ((paddr == stuck_addr) ? stuck_mask : 8'hFF);
        else if (pdata == 8'h50) for (i = 0; i < 4096; i = i + 1) flash[paddr + i] = 8'hFF;
        else if (pdata == 8'h30) for (i = 0; i < 65536; i = i + 1) flash[paddr + i] = 8'hFF;
        else for (i = 0; i < SIZE; i = i + 1) if (!prot(i)) flash[i] = 8'hFF;
        fbusy = 0;
    end
    reg [7:0] rd_val; reg stuck = 0;          // stuck: a chip whose DQ6 never stops toggling
    always @(negedge RD) begin
        if (stuck) begin toggle = ~toggle; rd_val = {1'b0, toggle, 6'b0}; end
        else if (fbusy) begin toggle = ~toggle; rd_val = {ferase ? 1'b0 : ~pdata[7], toggle, 6'b0}; end
        else if (!A[15] && fs == F_ID) rd_val = (fa[7:0] == 8'h00) ? 8'hBF : (fa[7:0] == 8'h01) ? 8'hC9 : 8'h00;
        else if (!A[15] && fs == F_CFI) rd_val = (fa[7:0] == 8'h10) ? "Q" : (fa[7:0] == 8'h11) ? "R" : (fa[7:0] == 8'h12) ? "Y" : 8'h00;
        else if (!A[15]) begin
            rd_val = flash[fa];
            if (id_exit_t >= 0 && $realtime - id_exit_t < 150.0) begin n_tida = n_tida + 1; rd_val = 8'hXX; end
        end else if (A[15:13] == 3'b101) begin
            #2;                                                 // /CS falls with /RD: the latch first
            if (ram_en && !CS) begin rd_val = fram[fram_a_lat]; n_fram_rd = n_fram_rd + 1; end else rd_val = 8'hFF;
        end
        else rd_val = 8'hFF;
    end
    assign DIN = (!RD && !DRV) ? rd_val : 8'hZZ;

