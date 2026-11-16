#!/usr/bin/env python3
"""Second part of patch_panel_sim.py: report the panel checks and fail the run when they fail.

Usage: patch_panel_sim2.py <working copy root>
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


edit("sim_gg/main.cpp", [
    ('      <<"\\nlast_frame_colors="<<colors.size()<<"\\nmaximum_buffered_pixels="<<max_buffer<<"\\nnomagic="<<nomagic<<"\\n";',
     '      <<"\\nlast_frame_colors="<<colors.size()<<"\\nmaximum_buffered_pixels="<<max_buffer<<"\\nnomagic="<<nomagic\n'
     '      <<"\\npanel_frames_compared="<<compared<<"\\npanel_frame_starts_missed="<<panel_missed\n'
     '      <<"\\npanel_frame_starts_extra="<<panel_extra<<"\\npanel_rows_not_912_gclk="<<bad_period\n'
     '      <<"\\npanel_rows_de_not_720="<<bad_de<<"\\npanel_frames_not_144_rows="<<bad_rows\n'
     '      <<"\\npanel_frames_image_mismatch="<<bad_image<<"\\nframe_buffer_address_errors="<<bad_addr\n'
     '      <<"\\npanel_rows_waited="<<(t.panel_dbg&0xffff)<<"\\npanel_shift="<<lock_shift<<"\\npanel_perm="<<lock_perm\n'
     '      <<"\\ngclk_phase="<<gphase<<"\\npanel_ok="<<panel_ok<<"\\n";'),
    ('<<" colors="<<colors.size()<<" buffer="<<max_buffer<<"\\n";\n    t.final(); return 0;',
     '<<" colors="<<colors.size()<<" buffer="<<max_buffer<<"\\n";\n'
     '    std::cout<<"panel: compared="<<compared<<" missed="<<panel_missed<<" extra="<<panel_extra<<" rows!=912="<<bad_period\n'
     '      <<" de!=720="<<bad_de<<" frames!=144rows="<<bad_rows<<" image_mismatch="<<bad_image<<" addr_errors="<<bad_addr\n'
     '      <<" gphase="<<gphase<<" ok="<<panel_ok<<"\\n";\n'
     '    t.final(); return panel_ok ? 0 : 3;'),
    ('(noload?"noload":nomagic?"nomagic":rompath.stem().string())',
     '((noload?std::string("noload"):nomagic?std::string("nomagic"):rompath.stem().string())+(gphase?"-g1":""))'),
])

run = root / "sim_gg/run.sh"
text = io.open(run, encoding="utf-8", newline="").read()
old_build = text[text.index("build)"):text.index("check)")]
if "BSP/vid_system_top.sv" in text:
    raise SystemExit("run.sh already patched")
new_build = '''build|build-gg2)
    gg=esp32t/src/rtl/EMU/GG
    bsp=esp32t/src/rtl/BSP
    # build-gg2: the adapter as installed in gg2 (frame start one hclk wide), to prove the test catches it
    if [ "$1" = build-gg2 ]; then vout=sim_gg/gg_video_out_gg2.v; mdir=sim_gg/out/build-gg2; else vout="$gg"/gg_video_out.v; mdir=sim_gg/out/build; fi
    verilator --cc --exe --build -j 4 -O3 -Wno-fatal --top-module tb_gg +1364-2005ext+v +1800-2017ext+sv \\
        --Mdir "$mdir" --output-split 16000 --output-split-cfuncs 300 \\
        -CFLAGS '-O3' sim_gg/tb_gg.sv sim_gg/gen/system_sim.v sim_gg/gen/video_sim.v \\
        "$gg"/gg_ce_gen.v "$gg"/gg_ram.v "$gg"/gg_rom_loader.v "$vout" "$gg"/sms_mister/jt89/*.v "$gg"/sms_mister/MC8123.v \\
        "$gg"/sms_mister/SEGASYS1_PRGDEC.v "$gg"/sms_mister/parts.v "$gg"/sms_mister/cheatcodes.sv \\
        esp32t/src/rtl/EMU/emu_system_top.v esp32t/src/rtl/EMU/ramcart.v \\
        "$bsp"/vid_system_top.sv "$bsp"/ST7785_panel_master.v "$bsp"/ST7785_init.v sim_gg/bsp_stubs.v \\
        sim_gg/audio_filter_stub.v "$(pwd)/sim_gg/main.cpp"
    ;;
'''
text = text.replace(old_build, new_build)
text = text.replace("check) shift; exec sim_gg/out/build/Vtb_gg \"$@\" ;;",
                    "check) shift; exec \"${SIM_BIN:-sim_gg/out/build/Vtb_gg}\" \"$@\" ;;")
text = text.replace("nomagic) exec sim_gg/out/build/Vtb_gg --nomagic", "nomagic) exec \"${SIM_BIN:-sim_gg/out/build/Vtb_gg}\" --nomagic")
text = text.replace("noload) exec sim_gg/out/build/Vtb_gg --noload", "noload) exec \"${SIM_BIN:-sim_gg/out/build/Vtb_gg}\" $SIM_OPTS --noload")
io.open(run, "w", encoding="utf-8", newline="").write(text)
print("patched sim_gg/run.sh")
