# Changes made in this repository

The base is Shiny Relay: `firmware/shiny-relay-mcu/` started as the `mcu/` directory of 1.1.5 and was brought to
1.1.12 on 2026-11-16 by a three-way merge; `fpga/shiny-relay-1.1.5/` is the `fpga/` directory of 1.1.5, unchanged. Shiny Relay's own changes against ModRetro's
release are listed in `firmware/shiny-relay-mcu/CHANGELOG.md` and in the notices it ships. Everything below was
changed here, in 2026.

## ESP32 firmware (`firmware/shiny-relay-mcu/`)

| Date | Version | Change |
|---|---|---|
| 2026-10-31 | 1.1.5-fr1 | GAMES library over the whole screen: folders as on the card, A-Z, played last; a GAMES tab; SELECT opens the library; card index v4 (long names and paths, folder files); text scrolling speed fixed |
| 2026-10-31 | 1.1.5-fr2 | The CONTROLS picture; the list no longer flickers |
| 2026-11-01 | 1.1.5-fr3 | Changing the card game without a black screen |
| 2026-11-03 | 1.1.5-fr3h | "Stop playing" without a black screen (an empty cartridge is loaded into the RAM cart); its result message opens by itself |
| 2026-11-03 | 1.1.5-fr3i | GAMES opens on the game that is playing |
| 2026-11-16 | 1.1.5-fr3j | Console command `sd mv FROM TO` (a file or folder moved on the card, the target's folders made) |
| 2026-11-16 | 1.1.5-fr3k..fr3m | Two FPGA designs in the boot flash (slots GB 0x330000-0x3FFFFF and GG 0x260000-0x32FFFF, fixed end of the game area); an FPGA update writes only the slot of the design it replaces; `cf_fota_switch` and the console commands `cart fw slots` / `cart fw switch gb\|gg`; SYSTEM > CORE; the switch question when a game needs the other core; the card index marks Game Gear games (index v5) and the lists show only the running core's games |
| 2026-11-16 | 1.1.5-fr3n | Two-level jump headers: the fixed header points to a selector sector (0x25E000 / 0x25F000) whose headers choose the design, so a switch no longer spends a fixed header place (about 960 switches instead of 31); SWITCHES LEFT on the CORE page; the game area ends at 0x25E000 |
| 2026-11-16 | 1.1.5-fr3o | The games pages wait for the booted core after power-on (`READING THE CORE`) instead of listing both cores' games; the first header read is retried quickly |
| 2026-11-16 | 1.1.12-fr4 | Shiny Relay 1.1.6 - 1.1.12 merged in (their changes: the real-time clock and SYSTEM > CLOCK, safer saves for games played from memory, SAVE TO MY GAMES, SYSTEM > CUSTOM, COLOR TEMP in Game Boy mode, see `firmware/shiny-relay-mcu/CHANGELOG.md`); everything above kept. Of 63 files they changed, 3 needed a decision by hand (`games.c`, `games_shots.c`, the version). The library still asks for one read of the card when it opens |
| 2026-11-16 | 1.1.12-fr4a | The COLOR TEMP page of Game Boy mode (new in 1.1.12) has the GAMES icon in its tab strip: its picture is made from this repository's palette picture |

## FPGA

| Date | Where | Change |
|---|---|---|
| 2026-11-01 | `fpga/game-boy-patches/0001-rcfg-mreset.patch` | Game Boy design: RCFG bit 4 (MRESET) resets the RAM cart's mapper without turning the RAM cart off |
| 2026-11-01 | `fpga/game-boy-patches/0002-build-route2.patch` | Build: route option 2 |
| 2026-11-16 | `fpga/game-boy-patches/0000-shiny-relay-18.33.patch` | Not a change of mine: what Shiny Relay 1.1.12 changed in the FPGA project (18.33: the colour temperature also warms Game Boy games). Applied first; the Game Boy design of release v0.2.0 is the base + 0000 + 0001 + 0002 |
| 2026-11-14..15 | `fpga/game-gear/` | The Game Gear design: the Game Boy core replaced by the MiSTer SMS core in Game Gear mode (`overlay/`: 69 files new or changed against the base): ROM loader over the RAM cart's cache, clock enables for the panel's frame rate, video adapter, the workaround for a Gowin synthesis defect with VHDL default port values (see `fpga/game-gear/README.md`) |
| 2026-11-16 | `fpga/game-gear/overlay/.../ramcart.v` (build gg15) | Every cache fill is read until two passes agree: a PSRAM read burst now and then arrives one word late (black screen at 25-40 % of the starts) |
| 2026-11-16 | `fpga/game-gear/overlay/` (build gg16) | The core's cartridge RAM (32 KiB) is the RAM cart's save area in PSRAM, so the firmware keeps Game Gear saves on the card |

## Tools (`tools/`), all new

| Date | File | What |
|---|---|---|
| 2026-11-01 | `log_serial.py` | The ESP32 console logged with DTR/RTS low |
| 2026-11-14 | `gg2gb.py` | A Game Gear ROM wrapped in a Game Boy container (MBC5 + RAM + battery header, a marker the FPGA checks) |
| 2026-11-16 | `chromatic_flash_image.py` | The 4 MiB boot flash image with both designs composed and described |
| 2026-11-16 | `installer/` | `chromatic_install.py`: check, backup, install, restore, addgame |
