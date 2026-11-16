#!/usr/bin/env bash
# Runs acceptance checks: accept.sh short | long
cd "$(dirname "$0")/.."
run() {
    local name="$1"; shift
    timeout 3000 bash sim_gg/run.sh "$@" > "sim_gg/out/run-$name.txt" 2>&1
    echo "$name rc=$?"
    tail -4 "sim_gg/out/run-$name.txt"
    cat "sim_gg/out/$name/summary.txt" 2>/dev/null
}
case "${1:-short}" in
short)
    run test_red_screen check sim_gg/roms/test_red_screen.gg 30
    run test_palette check sim_gg/roms/test_palette.gg 30
    run nomagic nomagic
    run noload noload
    ;;
long)
    run Columns check --content sim_gg/roms/Columns.gg 600 400
    run Sonic_the_Hedgehog check --content sim_gg/roms/Sonic_the_Hedgehog.gg 900 600
    ;;
esac
