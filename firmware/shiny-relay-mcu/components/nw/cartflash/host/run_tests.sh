#!/bin/sh
# All cart flash simulations (lane cartflash1). Builds, runs every cf_sim scenario, and the
# read-only probe against a real cart_probe_rx.py on 127.0.0.1 (a free port, never a
# reserved one), then checks the receiver's dump byte for byte against the simulated cart.
set -u
cd "$(dirname "$0")"
make -s all >/dev/null 2>&1 || { echo "build failed"; make all; exit 1; }
HERE=$(pwd)
BOOT=$HERE/../sm83/build/netboot_sameboy.bin
T=$(mktemp -d "${TMPDIR:-/tmp}/cartflash_sim.XXXXXX")
pass=0; fail=0; failed=""
one() {
    name=$1; shift
    if "$@" > "$T/$name.log" 2>&1; then pass=$((pass + 1)); r=PASS; else fail=$((fail + 1)); failed="$failed $name"; r=FAIL; fi
    printf "%-16s %s  %s\n" "$name" "$r" "$(grep -o 'stats: [0-9.]* s emulated' "$T/$name.log" | head -1)"
}
for s in install loss cut_backup cut_erase cut_header cut_program verify_fail wrong_chip gated unverified \
         bad_image other_cart esp_reboot header_mapper roundtrip ram_partial \
         selftest rt_tetris hw_sequence no_engine erase_timeout \
         ram_engine ram_cut bootblock gbtest gbtest_miss save_rt gbt_diy gbt_modretro foreign \
         strm_ok strm_drop strm_pause strm_cut_erase strm_cut_blk0 strm_verify strm_gone strm_2mib strm_2mib_top strm_wstg strm_wstg_cut \
         ovl_stream ovl_stuck_usb ovl_stuck_stream; do
    one $s ./cf_sim --scenario $s --boot "$BOOT"
done
# cartspeed1: an 18.26 FPGA with the express stream (the FPGA writes the cart; QSPI ring + UART ops)
one xs_wstg ./cf_sim --scenario strm_wstg --boot "$BOOT" --xs
one xs_wstg_lose ./cf_sim --scenario strm_wstg --boot "$BOOT" --xs --xs-lose 7
one xs_wstg_flip ./cf_sim --scenario strm_wstg --boot "$BOOT" --xs --xs-flip 300000
one xs_wstg_t2 ./cf_sim --scenario strm_wstg --boot "$BOOT" --xs --xs-timing 2
one xs_wstg_cut ./cf_sim --scenario strm_wstg_cut --boot "$BOOT" --xs
one xs_cut_prog ./cf_sim --scenario strm_xs_cut_prog --boot "$BOOT" --xs
one xs_stuck ./cf_sim --scenario strm_xs_stuck --boot "$BOOT" --xs
for s in strm_wprog strm_wprog_bad strm_wprog_cut strm_wprog_cut_diff strm_wprog_stale; do one xs_$s ./cf_sim --scenario $s --boot "$BOOT" --xs; done
# the owner's USB backup / restore (protocol CBU1): the reference tool against the device's code
usbcbu() {   # name, tool args...
    name=$1; shift
    d=$T/$name; mkdir -p "$d"
    ( cd "$d" && "$HERE/cf_sim" --scenario ${SC:-$name} --boot "$BOOT" ${SIMX:-} --usb-tool "python3 -I $HERE/cart_usb.py --stdio $* 2>$d/tool.log" ) > "$T/$name.log" 2>&1
}
one usb_backup usbcbu usb_backup backup --rom out.gb --sav out.sav
one usb_backup_noise usbcbu usb_backup_noise backup --rom out.gb --sav out.sav
one usb_restore usbcbu usb_restore --timeout 6 restore --rom restore.gb --sav restore.sav
one usb_restore_cut usbcbu usb_restore_cut --timeout 6 restore --rom restore.gb --sav restore.sav
# usb-cart-1: the staged install (ROM + save in the ESP32's flash, written there in bootloader mode)
stg() {   # log name, scenario, tool args...
    name=$1; sc=$2; shift 2
    d=$T/$name; mkdir -p "$d"
    ( cd "$d" && "$HERE/cf_sim" --scenario $sc --boot "$BOOT" ${SIMX:-} --rx-repl --usb-wait --usb-tool "python3 -I $HERE/cart_usb.py --stdio --timeout 30 install $* --rom restore.gb --sav restore.sav 2>$d/tool.log" ) > "$T/$name.log" 2>&1
}
one stg_ok stg stg_ok stg_ok
one stg_fast stg stg_fast stg_ok --fast
one stg_cut stg stg_cut stg_cut
one stg_blank stg stg_blank stg_blank
one stg_bad stg stg_bad stg_bad
one stg_big stg stg_big stg_big
one stg_online stg stg_online stg_online
SIMX="--xs" one xs_stg_ok stg stg_ok stg_ok
SIMX="--xs" one xs_stg_cut stg stg_cut stg_cut
# cart jobs while a game patch is ACTIVE (the FPGA's overlay): held off for the job, the cart's own bytes
one ovl_backup usbcbu ovl_backup backup --rom out.gb --sav out.sav
one ovl_restore usbcbu ovl_restore --timeout 6 restore --rom restore.gb --sav restore.sav
# the firmware before the hold must FAIL these three (it read the patched view): the scenarios do test it
legacy() {   # scenario, tool args (or none)
    lsc=$1; shift
    d=$T/${lsc}_legacy; mkdir -p "$d"
    if [ $# -gt 0 ]; then ( cd "$d" && "$HERE/cf_sim" --scenario $lsc --boot "$BOOT" --legacy-no-hold --usb-tool "python3 -I $HERE/cart_usb.py --stdio $* 2>$d/tool.log" ) > "$d/sim.log" 2>&1
    else "$HERE/cf_sim" --scenario $lsc --boot "$BOOT" --legacy-no-hold > "$d/sim.log" 2>&1; fi
    [ $? -ne 0 ] && grep -q "from the overlay" "$d/sim.log"
}
one ovl_backup_old_fails legacy ovl_backup backup --rom out.gb --sav out.sav
one ovl_restore_old_fails legacy ovl_restore --timeout 6 restore --rom restore.gb --sav restore.sav
one ovl_stream_old_fails legacy ovl_stream
one usb_savonly usbcbu usb_savonly --timeout 6 savonly --sav restore.sav
one stg_patch stg stg_patch stg_ok
# 1.0.2: the SST39VF1681 (Tetris carts) behind the bottom block's write test, WP# high and low
for s in wp_install wp_install_locked wp_selftest_locked wp_stream wp_stream_locked wp_noff wp_noff_locked wp_zero wp_hdr \
         wp_cut_test wp_cut_erase0 wp_neither wp_neither_install; do
    one $s ./cf_sim --scenario $s --boot "$BOOT"
done
SIMX="--chip 1681"
SC=usb_backup one c81_backup usbcbu c81_backup backup --rom out.gb --sav out.sav
SC=usb_restore one c81_restore usbcbu c81_restore --timeout 6 restore --rom restore.gb --sav restore.sav
SC=usb_restore_cut one c81_restore_cut usbcbu c81_restore_cut --timeout 6 restore --rom restore.gb --sav restore.sav
SC=usb_savonly one c81_savonly usbcbu c81_savonly --timeout 6 savonly --sav restore.sav
one c81_stg_ok stg c81_stg_ok stg_ok
one c81_stg_cut stg c81_stg_cut stg_cut
one c81_stg_patch stg c81_stg_patch stg_patch
one c81_strm_pause ./cf_sim --scenario strm_pause --boot "$BOOT" $SIMX
one c81_install ./cf_sim --scenario install --boot "$BOOT" $SIMX
one c81_cut_header ./cf_sim --scenario cut_header --boot "$BOOT" $SIMX
SIMX="--chip 1681 --wp-low"
SC=usb_restore one c81L_restore usbcbu c81L_restore --timeout 6 restore --rom restore.gb --sav restore.sav
SC=usb_savonly one c81L_savonly usbcbu c81L_savonly --timeout 6 savonly --sav restore.sav
SC=usb_backup one c81L_backup usbcbu c81L_backup backup --rom out.gb --sav out.sav
one c81L_stg_ok stg c81L_stg_ok stg_ok
one c81L_stg_patch stg c81L_stg_patch stg_patch
SIMX="--chip 1681 --wp-garble"
one c81G_stg_ok stg c81G_stg_ok stg_ok
SC=usb_restore one c81G_restore usbcbu c81G_restore --timeout 6 restore --rom restore.gb --sav restore.sav
SIMX=; SC=
one fetch ./cf_fetch_test
one ctr ./cf_ctr_test               # AES-256-CTR (cf_ctr_xor) vs OpenSSL + the wire format vector
one install ./cf_install_test       # the poll / ask / write / report path (mock API, download, OSD)          # the Wi-Fi download into the image region (fake gateway + fake NOR)
# probe runs: receiver + cf_sim + comparison
probe() {   # name, cf_sim scenario, receiver args...
    name=$1; sc=$2; shift 2
    d=$T/$name; mkdir -p "$d"
    python3 -I "$HERE/cart_probe_rx.py" --port 0 --port-file "$d/port" --out "$d/out" --exit-on-end "$@" > "$d/rx.log" 2>&1 &
    rx=$!
    for i in 1 2 3 4 5 6 7 8 9 10; do [ -s "$d/port" ] && break; sleep 0.3; done
    ( cd "$d" && "$HERE/cf_sim" --scenario $sc --receiver "$(cat "$d/port")" --boot "$BOOT" ${SIMARGS:-} ) > "$d/sim.log" 2>&1
    simrc=$?
    kill $rx 2>/dev/null; wait $rx 2>/dev/null
    cat "$d/sim.log" >> "$T/$name.log"
    if [ $simrc -ne 0 ]; then echo "cf_sim failed" >> "$T/$name.log"; return 1; fi
    [ "$sc" = probe_prod ] && { grep -q '"ev": "header"' "$d/rx.log" && return 1; return 0; }
    compare "$d" "$name"
}
# the probe over the USB console (cart_probe_usb.py --stdio as the computer), slow ESP32, no Wi-Fi
probe_usb() {
    name=$1; shift
    d=$T/$name; mkdir -p "$d"
    ( cd "$d" && "$HERE/cf_sim" --scenario probe_usb --boot "$BOOT" "$@" \
        --usb-tool "python3 -I $HERE/cart_probe_usb.py --stdio --out $d/out --ram-banks 4 --ram-kb 8 ${TOOLARGS:-} --timeout 100000 2>$d/tool.log" ) > "$d/sim.log" 2>&1
    simrc=$?
    cat "$d/sim.log" >> "$T/$name.log"
    [ $simrc -eq 0 ] || return 1
    compare "$d" "$name"
}
compare() {
    d=$1; name=$2
    python3 -I - "$d" "$name" >> "$T/$name.log" 2>&1 <<'PY'
import json, sys, os, glob
d, name = sys.argv[1], sys.argv[2]
rep = json.load(open(os.path.join(d, "out", "report.json")))
chip = open(os.path.join(d, "orig_chip.bin"), "rb").read()
ram = open(os.path.join(d, "orig_ram.bin"), "rb").read()
rom = open(rep["rom_file"], "rb").read()
sav = open(glob.glob(os.path.join(d, "out", "*.sav"))[0], "rb").read()
ok = True
def chk(c, m):
    global ok
    print("  %s %s" % ("ok  " if c else "FAIL", m)); ok = ok and c
chk(rep["rom_ok"], "receiver: ROM ok (checksums: header %s, global %s)" % (rep["header_checksum"], rep["global_checksum"]))
chk(rom == chip[:len(rom)], "dump (%d bytes) == the cart's flash" % len(rom))
if name == "probe_ram_partial":
    chk(not rep["ram_ok"] and rep["ram_map"] == ".xxxxxxx" * 4, "RAM map %s: floating KiBs flagged, report not ok" % rep["ram_map"])
    chk(not os.path.exists(os.path.join(d, "out", "SIM_TETRIS.sav")), "no .sav claims to be a backup (only .partial.sav)")
    good = all(sav[k * 1024:(k + 1) * 1024] == (ram[k * 1024:(k + 1) * 1024] if k % 8 == 0 else b"\xff" * 1024) for k in range(32))
    chk(good, "partial.sav: the stable KiB of each bank == the cart, the floating ones FF")
    chk(rep["ram_diag"]["reading"].startswith("ADDRESS"), "diag: %s" % rep["ram_diag"]["reading"])
else:
    chk(rep["ok"] and rep["ram_ok"], "receiver report ok, save RAM map %s" % rep.get("ram_map"))
    chk(sav == ram, "save dump == the cart's FRAM (%d bytes)" % len(sav))
if name == "probe_ram_timeout":
    chk(rep["ram_diag"]["reading"].startswith("TIME"), "diag: %s" % rep["ram_diag"]["reading"])
if name == "probe_usb_full":
    t = float(open(os.path.join(d, "sim.log")).read().split("stats: ")[1].split(" s")[0])
    chk(t < 300, "whole 2 MiB chip + 32 KiB save in %.0f s emulated (unit 1 took 27 min on the old path)" % t)
if name == "probe_ext":
    s = rep.get("ext_summary", {})
    chk(124 in s.get("distinct_non_ff", []), "fingerprints find the non-blank bank 124 past the game: %s" % s.get("distinct_non_ff"))
if name in ("probe_flashid", "probe_flashid_diy"):
    ids = rep["flash_id"]
    chk(len(ids) == 4 and all(i["reset_ok"] for i in ids), "4 ID variants, chip back in read mode after each")
    if name == "probe_flashid_diy":
        chk(any(i["match"] and i["match"].startswith("SST39VF1682") for i in ids), "a DIY flash cart (GB writes reach it) answers: %s" % [i["match"] for i in ids])
    else:
        chk(not any(i["match"] for i in ids), "as on unit 1: no variant identifies a chip through GB-timed cycles (the engine does)")
print("PROBE CHECK %s" % ("PASSED" if ok else "FAILED"))
sys.exit(0 if ok else 1)
PY
}
one probe       probe probe probe --ram-banks 4 --ram-kb 8
one probe_ext   probe probe_ext probe --extended --ext-banks 128 --ram-banks 4 --ram-kb 8
one probe_full  probe probe_full probe --rom-banks 128 --ram-banks 4 --ram-kb 8
one probe_prod  probe probe_prod probe_prod --mimic-prod
SIMARGS="--esp-up 12 --wifi-up 20"; one probe_slow probe probe_slow probe --ram-banks 4 --ram-kb 8
SIMARGS=; one probe_usb   probe_usb probe_usb --esp-up 12
one probe_ram_partial probe_usb probe_ram_partial --esp-up 12 --ram-kib 1
one probe_ram_timeout probe_usb probe_ram_timeout --esp-up 12 --ram-timeout-ms 200
TOOLARGS="--rom-banks 128"; one probe_usb_full probe_usb probe_usb_full --esp-up 12; TOOLARGS=
one probe_flashid probe probe_flashid probe_flashid --probe "$HERE/../sm83/build/cartprobe_flashid.bin" --allow-flash-id --ram-banks 4 --ram-kb 8
SIMARGS="--gb-writes-flash"; one probe_flashid_diy probe probe_flashid_diy probe_flashid --probe "$HERE/../sm83/build/cartprobe_flashid.bin" --allow-flash-id --ram-banks 4 --ram-kb 8; SIMARGS=
echo "$pass passed, $fail failed${failed:+ (}${failed# }${failed:+)}; logs in $T"
[ $fail -eq 0 ]
