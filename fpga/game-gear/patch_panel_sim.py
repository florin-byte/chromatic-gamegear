#!/usr/bin/env python3
"""One-off edit of the CG-102 working copy: put the stock display path into the simulation.

After the adversarial review CG-109: the testbench gets the real vid_system_top.sv (color correction, OSD mix,
frame buffer addressing) and ST7785_panel_master.v on gclk, and the driver checks, per core frame, what the
panel really does: it saw the frame start, its rows are 912 gclk long, 144 rows of 720 active gclk, the picture
rebuilt from LCD_DB equals the one the core sent, and the frame buffer address after row k is 0x10000 + 320 k.

Usage: patch_panel_sim.py <working copy root>
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


(root / "sim_gg/bsp_stubs.v").write_text("""// Simulation stand-ins for what vid_system_top.sv needs besides ST7785_panel_master.v / ST7785_init.v:
// the Gowin ODDR primitive and the VHDL overlays (battery, timer, debug), which are off in these tests.
module ODDR(output Q0, output Q1, input D0, input D1, input TX, input CLK);
    assign Q0 = CLK ? D0 : D1;
    assign Q1 = TX;
endmodule
module overlayBatteryFront(input [7:0] screenX, input [7:0] screenY, output overlayActive); assign overlayActive = 1'b0; endmodule
module overlayBatteryBack(input [7:0] screenX, input [7:0] screenY, output overlayActive); assign overlayActive = 1'b0; endmodule
module overlayTimerFront(input [7:0] screenX, input [7:0] screenY, output overlayActive); assign overlayActive = 1'b0; endmodule
module overlayTimerBack(input [7:0] screenX, input [7:0] screenY, output overlayActive); assign overlayActive = 1'b0; endmodule
module overlayTimerNumber(input [7:0] screenX, input [7:0] screenY, input [3:0] number, output overlayActive); assign overlayActive = 1'b0; endmodule
module overlayDebug(input [7:0] screenX, input [7:0] screenY, input [3:0] number, output overlayActive); assign overlayActive = 1'b0; endmodule
""", encoding="utf-8", newline="\n")
print("wrote sim_gg/bsp_stubs.v")

edit("sim_gg/tb_gg.sv", [
    ("module tb_gg(input pclk, hclk, reset_n, rc_on, rc_hold, start, inval,",
     "module tb_gg(input pclk, hclk, gclk, reset_n, rc_on, rc_hold, start, inval,\n"
     " output [5:0] lcd_db, output lcd_de, lcd_hs, lcd_vs, output p_vs, output lcd_init_done,\n"
     " output gb_newline, output [22:0] gb_addr, output [47:0] panel_dbg,"),
    (" .LCD_INIT_DONE(1'b1),", " .LCD_INIT_DONE(lcd_init_done),"),
    (" ramcart cart (",
     " // The stock display path: color correction, OSD mix, frame buffer addressing and the panel master.\n"
     " assign p_vs = vid.u_ST7785_panel_master.pGbVsync & ~vid.u_ST7785_panel_master.pGbVsync_r1;\n"
     " vid_system_top #(1) vid (\n"
     "  .appear_off(core_reset), .gClk(gclk), .hClk(hclk), .pClk(pclk), .reset(!reset_n), .BTN_MENU(1'b1),\n"
     "  .LCD_DB(lcd_db), .capture_valid(), .capture_frame_start(), .capture_line_end(), .capture_pixel(),\n"
     "  .LCD_DOTCLK(), .LCD_ENABLE(lcd_de), .LCD_HSYNC(lcd_hs), .LCD_RESET(), .LCD_SPI_CSX(), .LCD_SPI_SCLK(),\n"
     "  .LCD_SPI_SDA(), .LCD_TE(1'b0), .LCD_EN(1'b1), .LCD_GENLOCK(), .LCD_VSYNC(lcd_vs),\n"
     "  .frameBlendEnable(1'b0), .colorCorrectionEnableLCD(1'b0), .colorCorrectionEnableUVC(1'b0), .voltageLow(1'b0),\n"
     "  .lowBattDispMode(2'd0), .showTimer(1'b0), .runTimer(1'b0), .resetTimer(1'b0), .gSecondEna(1'b0),\n"
     "  .gPercentEna(1'b0), .debug_system(32'd0), .debug_system_on(1'b0),\n"
     "  .hGBNewLine(gb_newline), .hGBAddress(gb_addr), .hGBWrite(), .hGBData(), .hHsync(), .hVsync(),\n"
     "  .hWrBurstQ(16'd0), .hWrBurstQ2(16'd0), .hDrawOSD(), .gPanelDbg(panel_dbg), .LCD_INIT_DONE(lcd_init_done),\n"
     "  .gb_lcd_clkena(valid), .gb_lcd_data(pixel), .gb_lcd_mode(mode), .gb_lcd_on(1'b1), .gb_lcd_vsync(vsync));\n"
     " ramcart cart ("),
])

edit("sim_gg/main.cpp", [
    ("    bool content=false, nomagic=false, noload=false;",
     "    bool content=false, nomagic=false, noload=false;\n    int gphase=0;"),
    ('        else if(opt=="--nomagic") nomagic=true;',
     '        else if(opt=="--nomagic") nomagic=true;\n        else if(opt=="--gphase1") gphase=1;'),
    ("    int frames=0,lines=0,pixels=0;",
     "    int frames=0,lines=0,pixels=0;\n"
     "    // Panel side (gclk): rows rebuilt from LCD_DE / LCD_DB, the row period, the frame starts the panel saw.\n"
     "    std::vector<std::vector<uint8_t>> prows; std::vector<uint8_t> prow;\n"
     "    bool prev_hs=true; long row_gclk=0; int pvs=0, newlines=0; bool prev_nl=false;\n"
     "    long panel_missed=0, panel_extra=0, bad_period=0, bad_de=0, bad_rows=0, bad_image=0, bad_addr=0, compared=0;\n"
     "    int lock_shift=-1, lock_perm=-1;\n"
     "    static const int perms[6][3]={{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};\n"
     "    auto row_matches=[&](const std::vector<uint8_t>& got,const uint16_t* want,int shift,int perm) {\n"
     "        for(int x=0;x<160;x++) { uint8_t c[3]={uint8_t((want[x]&31)<<1),uint8_t(((want[x]>>5)&31)<<1),uint8_t(((want[x]>>10)&31)<<1)};\n"
     "            for(int k=0;k<3;k++) if(got[shift+3*x+k]!=c[perms[perm][k]]) return false; }\n"
     "        return true; };\n"
     "    auto compare_panel=[&] {\n"
     "        if(prows.size()!=144) { ++bad_rows; return; }\n"
     "        if(lock_shift<0) { for(int s=0;s+480<=720 && lock_shift<0;s++) for(int p=0;p<6;p++) { bool ok=true;\n"
     "              for(int y=0;y<144 && ok;y++) ok=row_matches(prows[y],&image[160*y],s,p);\n"
     "              if(ok) { lock_shift=s; lock_perm=p; break; } }\n"
     "            if(lock_shift<0) ++bad_image; return; }\n"
     "        for(int y=0;y<144;y++) if(!row_matches(prows[y],&image[160*y],lock_shift,lock_perm)) { ++bad_image; return; } };"),
    ("        t.pclk=0; t.hclk=(cycle&1); t.eval();",
     "        t.pclk=0; t.hclk=(cycle&1); t.eval();\n        const bool g_rise=((cycle+(gphase?2:0))&3)==0;"),
    ("                if(v && !prev_v) {\n                    if(synced && !image.empty()) {",
     "                if(v && !prev_v) {\n"
     "                    if(synced && frames>=3 && image.size()==23040) {\n"
     "                        if(pvs==0) ++panel_missed; else if(pvs>1) ++panel_extra;\n"
     "                        if(newlines!=144) ++bad_addr;\n"
     "                        compare_panel(); ++compared;\n"
     "                    }\n"
     "                    pvs=0; newlines=0; prows.clear();\n"
     "                    if(synced && !image.empty()) {"),
    ("                if(synced && !v) {", "                if(synced) {"),
    ("            prev_v=v; prev_m=m;\n",
     "            prev_v=v; prev_m=m;\n"
     "            // vid_system_top: the frame buffer address after the end of row k must be 0x10000 + 320 * k.\n"
     "            if(prev_nl && !t.core_reset && synced) { if(t.gb_addr!=0x10000u+320u*unsigned(newlines)) ++bad_addr; ++newlines; }\n"
     "            prev_nl=t.gb_newline;\n"),
    ("        t.hclk=!(cycle&1); t.pclk=1; t.eval();",
     "        if(g_rise && t.lcd_init_done) {   // the values gclk sees at this edge\n"
     "            if(t.p_vs) ++pvs;\n"
     "            ++row_gclk;\n"
     "            if(prev_hs && !t.lcd_hs) {\n"
     "                if(!prow.empty()) { if(prow.size()!=720) ++bad_de; else prows.push_back(prow);\n"
     "                    if(row_gclk!=912 && prows.size()>1) ++bad_period; }\n"
     "                prow.clear(); row_gclk=0;\n"
     "            }\n"
     "            if(t.lcd_de) prow.push_back(t.lcd_db);\n"
     "            prev_hs=t.lcd_hs;\n"
     "        }\n"
     "        t.gclk=((cycle+(gphase?2:0))&3)<2;\n"
     "        t.hclk=!(cycle&1); t.pclk=1; t.eval();"),
    ('    if(content) require(colors.size()>1,"Last frame has a single color");',
     '    if(content) require(colors.size()>1,"Last frame has a single color");\n'
     '    const bool panel_ok=compared>0 && !panel_missed && !panel_extra && !bad_de && !bad_rows && !bad_image && !bad_addr && (waits ? true : !bad_period);'),
])
