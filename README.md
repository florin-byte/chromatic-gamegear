# Chromatic dual core: Game Boy + Game Gear

A modified firmware and FPGA setup for the ModRetro Chromatic that holds **two FPGA designs** in the console's
boot flash — the Game Boy / Game Boy Color design and a Sega Game Gear design — and lets you choose which one
starts, from the console's own menu.

This is a derivative of **Shiny Relay 1.1.5** (by Shiny Gen, https://shinyrelay.com), which is itself a modified
version of ModRetro's open-source Chromatic FPGA design and ESP32 firmware. It is not made by, affiliated with or
endorsed by ModRetro, Shiny Gen, Sega or Nintendo. See `NOTICE.md` for where every part comes from and its
licence, and `CHANGES.md` for what was changed here.

**No games are included.** You need your own ROM files.

## What it does

- **Two cores, one console.** SYSTEM > CORE shows the core that runs and switches to the other one. A switch takes
  effect when the console is switched off and on (nothing can restart the FPGA from the inside). The page shows
  how many switches are left; a fresh install has about 960.
- **Game Gear games from the card.** A Game Gear ROM is wrapped by `tools/gg2gb.py` so the firmware loads it like a
  card game. Cartridge RAM (32 KiB) is kept on the card in `/SAVES`, as for Game Boy games.
- **Only the running core's games are listed.** If a game for the other core is started anyway, the menu offers
  the switch.
- **An installer** (`tools/installer`) that backs the console up, writes both designs with fresh jump headers and
  the firmware, restores a backup, and puts a Game Gear game on the card over USB.

## What you need

- A Chromatic that already runs Shiny Relay 1.1.5 (its boot flash layout with the recovery design is required;
  the installer refuses anything else).
- The console's Gowin USB cable interface (GWU2X) reachable from the computer, `openFPGALoader` with GWU2X
  support, Python 3.9+, `esptool` 4.x and `pyserial`.
- The binaries (`mcu_fw.bin`, `ota_data_initial.bin`, `partition-table.bin`, `fpga-gb.bin`, `fpga-gg.bin`,
  `SHA256SUMS`), built from this tree or taken from a release of this repository.

Read `tools/installer/README.md` before the first run. In short:

```sh
python3 tools/installer/chromatic_install.py check
python3 tools/installer/chromatic_install.py backup --dir backups
python3 tools/installer/chromatic_install.py install --backup backups/TIMESTAMP --files release --live gb
python3 tools/installer/chromatic_install.py addgame YOUR_GAME.gg --folder "Favorites"
```

Keep the backup: it is the way back (`restore`) and it may contain your Wi-Fi password, so do not share it.
After any step that uses the Gowin cable the screen is dark until the console is switched off and on.

## Limits and known issues

- Tested on one console, from macOS only. Windows and Linux steps are not verified; on Windows the Gowin cable
  usually needs an extra USB driver.
- Based on Shiny Relay 1.1.5. Shiny Relay has moved on (1.1.12 at the time of writing: real-time game clocks, warm
  whites for Game Boy games, save upload, a custom update channel); none of that is merged here.
- An FPGA update from the console's menu still describes a single design and replaces the design of the core that
  runs.
- The cartridge in the slot is a Game Boy thing: while the Game Gear core runs it is not offered.
- Game Gear carts with a 93C46 EEPROM have no save memory. Not every Game Gear game was tried (22 were; all run).
- No Gear-to-Gear link.
- After power-on the menu shows `READING THE CORE` for a few seconds before the games appear.
- One write through the Gowin cable out of about fifteen failed with a timeout and worked when repeated; the
  installer stops in that case and is simply run again.

## Layout of this repository

| Path | What |
|---|---|
| `firmware/shiny-relay-mcu/` | the ESP32 firmware: Shiny Relay 1.1.5's `mcu/` with the changes of `CHANGES.md` (version 1.1.5-fr3o) |
| `fpga/shiny-relay-1.1.5/` | Shiny Relay 1.1.5's FPGA project, unchanged: the base both designs are built from |
| `fpga/game-boy-patches/` | two patches on that base for the Game Boy design |
| `fpga/game-gear/` | the Game Gear design: the files that are new or changed against the base (`overlay/`), its simulation, notes |
| `tools/` | `gg2gb.py`, `chromatic_flash_image.py`, the installer, console helpers |

Building the FPGA designs needs the Gowin tools (the shipping USB device uses Gowin's encrypted IP, as in
ModRetro's release); building the firmware needs ESP-IDF 5.3. See `firmware/README.md` and
`fpga/game-gear/README.md`.

## Licence

GPL-3.0 for this repository as a whole (`LICENSE`), with the third-party parts under their own licences as listed
in `NOTICE.md`. There is NO WARRANTY. Flashing a console can leave it unable to start until it is written again
through the cable; make the backup first.
