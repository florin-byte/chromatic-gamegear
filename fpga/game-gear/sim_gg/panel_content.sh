#!/usr/bin/env bash
# Panel checks with picture content, both gclk phases: panel_content.sh <rom> <frames> [start frame]
cd "$(dirname "$0")/.."
for ph in "" "--gphase1"; do
    bash sim_gg/run.sh check $ph "$@" 2>&1 | tail -2
    echo "phase '$ph' rc=${PIPESTATUS[0]}"
done
