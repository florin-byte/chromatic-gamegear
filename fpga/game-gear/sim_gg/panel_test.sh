#!/usr/bin/env bash
# Panel checks: builds the new adapter and the gg2 adapter, runs the idle (no game) test on both gclk phases.
cd "$(dirname "$0")/.."
bash sim_gg/run.sh build > sim_gg/out/build-panel.txt 2>&1; echo "build new rc=$?"
bash sim_gg/run.sh build-gg2 > sim_gg/out/build-panel-gg2.txt 2>&1; echo "build gg2 rc=$?"
grep -n "%Error" -A3 sim_gg/out/build-panel.txt sim_gg/out/build-panel-gg2.txt | head -30
for bin in build build-gg2; do
    for ph in "" "--gphase1"; do
        SIM_BIN=sim_gg/out/$bin/Vtb_gg SIM_OPTS="$ph" bash sim_gg/run.sh noload 2>&1 | tail -2
        echo "$bin $ph rc=$?"
    done
done
