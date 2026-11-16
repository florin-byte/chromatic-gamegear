#!/bin/zsh
# Open-source build of the Chromatic FPGA (Yosys + GHDL + nextpnr-himbaechel +
# Apicula). No Gowin tools. Output: open_build/out/top.fs (load to SRAM only).
#
#   open_build/build_open.sh [synth|pnr|pack|all]
#
# Differences from the Gowin build (src/open/):
#   fifo1k_open.v         replaces the encrypted fifo1k IP
#   usbuvcuart_stub.v     replaces the USB block (encrypted PHY + controller):
#                         no USB device, ESP32 bridge idle
#   eReg_SavestateV.v     Verilog port of the VHDL register (per-instance
#                         generics from Verilog parents)
#
# PLL (2026-10-09): Apicula's GW5A-25A pad->PLL table maps every PLL pad to
# X0Y0, so the first open bitstream put the main PLL in the top-left PLL
# (slot 5) on a dedicated input with no clock: CLKIN was disconnected, the
# design never ran, the ESP32 saw zero packets. The stock (Gowin) bitstream
# uses slot 8 (bottom PLL, X45Y36) with A_CLKIN_SEL=CLKIN2 fed by CLK_FPGA on
# M2/IOB89A (BPLL_T_IN0); every other PLL param matched ours. Fixed in
# patches/nextpnr_gowin.patch (pad map) + patches/apicula_gowin_pack.patch
# (honor INSEL for PLLA). Check slots with tools/pll_slots.py.
set -e
here=${0:A:h}
root=${here:h}
oss=${THIRD_PARTY:?set_THIRD_PARTY_to_the_dir_holding_oss-cad-suite_nextpnr_src_apicula_patched}/oss-cad-suite
export PATH=$oss/bin:$PATH
export GHDL_PREFIX=$oss/lib/ghdl
out=$here/out
mkdir -p $out
step=${1:-all}

cd $root
files=(${(f)"$(grep -o 'path="[^"]*"' evt1_x2.gprj | sed 's/path="//;s/"$//')"})
vhd=()
vlog=()
for f in $files; do
    case $f in
        src/fifo1k/*|src/rtl/USB/*) ;;                      # replaced
        *.vhd) vhd+=$f ;;
        *.v|*.sv) vlog+=$f ;;
    esac
done
vlog+=(src/open/fifo1k_open.v src/open/usbuvcuart_stub.v src/open/eReg_SavestateV.v src/open/gw5a_bufg_bb.v)

if [[ $step == synth || $step == all ]]; then
    work=$out/ghdl_work
    # Fresh library every build (stale units make dependents "obsolete").
    [[ -d $work ]] && mv $work $out/.ghdl_work_old_$$
    mkdir -p $work
    # Analyze in dependency order: retry failures until a pass makes no
    # progress (the project file lists VHDL alphabetically).
    pending=($vhd)
    while (( ${#pending} )); do
        failed=()
        for f in $pending; do
            ghdl -a --std=08 -fsynopsys --workdir=$work $f 2>/dev/null || failed+=$f
        done
        if (( ${#failed} == ${#pending} )); then
            for f in $failed; do ghdl -a --std=08 -fsynopsys --workdir=$work $f; done
            exit 1
        fi
        pending=($failed)
    done
    # VHDL entities instantiated from Verilog, with the generics their
    # parents pass (others use defaults that match).
    ghdl_units=(
        "GBse"
        "gb_savestates"
        "-gSoftmap_SaveState_ADDR=58720256 -gSoftmap_Rewind_ADDR=33554432 gb_statemanager"
        "gbc_snd"
        "MultiPortRamCtrl"
        "PSRAMBIST_Burst"
        "overlayBatteryBack" "overlayBatteryFront" "overlayDebug"
        "overlayTimerBack" "overlayTimerFront" "overlayTimerNumber"
        "speedcontrol"
        "-gP_PARITY_BIT=odd UART_RX" "UART_TX"
    )
    # GHDL -> Verilog per unit (the yosys-ghdl plugin crashes on GBse).
    vv=$out/vhdl_v
    mkdir -p $vv
    vhdl_v=()
    for u in $ghdl_units; do
        name=${u##* }
        ghdl --synth --std=08 -fsynopsys --latches --workdir=$work --out=verilog ${=u} > $vv/$name.v
        vhdl_v+=$vv/$name.v
    done
    # GHDL bakes generics in (values above match the parents); the Verilog
    # parents still pass them, so give those modules inert parameters.
    typeset -A inert=(
        PSRAMBIST_Burst  "parameter BIST_BURSTLENGTH = 0, parameter SHORTTEST = 0"
        MultiPortRamCtrl "parameter ISSIMU = 0"
        UART_RX          "parameter P_PARITY_BIT = \"none\""
        gb_statemanager  "parameter Softmap_SaveState_ADDR = 0, parameter Softmap_Rewind_ADDR = 0"
    )
    for m p in ${(kv)inert}; do
        sed -i '' "s/^module $m\$/module $m #($p)/" $vv/$m.v
    done
    # The PSRAM IODELAYs are static (C_STATIC_DLY, DLYSTEP tied 0); nextpnr's
    # IODELAY packer fails on a constant DLYSTEP, so leave it unconnected.
    sed -i '' "/^ *\.DLYSTEP(8'b00000000),\$/d" $vv/MultiPortRamCtrl.v
    {
        # Shared sub-entities appear in several units under the same
        # generic-hashed name; -overwrite keeps one.
        echo "read_verilog -overwrite $vhdl_v"
        echo "read_verilog -sv -D OPEN_BUILD -I src -I src/rtl/BSP -I src/rtl/EMU -I src/rtl/EMU/CORE $vlog"
        echo "synth_gowin -family gw5a -nolutram -top top -json $out/top.json"
        echo "stat"
    } > $out/synth.ys
    yosys -l $out/synth.log -q $out/synth.ys
fi

if [[ $step == pnr || $step == all ]]; then
    # nextpnr's CST parser needs quoted port names (5 IO_PORT lines lack them).
    # USB is stubbed: its pins become plain high-Z inputs, no pulls (the host
    # sees no device; avoids the LVDS25-in-a-3.3V-bank pairing check).
    sed -E -e 's/^IO_PORT ([A-Za-z_][A-Za-z0-9_]*) /IO_PORT "\1" /' \
        -e 's/^IO_PORT "usb_[a-z_]+" .*$/&__USB__/' \
        src/board/evt1_x2/evt1_x2.cst \
        | sed -E 's/^IO_PORT ("usb_[a-z_]+") .*__USB__$/IO_PORT \1 IO_TYPE=LVCMOS33 PULL_MODE=NONE;/' \
        | sed -E 's/^IO_PORT ("PS_[A-Z_]+(\[[0-9]\])?") IO_TYPE=SSTL18_I/IO_PORT \1 IO_TYPE=LVCMOS18/' > $out/board.cst
    # Apicula allows one IO_TYPE per bank and its SSTL path is incomplete; the
    # PSRAM bank (1.8 V supply) also holds LVCMOS18 inputs (CLK_FPGA,
    # VERSION_DET2), so the PSRAM pins use LVCMOS18: same 1.8 V push-pull.
    # Patched nextpnr (third_party/nextpnr_src, 861c57be + IDES4 pair-check
    # exemption): stock nextpnr refuses IDES4 on both pins of an IO pair,
    # which the PSRAM DQ/DQS pins need and Gowin's tools allow.
    ${THIRD_PARTY:?set_THIRD_PARTY_to_the_dir_holding_oss-cad-suite_nextpnr_src_apicula_patched}/nextpnr_src/build/nextpnr-himbaechel --json $out/top.json --write $out/top_pnr.json \
        --device GW5A-EV25UG256CC1/I0 \
        --vopt family=GW5A-25A \
        --vopt cst=$out/board.cst \
        --vopt sspi_as_gpio --vopt i2c_as_gpio \
        -l $out/pnr.log
fi

if [[ $step == pack || $step == all ]]; then
    # Patched Apicula (third_party/apicula_patched): unused DOUT config pin
    # stays an input when SSPI/MSPI are GPIO, as in the stock bitstream.
    PYTHONPATH=${THIRD_PARTY:?set_THIRD_PARTY_to_the_dir_holding_oss-cad-suite_nextpnr_src_apicula_patched}/apicula_patched \
    tabbypy3 -c 'from apycula.gowin_pack import main; main()' -d GW5A-25A -o $out/top.fs \
        --sspi_as_gpio --mspi_as_gpio --i2c_as_gpio --cpu_as_gpio \
        $out/top_pnr.json
fi
