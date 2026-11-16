#!/usr/bin/env bash
# Reference probe values: Columns and Sonic, started as on the console (idle first), normal and with SELECT held.
cd "$(dirname "$0")/.."
bash sim_gg/run.sh build > sim_gg/out/build-probes.txt 2>&1; echo "build rc=$?"
grep -n "%Error\|error:" -A3 sim_gg/out/build-probes.txt | head -20
for rom in Columns Sonic_the_Hedgehog; do
  for mode in "" "--alt"; do
    echo "== $rom $mode"
    bash sim_gg/run.sh check --warm $mode sim_gg/roms/$rom.gg 110 2>&1 | grep -a "probe\|panel:\|FAIL"
  done
done
