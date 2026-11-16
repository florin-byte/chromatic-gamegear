# ESP32 firmware

`shiny-relay-mcu/` started as the `mcu/` directory of Shiny Relay 1.1.5 (GPL-3.0, source archive
`shiny-relay-src-1.1.5.tar.gz` from https://install.shinyrelay.com), unchanged, and was brought to Shiny Relay
1.1.12 (`shiny-relay-src-1.1.12.tar.gz`) by a three-way merge; the changes made in this repository are listed in
`../CHANGES.md`. `LICENSES/` and `NOTICE.md` are the ones of those archives (the same in both).

The version built from this tree is `1.1.12-fr4a` (`shiny-relay-mcu/sdkconfig.wifiinst`).

## Tests on a PC (no ESP-IDF needed, gcc and make)

```sh
make -C shiny-relay-mcu/components/sdcard/host test
make -C shiny-relay-mcu/components/osd/system/host games_shots && shiny-relay-mcu/components/osd/system/host/games_shots > /dev/null
cd shiny-relay-mcu/components/nw/cartflash/host && cc -O1 -I.. -o cf_fota_test cf_fota_test.c ../cf_fota.c && ./cf_fota_test
```

- `components/sdcard/host`: the card library over FatFs on a RAM disk (folders, recent games, the per-core views).
- `components/osd/system/host`: the real games page with a simulated card.
- `components/nw/cartflash/host`: the FPGA boot flash layout, the core switch and the update, with a power cut at
  every program step.

## Build

ESP-IDF 5.3. The configuration Shiny Relay 1.1.x ships is `sdkconfig.defaults` + `sdkconfig.usbcart` +
`sdkconfig.wifiinst` (OTA partitions, rollback):

```sh
cd shiny-relay-mcu
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.usbcart;sdkconfig.wifiinst" set-target esp32
idf.py build
```

The generated `sdkconfig` must be removed when `sdkconfig.wifiinst` changes, or the version string is not
regenerated. `build/mcu_fw.bin`, `build/ota_data_initial.bin` and `build/partition_table/partition-table.bin` are
the three files the installer needs; it writes the app at 0x10000 and otadata at 0x200000 and leaves the boot
loader and the partition table alone.

## Console commands added here

`sd mv FROM TO`, `cart fw slots`, `cart fw switch gb|gg`; `sd list` shows `G` for a Game Gear game and `sd st`
shows the visible count next to the total.
