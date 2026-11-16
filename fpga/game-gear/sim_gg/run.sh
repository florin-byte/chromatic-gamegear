#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
case "${1:-}" in
build|build-gg2)
    gg=esp32t/src/rtl/EMU/GG
    bsp=esp32t/src/rtl/BSP
    # build-gg2: the adapter as installed in gg2 (frame start one hclk wide), to prove the test catches it
    if [ "$1" = build-gg2 ]; then vout=sim_gg/gg_video_out_gg2.v; mdir=sim_gg/out/build-gg2; else vout="$gg"/gg_video_out.v; mdir=sim_gg/out/build; fi
    verilator --cc --exe --build -j 4 -O3 -Wno-fatal --top-module tb_gg +1364-2005ext+v +1800-2017ext+sv \
        --Mdir "$mdir" --output-split 16000 --output-split-cfuncs 300 \
        -CFLAGS '-O3' sim_gg/tb_gg.sv sim_gg/gen/system_sim.v sim_gg/gen/video_sim.v \
        "$gg"/gg_ce_gen.v "$gg"/gg_ram.v "$gg"/gg_rom_loader.v "$vout" "$gg"/sms_mister/jt89/*.v "$gg"/sms_mister/MC8123.v \
        "$gg"/sms_mister/SEGASYS1_PRGDEC.v "$gg"/sms_mister/parts.v "$gg"/sms_mister/cheatcodes.sv \
        esp32t/src/rtl/EMU/emu_system_top.v esp32t/src/rtl/EMU/ramcart.v \
        "$bsp"/vid_system_top.sv "$bsp"/ST7785_panel_master.v "$bsp"/ST7785_init.v sim_gg/bsp_stubs.v \
        sim_gg/audio_filter_stub.v "$(pwd)/sim_gg/main.cpp"
    ;;
check) shift; exec "${SIM_BIN:-sim_gg/out/build/Vtb_gg}" "$@" ;;
nomagic) exec "${SIM_BIN:-sim_gg/out/build/Vtb_gg}" --nomagic sim_gg/roms/test_palette.gg 10 ;;
noload) exec "${SIM_BIN:-sim_gg/out/build/Vtb_gg}" ${SIM_OPTS:-} --noload sim_gg/roms/test_palette.gg 10 ;;
*) echo 'Usage: run.sh build | check [--content] rom.gg frames [start_frame] | nomagic' >&2; exit 2 ;;
esac
