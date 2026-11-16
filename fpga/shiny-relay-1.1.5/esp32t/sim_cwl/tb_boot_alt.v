// tb_boot_alt.v (lane cartflash1): link cmd 10 BOOT_SELECT through link_net_cdc, the
// alternate boot image (boot_rom_alt.v) and the gb.v selection rule (sampled in reset).
// Run from src/rtl/EMU/CORE (the image path is relative, as for the Gowin build):
//   iverilog -g2012 -o /tmp/tb_boot_alt ../../../../sim_cwl/tb_boot_alt.v ../../BSP/link_net_cdc.v boot_rom_alt.v && vvp /tmp/tb_boot_alt
`timescale 1ns/1ps
module tb_boot_alt;
    reg hclk = 0, gclk = 0;
    always #29.8 hclk = ~hclk;
    always @(posedge hclk) gclk <= ~gclk;
    reg rst = 1, gbreset = 1;
    reg g_cmd_valid = 0; reg [7:0] g_cmd = 0, g_cmd_byte = 0;
    wire h_boot_alt, h_cart_cycle;
    wire g_ev_valid; wire [2:0] g_ev_type; wire [7:0] g_ev_byte;
    link_net_cdc cdc (
        .gclk(gclk), .g_reset(rst), .hclk(hclk), .h_reset(rst),
        .g_cmd_valid(g_cmd_valid), .g_cmd(g_cmd), .g_cmd_byte(g_cmd_byte),
        .g_ev_valid(g_ev_valid), .g_ev_type(g_ev_type), .g_ev_byte(g_ev_byte), .g_ev_taken(1'b0),
        .g_net_en(), .g_esp_may_sleep(),
        .h_net_en(), .h_master_reply_pend(), .h_master_reply_byte(), .h_master_reply_drop(), .h_barrier_en(),
        .h_master_reply_ack(1'b0), .h_slave_xfer_pend(), .h_slave_xfer_byte(), .h_slave_xfer_ack(1'b0),
        .h_cart_cycle(h_cart_cycle), .h_standing_en(), .h_slave_withdraw_pend(), .h_slave_withdraw_ack(1'b0),
        .h_boot_alt(h_boot_alt),
        .h_ev_valid(1'b0), .h_ev_type(3'd0), .h_ev_byte(8'd0));
    // gb.v rule
    reg boot_alt_r = 0;
    always @(posedge hclk) if (gbreset) boot_alt_r <= h_boot_alt;
    reg [11:0] addr = 0;
    wire [7:0] alt_q;
    boot_rom_alt rom (.clock(hclk), .address_a(addr), .q_a(alt_q));
    reg [7:0] refm [2047:0];
    initial $readmemh("BootROMs/cf_netboot.mif.vmem", refm);

    integer ok = 0, bad = 0, i, pulses = 0;
    always @(posedge hclk) if (h_cart_cycle) pulses = pulses + 1;
    task check(input c, input [255:0] what);
        begin if (c) ok = ok + 1; else begin bad = bad + 1; $display("FAIL %0s", what); end end
    endtask
    task cmd(input [7:0] c, input [7:0] b);
        begin
            @(posedge gclk); g_cmd <= c; g_cmd_byte <= b; g_cmd_valid <= 1;
            @(posedge gclk); g_cmd_valid <= 0;
            repeat (20) @(posedge hclk);
        end
    endtask
    initial begin
        repeat (10) @(posedge hclk);
        rst = 0;
        repeat (10) @(posedge hclk);
        check(h_boot_alt == 0, "stock image after FPGA reset");
        cmd(8'd10, 8'd1);
        check(h_boot_alt == 1, "cmd 10 (1): alternate selected");
        check(boot_alt_r == 1, "sampled while the GB core is in reset");
        gbreset = 0;
        repeat (5) @(posedge hclk);
        cmd(8'd10, 8'd0);
        check(h_boot_alt == 0, "cmd 10 (0): stock selected");
        check(boot_alt_r == 1, "no switch in the middle of a boot (core running)");
        cmd(8'd10, 8'd1);
        cmd(8'd5, 8'd0);
        check(pulses == 1, "cmd 5 still pulses h_cart_cycle once");
        check(h_boot_alt == 1, "the select survives the cart power cycle command");
        gbreset = 1; repeat (3) @(posedge hclk); gbreset = 0;
        check(boot_alt_r == 1, "next boot uses the alternate image");
        cmd(8'd3, 8'd1);
        check(h_boot_alt == 1, "other commands leave it alone");
        // the image: every byte of the vmem, with the registered read
        for (i = 0; i < 2048; i = i + 1) begin
            addr = i; @(posedge hclk); #1;
            if (alt_q !== refm[i]) begin bad = bad + 1; if (bad < 4) $display("FAIL rom[%0d] %h != %h", i, alt_q, refm[i]); end
        end
        ok = ok + 1;
        addr = 12'h800; @(posedge hclk); #1;
        check(alt_q === refm[0], "0x800 wraps like the stock 2 KiB image");
        rst = 1; repeat (3) @(posedge hclk); rst = 0; repeat (3) @(posedge hclk);
        check(h_boot_alt == 0, "FPGA reset (power on) clears the select");
        $display("tb_boot_alt: %0d ok, %0d failed", ok, bad);
        if (bad) $display("FAILED"); else $display("ALL PASSED");
        $finish;
    end
endmodule
