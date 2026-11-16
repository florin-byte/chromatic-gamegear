#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
case "${1:-}" in
build)
    gg=esp32t/src/rtl/EMU/GG
    verilator --cc --exe --build -j 4 -O3 -Wno-fatal --top-module tb_gg +1364-2005ext+v +1800-2017ext+sv \
        --Mdir sim_gg/out/build --output-split 16000 --output-split-cfuncs 300 \
        -CFLAGS '-O3' sim_gg/tb_gg.sv sim_gg/gen/system_sim.v sim_gg/gen/video_sim.v \
        "$gg"/gg_*.v "$gg"/sms_mister/jt89/*.v "$gg"/sms_mister/MC8123.v \
        "$gg"/sms_mister/SEGASYS1_PRGDEC.v "$gg"/sms_mister/parts.v "$gg"/sms_mister/cheatcodes.sv \
        esp32t/src/rtl/EMU/emu_system_top.v esp32t/src/rtl/EMU/ramcart.v \
        sim_gg/audio_filter_stub.v "$(pwd)/sim_gg/main.cpp"
    ;;
check) shift; exec sim_gg/out/build/Vtb_gg "$@" ;;
nomagic) exec sim_gg/out/build/Vtb_gg --nomagic sim_gg/roms/test_palette.gg 10 ;;
noload) exec sim_gg/out/build/Vtb_gg --noload sim_gg/roms/test_palette.gg 10 ;;
*) echo 'Usage: run.sh build | check [--content] rom.gg frames [start_frame] | nomagic' >&2; exit 2 ;;
esac
