#!/usr/bin/env bash
# Regenerates sim_gg/gen/*.v from the VHDL core with GHDL (Windows: Git Bash, GHDL 6).
# The generics are the ones emu_system_top.v passes: SYSTEM_E=0, MAX_SPPL=7, EXTERNAL_BIOS=0.
set -eu
cd "$(dirname "$0")/.."
GHDLX="${GHDL:-ghdl}"
V=esp32t/src/rtl/EMU/GG
OUT=sim_gg/gen
rm -rf "$OUT/work"; mkdir -p "$OUT/work"
FILES="$V/gowin_ram.vhd $V/opll_stub.vhd"
for f in T80/T80_Reg T80/T80_MCode T80/T80_ALU T80/T80 T80/T80s T80/T80pa jt89/jt89 video vdp_sprite_shifter \
         vdp_sprites vdp_main vdp_cram vdp_background vdp mapper_detect mapper_ctrl memory_decode memory_bus \
         evolution_rom_address evolution_mapper eeprom_93c46 cart_eeprom io AudioMix system; do
    FILES="$FILES $V/sms_mister/$f.vhd"
done
G="--std=08 -frelaxed -fsynopsys --workdir=$OUT/work"
"$GHDLX" -i $G $FILES > "$OUT/ghdl.txt" 2>&1
"$GHDLX" -m $G system >> "$OUT/ghdl.txt" 2>&1
"$GHDLX" -m $G video >> "$OUT/ghdl.txt" 2>&1
"$GHDLX" --synth $G --out=verilog -gSYSTEM_E=0 -gMAX_SPPL=7 -gEXTERNAL_BIOS=0 system > "$OUT/system_sim.v" 2>> "$OUT/ghdl.txt"
"$GHDLX" --synth $G --out=verilog video > "$OUT/video_sim.v" 2>> "$OUT/ghdl.txt"
# Never-written memories come out as huge X constants, beyond Verilator's number width limit.
perl -pi -e "s/= \d{5,}'bX;/= '0;/" "$OUT/system_sim.v"
rm -rf "$OUT/work"
wc -l "$OUT"/*.v
