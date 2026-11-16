# Notices: where each part comes from

This repository is a modified version of **Shiny Relay 1.1.12**, which is a modified version of ModRetro's
open-source Chromatic FPGA design and ESP32 firmware. It is not made by, affiliated with or endorsed by ModRetro,
Shiny Gen, Sega or Nintendo. The changes made here are listed, with dates, in `CHANGES.md`.

## The chain

| Layer | Who | Where | Licence |
|---|---|---|---|
| Chromatic FPGA design | ModRetro | github.com/ModRetro/oss-chromatic-console-fpga | GPL-3.0 |
| Chromatic ESP32 firmware | ModRetro | github.com/ModRetro/oss-chromatic-console-mcu | GPL-3.0 |
| Shiny Relay 1.1.5 and 1.1.12 (changes to both) | Shiny Gen | source archives from https://install.shinyrelay.com (`shiny-relay-src-1.1.5.tar.gz`, `shiny-relay-src-1.1.12.tar.gz`) | GPL-3.0 |
| This repository (changes to both, the Game Gear design, tools) | the authors of this repository | here | GPL-3.0 |

`firmware/NOTICE.md` and `firmware/LICENSES/` are Shiny Relay's own notices and licence texts (the same in 1.1.5
and 1.1.12), kept as they came; they list what is inside the two programs (the MiSTer Game Boy core, T80, the SameBoy boot ROM, ESP-IDF,
FreeRTOS, lwIP, Mbed TLS, LVGL and the others) and apply here unchanged. `fpga/shiny-relay-1.1.5/` is Shiny
Relay 1.1.5's `fpga/` directory, unchanged, including the Gowin IP that ModRetro ships encrypted; what 1.1.12
changed in it is `fpga/game-boy-patches/0000-shiny-relay-18.33.patch`.

## Added here

### The Game Gear core: SMS_MiSTer

- What: the Sega Master System / Game Gear core of the MiSTer project, used in Game Gear mode; the files are in
  `fpga/game-gear/overlay/esp32t/src/rtl/EMU/GG/sms_mister/` (see the `README.md` there for the exact commit and
  the local changes).
- Source: https://github.com/MiSTer-devel/SMS_MiSTer, commit `8443a0d`, taken through the Game Bub port of the
  same core (github.com/florin-byte/gamebub-rev2, `fpga/vendor/sms_mister`).
- Licence: **the upstream repository has no top-level licence file and states no licence.** The files carry their
  own headers where they have one: the T80 CPU (Daniel Wallner, MikeJ, Sorgelig) under its BSD-style notice
  (`firmware/LICENSES/T80-BSD-3-Clause.txt` reproduces it), the JT89 sound chip (Jose Tejada) under GPL-3.0, the
  VM2413 files under their own notice; 20 of the 54 files used carry no licence header. They are included here
  with attribution, as the MiSTer project's cores are commonly shared, and will be removed or relicensed on the
  request of their authors.

### Everything else added here

The Game Gear glue (`gg_rom_loader.v`, `gg_ce_gen.v`, `gg_video_out.v`, `gg_ram.v`, the changes to `ramcart.v`,
`emu_system_top.v`, `top.v`), the firmware changes and the tools in `tools/` are original work for this
repository, under GPL-3.0.

`tools/gg2gb.py` writes a Game Boy cartridge header in front of a Game Gear ROM so that the firmware loads it; it
contains no Nintendo or Sega code or data (no logo, no boot ROM).
