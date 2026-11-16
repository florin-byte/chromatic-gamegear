#!/usr/bin/env python3
"""One-off edit of the CG-102 working copy: diagnostic build gg4diag (probes of review CG-111).

Wires GG/gg_probe.v to the 48-bit bench word read by `rc dbg`, adds the cache fill check in ramcart.v, the OSD
line probe in vid_system_top.sv and the experiment under a switch (SELECT held when the game starts): the CPU
resumes only after the ROM answer for an unchanged address has been valid for 3 pclk.

Usage: patch_probes.py <working copy root>
"""
import io
import sys
from pathlib import Path

root = Path(sys.argv[1])


def edit(rel, pairs):
    path = root / rel
    text = io.open(path, encoding="utf-8", newline="").read()
    for old, new in pairs:
        if text.count(old) != 1:
            raise SystemExit("%s: expected exactly one match (found %d) for: %s" % (rel, text.count(old), old[:70]))
        text = text.replace(old, new)
    io.open(path, "w", encoding="utf-8", newline="").write(text)
    print("patched", rel)


edit("esp32t/src/rtl/EMU/emu_system_top.v", [
    ("    output  [21:0]      rc_lin_a,\n",
     "    output  [21:0]      rc_lin_a,\n"
     "    // gg4diag: probes (gg_probe.v) out to the bench word of `rc dbg`; the RAM cart's fill check and the\n"
     "    // OSD line probe come in\n"
     "    output  [47:0]      gg_dbg,\n"
     "    input   [8:0]       rc_fill_dbg,\n"
     "    input   [44:0]      osd_dbg,\n"),
    ("    wire run = !core_reset && (idle || !stall_r);",
     "    // gg4diag experiment (SELECT held while the game starts): the CPU resumes only after the ROM answer for an\n"
     "    // unchanged address has been valid for 3 pclk in a row.\n"
     "    reg alt = 1'b0;\n"
     "    reg [1:0] q_cnt = 2'd0;\n"
     "    reg [23:0] rom_a_prev = 24'd0;\n"
     "    always @(posedge pclk) begin\n"
     "        if (core_reset) alt <= BTN_SEL_filtered;\n"
     "        rom_a_prev <= rom_a;\n"
     "        if (rc_stall || rom_a != rom_a_prev) q_cnt <= 2'd0;\n"
     "        else if (q_cnt != 2'd3) q_cnt <= q_cnt + 1'b1;\n"
     "    end\n"
     "    wire rom_ok = alt ? (q_cnt == 2'd3 && !rc_stall && rom_a == rom_a_prev) : !stall_r;\n"
     "    wire run = !core_reset && (idle || rom_ok);"),
    ("        .z80_reg_out(),", "        .z80_reg_out(z80_reg),"),
    ("        .vdp_regs_out(),", "        .vdp_regs_out(vdp_regs),"),
    ("        .vdp_cram_out(),", "        .vdp_cram_out(vdp_cram),"),
    ("        .z80_m1_n(),", "        .z80_m1_n(z80_m1_n),"),
    ("    assign o_emulator_reset = reset_h;",
     "    assign o_emulator_reset = reset_h;\n"
     "    wire [229:0] z80_reg;\n"
     "    wire [127:0] vdp_regs;\n"
     "    wire [383:0] vdp_cram;\n"
     "    wire z80_m1_n;\n"
     "    reg [1:0] hold_s = 2'b00;\n"
     "    always @(posedge pclk) hold_s <= {hold_s[0], rc_hold};\n"
     "`ifdef VERILATOR\n"
     "    gg_probe #(.SLOT_BITS(12)) probe (\n"
     "`else\n"
     "    gg_probe probe (\n"
     "`endif\n"
     "        .clk(pclk), .clear(hold_s[1]), .core_reset(core_reset), .loaded(loaded), .bad(bad), .idle(idle),\n"
     "        .stall_r(stall_r), .alt(alt), .rom_stall(rc_stall), .ce_cpu(ce_cpu), .m1_n(z80_m1_n),\n"
     "        .cpu_di(idle ? 8'h00 : rom_do), .pc(z80_reg[79:64]), .sp(z80_reg[63:48]), .halt(z80_reg[229]),\n"
     "        .dl_en(dl_en), .dl_a(dl_a), .dl_d(dl_d),\n"
     "        .vdp_regs(vdp_regs[15:0]), .cram0(vdp_cram[11:0]), .cram16(vdp_cram[203:192]),\n"
     "        .ram_a(ram_a[12:0]), .ram_d(ram_d), .ram_q(ram_q), .ram_we(ram_we && run),\n"
     "        .fill_err(rc_fill_dbg[8]), .fills(rc_fill_dbg[7:0]), .osd(osd_dbg), .word(gg_dbg));"),
])

edit("esp32t/src/rtl/EMU/ramcart.v", [
    ("    output     [47:0]  h_dbg,              // bench: {flags, fills, the core's address}\n",
     "    output     [47:0]  h_dbg,              // bench: {flags, fills, the core's address}\n"
     "    output reg         x_fill_err = 1'b0,  // gg4diag (sticky): a line fill ended with a word count other than 16\n"),
    ("                    if (x_done) xs <= (xk == K_FILL) ? X_TAG : X_DONE;",
     "                    if (x_done) xs <= (xk == K_FILL) ? X_TAG : X_DONE;\n"
     "                    if (x_done && xk == K_FILL && (xi + {8'd0, x_dout_valid}) != 9'd16) x_fill_err <= 1'b1;"),
])

edit("esp32t/src/rtl/BSP/vid_system_top.sv", [
    ("    output      [47:0]  gPanelDbg,          // 18.32 bench: {0, short rows, extra VSYNCs, end holds, rows waited for}\n",
     "    output      [47:0]  gPanelDbg,          // 18.32 bench: {0, short rows, extra VSYNCs, end holds, rows waited for}\n"
     "    output reg  [44:0]  osd_dbg = 45'd0,    // gg4diag: the OSD pixels read for row 72 {first, last, count, seen, 4'd0}\n"),
    ("    wire osdTransparent = (hWrBurstQ2 == 16'hF81F) ? 1'd1 : 1'd0;",
     "    wire osdTransparent = (hWrBurstQ2 == 16'hF81F) ? 1'd1 : 1'd0;\n"
     "\n"
     "    // gg4diag: which OSD words reach the first and the last pixel of row 72 while the menu is drawn\n"
     "    reg [7:0] oRow = 8'd0, oCount = 8'd0;\n"
     "    reg [15:0] oFirst = 16'd0, oLast = 16'd0;\n"
     "    always @(posedge hClk) begin\n"
     "        if (hVsync & ~hVsync_r1) oRow <= 8'd0;\n"
     "        else if (~hHsync & hHsync_r1) begin\n"
     "            if (oRow == 8'd72 && hDrawOSD) osd_dbg <= {oFirst, oLast, oCount, 1'b1, 4'd0};\n"
     "            if (oRow != 8'd255) oRow <= oRow + 1'd1;\n"
     "            oCount <= 8'd0;\n"
     "        end else if (gb_lcd_clkena && oRow == 8'd72) begin\n"
     "            if (oCount == 8'd0) oFirst <= hWrBurstQ2;\n"
     "            oLast <= hWrBurstQ2;\n"
     "            oCount <= oCount + 1'd1;\n"
     "        end\n"
     "    end"),
])

edit("esp32t/src/top.v", [
    ("    always @(posedge gClk) rc_dbg_g2 <= {fb_dbg[23:16], rc_dbg[39:32], fb_dbg[15:0], rc_dbg[23:16], rc_dbg[7:0]};",
     "    // gg4diag: the bench word carries the Game Gear probes (gg_probe.v, pclk; a slot is frozen for ~1 s)\n"
     "    wire [47:0] gg_dbg;\n"
     "    wire [44:0] osd_dbg;\n"
     "    wire [47:0] rc_hdbg;\n"
     "    wire rc_fill_err;\n"
     "    reg [47:0] gg_dbg_g1 = 48'd0;\n"
     "    always @(posedge gClk) begin gg_dbg_g1 <= gg_dbg; rc_dbg_g2 <= gg_dbg_g1; end\n"
     "    // (18.32 panel statistics, not shown in this build:)\n"
     "    wire [47:0] rc_dbg_panel = {fb_dbg[23:16], rc_dbg[39:32], fb_dbg[15:0], rc_dbg[23:16], rc_dbg[7:0]};"),
    ("        .gPanelDbg(rc_dbg),", "        .gPanelDbg(rc_dbg),\n        .osd_dbg(osd_dbg),"),
    ("        .rc_lin_a           (rc_lin_a),",
     "        .rc_lin_a           (rc_lin_a),\n"
     "        .gg_dbg             (gg_dbg),\n"
     "        .rc_fill_dbg        ({rc_fill_err, rc_hdbg[23:16]}),\n"
     "        .osd_dbg            (osd_dbg),"),
    ("        .h_dbg            (),", "        .h_dbg            (rc_hdbg),\n        .x_fill_err       (rc_fill_err),"),
])

edit("esp32t/evt1_x2.gprj", [
    ('        <File path="src/rtl/EMU/GG/gg_ram.v" type="file.verilog" enable="1"/>',
     '        <File path="src/rtl/EMU/GG/gg_ram.v" type="file.verilog" enable="1"/>\n'
     '        <File path="src/rtl/EMU/GG/gg_probe.v" type="file.verilog" enable="1"/>'),
])

edit("sim_gg/tb_gg.sv", [
    (" output rom_wait, output running, output [14:0] buffered);",
     " output rom_wait, output running, output [14:0] buffered,\n input sel, output [47:0] gg_dbg);"),
    (" .BTN_SEL(0),", " .BTN_SEL(sel),"),
    (" .o_emulator_reset(core_reset));",
     " .gg_dbg(gg_dbg), .rc_fill_dbg({cart_fill_err, cart_hdbg[23:16]}), .osd_dbg(45'd0),\n"
     " .o_emulator_reset(core_reset));\n"
     " wire [47:0] cart_hdbg;\n wire cart_fill_err;"),
    (" .h_dirty(), .h_dbg(),", " .h_dirty(), .h_dbg(cart_hdbg), .x_fill_err(cart_fill_err),"),
])

edit("sim_gg/main.cpp", [
    ("    int gphase=0; uint64_t warm=0;", "    int gphase=0; uint64_t warm=0; bool alt=false;"),
    ('        else if(opt=="--gphase1") gphase=1;', '        else if(opt=="--gphase1") gphase=1;\n        else if(opt=="--alt") alt=true;'),
    ("        t.start=start_at>=0", "        t.sel=alt;\n        t.start=start_at>=0"),
    ("    t.final(); return panel_ok ? 0 : 3;",
     "    // the probe word as `rc dbg` would show it: let the slots rotate once and print each\n"
     "    { uint64_t seen[8]; bool have[8]={false};\n"
     "      for(int i=0;i<8*4096+64;i++) { t.pclk=0; t.hclk=((cycle+i)&1); t.eval(); t.hclk=!((cycle+i)&1); t.pclk=1; t.eval();\n"
     "          int s=(t.gg_dbg>>45)&7; seen[s]=t.gg_dbg; have[s]=true; }\n"
     "      static const char* names[8]={\"0A\",\"0B\",\"1A\",\"1B\",\"2A\",\"2B\",\"3A\",\"3B\"};\n"
     "      std::ofstream pr(out/\"probes.txt\");\n"
     "      for(int s=0;s<8;s++) if(have[s]) { char line[96];\n"
     "          snprintf(line,sizeof(line),\"probe %s a=%04llx fills=%04llx flags=%04llx\\n\",names[s],\n"
     "              (unsigned long long)(seen[s]&0xffff),(unsigned long long)((seen[s]>>16)&0xffff),(unsigned long long)((seen[s]>>32)&0xffff));\n"
     "          std::cout<<line; pr<<line; } }\n"
     "    t.final(); return panel_ok ? 0 : 3;"),
])

run = root / "sim_gg/run.sh"
text = io.open(run, encoding="utf-8", newline="").read()
old = '"$gg"/gg_ce_gen.v "$gg"/gg_ram.v "$gg"/gg_rom_loader.v "$vout"'
if text.count(old) != 1:
    raise SystemExit("run.sh: file list not found")
text = text.replace(old, '"$gg"/gg_ce_gen.v "$gg"/gg_ram.v "$gg"/gg_probe.v "$gg"/gg_rom_loader.v "$vout"')
io.open(run, "w", encoding="utf-8", newline="").write(text)
print("patched sim_gg/run.sh")
