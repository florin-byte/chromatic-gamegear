# SMS_MiSTer (vendored)

Sega Master System / Game Gear / SG-1000 core from the MiSTer project, used here in Game Gear mode by
`../../emu_system_top.v`.

- Source: https://github.com/MiSTer-devel/SMS_MiSTer
- Commit: `8443a0d` (14 Oct 2026), taken through the Game Bub port of the same core
  (github.com/florin-byte/gamebub-rev2, `fpga/vendor/sms_mister`), whose local changes are included.
- Files: the RTL used by `system.vhd` (and `video.vhd` in the simulation), the T80 CPU, JT89, VM2413. The MiSTer
  top level (`SMS.sv`), the MiSTer framework (`sys/`) and the Altera RAM wrappers are not included: this design
  has its own top level and RAM entities (`../gowin_ram.vhd`, `../xilinx_ram.vhd`, `../gg_ram.v`).
- **Licence: the upstream repository has no top-level licence file and states no licence; the files carry their
  own headers where they have one** (T80: BSD-style notice; JT89: GPL-3.0; VM2413: its own notice; 20 of the 54
  files have none). See `NOTICE.md` at the root of this repository.

## Local changes

From the Game Bub port:
- `jt89/jt89.vhd` and `system.vhd`: package `jt89` renamed to `jt89_components`; `jt89.v` / `jt89.vhd`: ports
  `soundL` / `soundR` in lower case.
- `system.vhd`, `T80/*.vhd`: `entity work.<name>` instances (standard VHDL, for GHDL).

For the Chromatic (Gowin GW5A-25), see `fpga/game-gear/README.md`:
- No default values on the input ports of compiled VHDL entities, every input connected explicitly
  (`patch_ssports.py`, `patch_defaults.py`): GowinSynthesis 1.9.12.04 wired such ports to an unrelated signal.
- `system.vhd` / `memory_bus.vhd`: outputs `rom_rd` (a CPU read the ROM answers) and `nvram_rd` (a CPU read the
  cartridge RAM answers), so that only real ROM and cartridge RAM reads reach the RAM cart's cache.
- The System E hardware made optional to fit the device's 56 block RAMs (`slim_gg_core.py`: generics `SYSTEM_E`
  in `system` and `SE_VRAM` in `vdp`; with `SYSTEM_E` = 0 there is no second VDP, no second PSG, no boot ROM and
  16 KiB of VRAM).
- FM sound is off (`fm_ena` = 0; `../opll_stub.vhd` is the stub for the FM chip) and the save-state bus is tied
  off in `../../emu_system_top.v`.

To update: copy the same files from a newer upstream checkout, apply the changes above and record the new commit
here.
