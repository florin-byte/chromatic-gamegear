# Game Gear core on the Chromatic FPGA (CG-101 … CG-115, CG-106)

The design is the stock Shiny Relay 1.1.5 FPGA project (`fpga/shiny-relay-1.1.5/esp32t` in this repository) with the
Game Boy core replaced by the MiSTer SMS core in Game Gear mode. This directory holds everything that differs
from the stock tree.

- `overlay/esp32t/` — the 69 files that are new or changed against the stock project (state of build gg16).
  Copy them over a copy of the stock `esp32t` directory, then build with
  the Gowin tools (`gw_sh build.tcl`, place option 2 closed timing here).
- `sim_gg/` — Verilator simulation with the real panel modules (run in WSL; `regen_gen.sh` in Git Bash, it
  converts the VHDL with the Windows GHDL). The game ROMs are not kept here.
- `check_netlist.py` — **run it on `impl/gwsynthesis/evt1_x2.vg` after every build** (exit code 0 = clean).
- `patch_ssports.py a`, `patch_defaults.py` — how the VHDL of the overlay was derived from the vendor core:
  no default values on VHDL input ports, every input connected explicitly.
- `slim_gg_core.py`, `gowin_ram.vhd`, `patch_idle.py`, `patch_panel_sim*.py` — earlier one-off edits, already
  contained in the overlay. `patch_probes.py`, `patch_trace.py`, `rtl/gg_probe.v`, `rtl/gg_trace.v` and the
  decoders in `tools/` belong to the diagnostic builds gg4diag / gg5diag and are not part of gg8.

## The Gowin defect (CG-113, CG-114)

GowinSynthesis 1.9.12.04, project in VHDL-2008 mode: an input port of a VHDL entity that has a default value
(`: in std_logic := '0'`) was not connected to its actual, nor to its default, but to `secondCounter[16]` /
`secondCounter[15]`, a free-running counter of `top.v` — both for ports the instantiation connects and for ports
it leaves out. No message in the synthesis log. The simulation (GHDL + Verilator) cannot see it.
Effects seen on the unit: the Z80 held in the save-state restore branch (white screen), the video counters
restored periodically (menu shifted with wrap-around), the video RAM written half of the time (corrupted tiles).
Rule for VHDL in this project: no default values on input ports of compiled entities; connect every input.
The stock design has no such ports in its VHDL files.

## PSRAM reads one word late (CG-117, build gg15)

Symptom on the unit: a game now and then started with a black screen (25-40 % of the starts, any game): the
loader found no `GGCHROM1` marker and left the core idle. Measured: the first cache fill after the release (the
header line 0x100140) sometimes arrives from the PSRAM with the right data one position late (position k holds
word k-1), 16 valid words. The stock firmware knows the same thing for its window reads (`cf_rc_dev.c`,
`psram_read_any`: "with the core held ... never with the core running"). In this design the video side stands
still while the loader works, so the PSRAM bus is idle around that first read. The mechanism inside
`PSRAMController.vhd` is not proven.

Cure (`rtl/EMU/ramcart.v`): every cache fill reads its line again until two passes in a row agree (16 words each
and the same order-sensitive 32-bit sum); only then the tag is written. After 10 passes without agreement the
line is taken as it is and counted. On the unit (54 starts by command, then from the menu): no bad start; the
late read appeared in 19 of the 54 starts, always on the header line only, and was repeated; no repeat while a
game ran (a few minutes of one game).

Simulation: `sim_gg/run.sh check --shift=N[,P] rom frames` makes the read bursts with index % N == P come one
word late. With the old `ramcart.v`, `--shift=7,0` gives the black screen; with this one the frames are
identical to a run without the fault.

The diagnostics of gg12diag … gg15 are still in the design: `rc dbg` prints `gg_dbg` (pages in
`emu_system_top.v`; page 6 = {giveup, redo_load, redo_run, reloads}, counters described at `x_diag` in
`ramcart.v`).

## Cartridge RAM and saves (CG-107, build gg16)

The core's cartridge RAM (32 KiB, `nvram_*`) is no longer a block RAM: it is the RAM cart's save area in PSRAM
(`RAM_BASE`), reached through the same cache port as the ROM (`ramcart.v`: `lin_ram`; `emu_system_top.v` muxes the
port by `nvram_rd`, a new output of `system.vhd` / `memory_bus.vhd`). `gg2gb.py` writes a header that says MBC5 +
RAM + BATTERY with 32 KiB, so the unmodified firmware loads `/SAVES/<game>.sav` into that area and copies it back
after writes. A save RAM write no longer waits for the PSRAM word (the cached line holds the byte; requests stay in
order). Shining Force 2 needs the 32 KiB (it stayed black with the 8 KiB block RAM). Not supported: the 93C46
EEPROM carts (the core's EEPROM model used the same RAM).
