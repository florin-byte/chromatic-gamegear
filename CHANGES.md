# Changes made in this repository

The base is Shiny Relay 1.1.5 (`firmware/shiny-relay-mcu/` started as its `mcu/` directory unchanged;
`fpga/shiny-relay-1.1.5/` is its `fpga/` directory unchanged). Shiny Relay's own changes against ModRetro's
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

## FPGA

| Date | Where | Change |
|---|---|---|
| 2026-11-01 | `fpga/game-boy-patches/0001-rcfg-mreset.patch` | Game Boy design: RCFG bit 4 (MRESET) resets the RAM cart's mapper without turning the RAM cart off |
| 2026-11-01 | `fpga/game-boy-patches/0002-build-route2.patch` | Build: route option 2 |
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
