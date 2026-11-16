#!/usr/bin/env bash
cd "$(dirname "$0")/.."
bash sim_gg/run.sh build > sim_gg/out/build-probes2.txt 2>&1; echo "build rc=$?"
bash sim_gg/run.sh check --warm sim_gg/roms/Sonic_the_Hedgehog.gg 130 2>&1 | grep -a "probe\|panel:\|FAIL"
cp sim_gg/out/Sonic_the_Hedgehog/probes.txt sim_gg/probes-ref-Sonic.txt
