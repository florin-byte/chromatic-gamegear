#!/bin/zsh
# OWNER APPROVAL REQUIRED. NOT RUN BY THE cartflash1 AGENT (it never touches the device).
#
# Hardware step 1 of CART_FLASH_DESIGN.md: SRAM-load the TEST bitstream (never writes the
# FPGA's flash; a power cycle returns to the bitstream in flash). The netboot boot ROM then
# fetches the READ-ONLY probe from the owner's Mac (cart_probe_rx.py) and dumps the inserted
# cart (ModRetro Tetris) to the Mac: header, all ROM banks, the save FRAM. Reads plus the
# ordinary MBC register writes every game makes; no flash command is written.
#
# Before running, by hand:
#  1. the device belongs to this test (no other agent using the serial port / FPGA cable);
#  2. note the current `nw url` (console), then `nw url http://<this Mac's LAN IP>:8791/gbnet/v1`;
#  3. start the receiver from Terminal (LAN-facing):
#       python3 cart_probe_rx.py --host 0.0.0.0 --port 8791 --rom-banks 128 --ram-banks 4 --ram-kb 8
#     (--rom-banks 128 = the whole 2 MiB SST39VF168x chip; 256 for a 4 MiB chip)
#  4. the Tetris cart is in, the console is on, battery charged.
# After: power-cycle the Chromatic (back to the flashed bitstream), restore `nw url`.
#
#   CARTFLASH_OWNER_APPROVED=step1 CARTFLASH_BUSDEV=2:4 ./load_netboot_sram.sh
# CARTFLASH_BUSDEV pins the cable to one unit. openFPGALoader 1.1.1 swaps the halves of
# --busdev-num (it compares the first number with the device address): unit 1's GWU2X on
# USB bus 4 address 2 is `--busdev-num 2:4` (device session 2026-10-10).
#
# PREFERRED FLOW (no Wi-Fi, release ESP32 builds with CONFIG_NW_CARTFLASH): after the load,
# with tools/console_daemon.py holding the console open, run
#   python3 ../host/cart_probe_usb.py --daemon --rom-banks 128 --ram-banks 4 --ram-kb 8
# It sends `cart probe`, the ESP32 power-cycles the cart and serves the probe from CF_LOAD
# (no race with the ESP32's reboot after the SRAM load), and the dump comes over USB.
# Steps 2-3 above (nw url, LAN receiver) are then not needed.
set -u
[[ ${CARTFLASH_OWNER_APPROVED:-} == step1 ]] || { echo "refused: set CARTFLASH_OWNER_APPROVED=step1 (owner approval, see CART_FLASH_DESIGN.md)"; exit 1; }
FS=${CWL_ROOT:?set_CWL_ROOT_to_your_work_folder}/fpga_cartflash/esp32t/build/evt1_x2_nwin_netboot.fs
[[ -e $FS ]] || { echo "no $FS (build_netboot_fs.sh)"; exit 1; }
shasum -a 256 $FS
echo "SRAM load (openFPGALoader -m = write SRAM only, never flash):"
if [[ -n ${CARTFLASH_BUSDEV:-} ]]; then
    exec openFPGALoader --cable gwu2x --busdev-num $CARTFLASH_BUSDEV -m $FS
fi
exec openFPGALoader --cable gwu2x -m $FS
