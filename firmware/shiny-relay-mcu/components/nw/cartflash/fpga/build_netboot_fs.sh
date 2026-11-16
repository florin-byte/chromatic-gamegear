#!/bin/zsh
# TEST-ONLY bitstream for the cart probe (lane cartflash1): FPGA 18.21 (network window) with
# the netboot test boot ROM (sm83/cf_netboot.inc inside the stock CGB boot ROM). The boot ROM
# itself never touches the cartridge: it fetches a WRAM program over the network window
# (the read-only probe from the owner's Mac, or later the writer from the ESP32) and returns
# to a normal boot when there is none.
#
# Builds in the cartflash1 fpga worktree only (never the base fpga tree, which other agents
# use), swapping the boot ROM vmem for the build and restoring it after. Output:
#   ${CWL_ROOT:?set_CWL_ROOT_to_your_work_folder}/fpga_cartflash/esp32t/build/${OUT_NAME:-evt1_x2_nwin_netboot}.fs
# NEVER flash it: SRAM load only (load_netboot_sram.sh, owner approval). A power cycle of
# the Chromatic returns to the bitstream in its flash.
set -u
FPGA=${CWL_ROOT:?set_CWL_ROOT_to_your_work_folder}/fpga_cartflash
HERE=${0:A:h}
SM83=$HERE/../sm83
V=$FPGA/esp32t/src/rtl/EMU/CORE/BootROMs/cgb_boot.mif.vmem
ORIG=${CWL_ROOT:?set_CWL_ROOT_to_your_work_folder}/tools/nw/bootrom/cgb_boot.mif.vmem.orig
[[ $(git -C $FPGA rev-parse --abbrev-ref HEAD) == cartflash1* ]] || { echo "fpga worktree is not on cartflash1, stop"; exit 1; }
cmp -s $V $ORIG || { echo "boot ROM vmem in $FPGA is not stock, stop"; exit 1; }
sh $SM83/make_sm83.sh > /dev/null || { echo "make_sm83.sh failed"; exit 1; }
cp $SM83/build/netboot.mif.vmem $V
cd $FPGA/esp32t
GOWIN_PLACE_OPTION=${GOWIN_PLACE_OPTION:-2} GOWIN_APP=${CWL_ROOT:?set_CWL_ROOT_to_your_work_folder}/third_party/gowin_full/GowinIDE.app ${CWL_ROOT:?set_CWL_ROOT_to_your_work_folder}/tools/gw_sh.sh build.tcl
rc=$?
cp $ORIG $V
git -C $FPGA diff --quiet -- $V || { echo "WARNING: vmem not restored"; rc=1; }
if [[ $rc == 0 ]]; then
    mkdir -p build
    [[ -e build/${OUT_NAME:-evt1_x2_nwin_netboot}.fs ]] && { mkdir -p $FPGA/.trash; mv -f build/${OUT_NAME:-evt1_x2_nwin_netboot}.fs $FPGA/.trash/${OUT_NAME:-evt1_x2_nwin_netboot}_$(date +%s).fs; }
    cp impl/pnr/evt1_x2.fs build/${OUT_NAME:-evt1_x2_nwin_netboot}.fs
    shasum -a 256 build/${OUT_NAME:-evt1_x2_nwin_netboot}.fs $SM83/build/netboot.mif.vmem $SM83/build/cartprobe.bin
    grep -E "Setup|Hold|BSRAM|Logic" impl/pnr/evt1_x2.rpt.txt 2>/dev/null | head -8
fi
echo "BUILD rc=$rc"
