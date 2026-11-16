# Chromatic two-core installer

Requires Python 3.9+, pyserial, esptool 4.x, openFPGALoader with GWU2X support,
and a Chromatic already running Shiny Relay 1.1.5 (or this project's fr firmware).
Use a USB data connection and the unit's Gowin cable. Nothing is downloaded.

Provide `--files DIR` containing `mcu_fw.bin`, `ota_data_initial.bin`,
`partition-table.bin`, `fpga-gb.bin`, `fpga-gg.bin`, and `SHA256SUMS`.
Each manifest line is a SHA256 hex digest, two spaces (or space + `*`), and a
file basename. Binaries are distributed separately, not stored in this repository.

Run from the repository root:

```sh
python3 tools/installer/chromatic_install.py check
python3 tools/installer/chromatic_install.py backup --dir backups
python3 tools/installer/chromatic_install.py install --backup backups/TIMESTAMP --files release --live gb
python3 tools/installer/chromatic_install.py restore --backup backups/TIMESTAMP
python3 tools/installer/chromatic_install.py addgame YOUR_GAME.gg --folder "Favorites" --title "GAME TITLE" --name "Game.gb"
```

`--port PORT` overrides automatic selection (exactly one USB 374e:0101 device).
`--openfpgaloader PATH` overrides PATH and the two standard macOS search locations.
Connection options and `--files` work before or after the subcommand.
Writing commands display their destinations and require the exact word `WRITE`;
`--yes` bypasses this prompt. Backup only reads the unit and creates local files.

Backup creates a timestamped directory with `esp32.bin`, `fpga.bin`, and
`SHA256SUMS`, published only after two identical 4 MiB FPGA reads. Keep it safe:
it may contain Wi-Fi credentials. Install requires both files and valid hashes,
uses that backup's recovery design, resets the jump header places, and selects
GB by default (`--live gg` selects GG). It checks the backup and current partition
tables before writing only the ESP32 app and otadata, then writes and reads back
the whole FPGA image. Restore checks the current table against the backup and
writes only the backup's app/otadata plus the whole FPGA dump. Bootloader and
other ESP32 regions are preserved.

**After any Gowin cable operation, switch the console off and on.** Flash access
clears the running FPGA configuration: the screen goes dark and serial USB goes
away. Every command performs serial work first and cable work last. Cable
detection is therefore performed after ESP32 work in install/restore; a cable
failure there can leave the ESP32 updated with the old FPGA image.

Addgame wraps a user-provided game using `gg2gb.wrap`, reads the inactive staging
address from the boot message, uploads in chunks of at most 1 MiB, imports and
moves the game, then compares the card's CRC32. The destination folder must
already exist; this tool does not delete an existing destination file. A failed
move can leave a temporary `GGxxxxxxxx.gb` file in `/GAMES`. A successful import
may still require the firmware's normal card refresh to appear in its menu.

On failure the command exits nonzero and stops immediately. Do not assume a
partly completed install was rolled back. Keep the backup, power-cycle after
Gowin access, fix the reported issue, then restore or rerun install using the
same verified backup. An incomplete backup has no valid manifest and cannot
be installed/restored. There is no hardware access in the unit tests:

```sh
python3 -m unittest discover -s tools/installer -p "test_*.py" -v
```
