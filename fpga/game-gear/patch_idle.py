#!/usr/bin/env python3
"""One-off edit of the CG-102 working copy: keep the menu usable with no Game Gear game loaded.

top.v disables the menu while the core reports a reset (appear_off). The first integration held the core
in reset whenever no game was loaded, so a game could never be chosen. After this edit the core is only
reset where the stock Game Boy design resets its core (and while a game's ROM is replayed); with no game
(RAM cart off, or an image without the GGCHROM1 marker) it runs idle on a ROM of zeros and shows black.

Usage: patch_idle.py <working copy root>
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
            raise SystemExit("%s: expected exactly one match for: %s" % (rel, old[:70]))
        text = text.replace(old, new)
    io.open(path, "w", encoding="utf-8", newline="").write(text)
    print("patched", rel)


edit("esp32t/src/rtl/EMU/emu_system_top.v", [
    ("""    // reset_n comes from another clock domain (memrst, xclk2) and the RAM cart flags from hclk:
    // synchronize the reload condition into pclk before it reaches the core's clock enables.
    wire reload_raw = !reset_n || !POWER_GOOD || !LCD_INIT_DONE || !rc_on || rc_hold ||
                  (MENU_CLOSED && BTN_MENU && BTN_A && BTN_B && BTN_START && BTN_SEL);
    reg [1:0] reload_s = 2'b11;
    always @(posedge pclk) reload_s <= {reload_s[0], reload_raw};
    wire reload = reload_s[1];
    wire loaded;
""",
     """    // Hard reset: exactly the conditions that reset the Game Boy core in the stock design. top.v turns
    // the menu off while o_emulator_reset is high (appear_off), so "no game loaded" must not be a reset:
    // the core then runs idle on a ROM of zeros (black screen), like the stock design with an empty slot.
    // reset_n comes from another clock domain (memrst, xclk2) and the RAM cart flags from hclk:
    // everything is synchronized into pclk before it reaches the core's clock enables.
    wire hard_raw = !reset_n || !POWER_GOOD || !LCD_INIT_DONE || rc_hold ||
                    (!latched_cart_rst_n && !rc_on) ||
                    (MENU_CLOSED && BTN_MENU && BTN_A && BTN_B && BTN_START && BTN_SEL);
    reg [1:0] hard_s = 2'b11, on_s = 2'b00;
    always @(posedge pclk) begin
        hard_s <= {hard_s[0], hard_raw};
        on_s <= {on_s[0], rc_on};
    end
    wire hard = hard_s[1];
    wire reload = hard || !on_s[1];       // the loader starts over
    wire loaded, bad;
    wire idle = !on_s[1] || bad;          // no Game Gear game: RAM cart off, or an image without the marker
"""),
    ("        .loaded(loaded), .rom_do(rom_do),", "        .loaded(loaded), .bad(bad), .rom_do(rom_do),"),
    ("    wire core_reset = reload || !loaded;", "    wire core_reset = hard || (!idle && !loaded);   // hard reset, or a game's ROM being replayed"),
    ("    wire run = !core_reset && !stall_r;", "    wire run = !core_reset && (idle || !stall_r);"),
    ("        .color(color), .hb(hb), .vb(vb),", "        .color(idle ? 12'd0 : color), .hb(hb), .vb(vb),"),
    ("        snd_l <= core_reset ? 16'd0 : audio_l;\n        snd_r <= core_reset ? 16'd0 : audio_r;",
     "        snd_l <= (core_reset || idle) ? 16'd0 : audio_l;\n        snd_r <= (core_reset || idle) ? 16'd0 : audio_r;"),
    ("        .rom_do(rom_do),\n", "        .rom_do(idle ? 8'h00 : rom_do),\n"),
])
edit("esp32t/src/rtl/EMU/GG/gg_rom_loader.v", [
    ("    output reg loaded = 0, output [7:0] rom_do,", "    output reg loaded = 0, output bad, output [7:0] rom_do,"),
    ("    assign rom_do = q;", "    assign rom_do = q;\n    assign bad = state == BAD;"),
])
edit("sim_gg/main.cpp", [
    ("    bool content=false, nomagic=false;", "    bool content=false, nomagic=false, noload=false;"),
    ('        else if(opt=="--nomagic") nomagic=true;',
     '        else if(opt=="--nomagic") nomagic=true;\n        else if(opt=="--noload") { nomagic=true; noload=true; }'),
    ("    const uint64_t limit=nomagic ? 2000000 : uint64_t(wanted+20)*1500000 + rom.size()*64;",
     "    const uint64_t limit=uint64_t(wanted+20)*1500000 + rom.size()*64;"),
    ("t.rc_on=cycle>100;", "t.rc_on=!noload && cycle>100;"),
    ('        if(nomagic && cycle>400 && (t.running || !t.core_reset)) throw std::runtime_error("No-magic image released core reset");',
     '        if(nomagic && t.running) throw std::runtime_error("An image without the marker was started as a game");'),
    ('    if(nomagic) require(frames==0 && t.core_reset && !t.running,"Negative test failed");\n    else require(',
     '    if(nomagic) require(frames==wanted && !t.core_reset && !t.running,"Idle test failed: frames="+std::to_string(frames));\n    else require('),
    ('    if(content) require(colors.size()>1,"Last frame has a single color");',
     '    if(content) require(colors.size()>1,"Last frame has a single color");\n'
     '    if(nomagic) require(colors.size()==1 && *colors.begin()==0,"Idle frame is not black");'),
    ('(nomagic?"nomagic":rompath.stem().string())', '(noload?"noload":nomagic?"nomagic":rompath.stem().string())'),
])
edit("sim_gg/run.sh", [
    ("nomagic) exec sim_gg/out/build/Vtb_gg --nomagic sim_gg/roms/test_palette.gg 0 ;;",
     "nomagic) exec sim_gg/out/build/Vtb_gg --nomagic sim_gg/roms/test_palette.gg 10 ;;\n"
     "noload) exec sim_gg/out/build/Vtb_gg --noload sim_gg/roms/test_palette.gg 10 ;;"),
])
edit("sim_gg/accept.sh", [
    ("    run nomagic nomagic\n", "    run nomagic nomagic\n    run noload noload\n"),
])
