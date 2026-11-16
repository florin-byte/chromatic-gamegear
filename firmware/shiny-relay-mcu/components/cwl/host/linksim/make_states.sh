#!/bin/zsh
# Battle-start save states for linksim (ModRetro Tetris Battle, Tetris DX VS), made with
# linksim itself over an ideal 2 ms link. Kept OUTSIDE the repo (ROM-derived):
#   make_states.sh OUTDIR        then  LINKSIM_STATES=OUTDIR python3 -I run_matrix.py ...
set -e
O=${1:?outdir}; mkdir -p $O; cd ${0:A:h}
T=~/ChromaticCartBackups/u1_tetris_20261010_205736/MR_TETRIS.gbc
TD="${LINKSIM_ROMS:?set LINKSIM_ROMS to your ROM folder}/Tetris DX (World).gbc"
# ModRetro Tetris: title -> MULTIPLAYER -> GUEST -> A hosts, B joins -> B READY, A START
./linksim $T - $T - --frames 640 --keys-a 500:down,540:a --keys-b 505:down,545:a --save-a $O/mrt_sel_a --save-b $O/mrt_sel_b >/dev/null 2>&1
./linksim $T $O/mrt_sel_a $T $O/mrt_sel_b --frames 300 --keys-a 20:down,40:down,60:down,90:a --keys-b 20:down,40:down,60:down,90:a --save-a $O/mrt_mp_a --save-b $O/mrt_mp_b >/dev/null 2>&1
./linksim $T $O/mrt_mp_a $T $O/mrt_mp_b --frames 300 --keys-a 30:a --keys-b 40:right,70:a --save-a $O/mrt_lobby_a --save-b $O/mrt_lobby_b >/dev/null 2>&1
./linksim $T $O/mrt_lobby_a $T $O/mrt_lobby_b --frames 280 --keys-b 30:a --keys-a 90:down,120:down,150:down,180:right,210:right,240:a --save-a $O/mrt_bat_a --save-b $O/mrt_bat_b >/dev/null 2>&1
# Tetris DX: from chromatic_wifi/states/tdx_d2 (2PLAYER, name A) on both, into a VS game
S0=${LINKSIM_STATES0:?set LINKSIM_STATES0 to the save-state folder}
./linksim "$TD" $S0/tdx_d2 "$TD" $S0/tdx_d2 --frames 900 --esp a --keys-a 30:a,200:a,300:a,400:a,600:start --keys-b 40:a,210:a,310:a,410:a,610:start --save-a $O/tdx_vs_a --save-b $O/tdx_vs_b >/dev/null 2>&1
ls $O
