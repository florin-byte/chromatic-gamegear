## v18.31 (Shiny Relay 1.1.0, lane fpgacomp1)

The FPGA image of Shiny Relay 1.1.0 in the boot flash layout "rf" (headers, a recovery design, one full design, the
game area). Built compressed (bit_compress), place 2 route 2 (build r1831a: 0 setup / 0 hold violations; logic 93%,
CLS 97%, BSRAM 55 of 56; 768,397 bytes). The recovery design (rec2.gprj, top_rec2.v) is built beside it (r1831rec:
0 / 0, 370,327 bytes). Like every Shiny Relay image it is a custom build (the version's debug bit is set).

### The boot flash layout rf (mcu cartflash/cf_fota.h; measured on both units)
- 0x000-0x7FF: 32 MSPI jump headers (the chip's sync scan reaches 0x7C0, not 0x800). 0x7C0: the recovery header ->
  0x000800. 0x780 down: full design headers, the lowest complete one is live; an update invalidates it (its sync
  programmed to 00 00: the chip skips it) before the full design is touched and writes the next one after.
- 0x000800-0x05BFFF: the recovery design. 0x05C000 up to the full design: the game area (3,047,424 bytes with this
  image). The full design at the top of the flash, its size rounded up to 4 KiB.
- With no full header live the recovery design boots: the ESP32 finishes an FPGA update from its staged copy by itself.

### Changed
- Compressed bitstream (17% smaller than 18.30's).
- cart_xs: the write guards are parameters (FLASH_W_BASE 0x05C000, FLASH_H_TOP 0x7C0, HDRERASE_OK 0): the recovery
  design and its header can be changed only over USB (JTAG); an erase is checked by the start of the sector / block it
  erases; FHDRERASE (sector 0) is refused. The guards are worked out as each op is queued (the same tests made at
  dispatch refused good ops now and then on unit 1 with no timing violation reported).
- cart_xs op 22 FREAD: boot flash bytes (up to 1 KiB) into a read window (one BSRAM), read over QSPI like the RAM cart's
  window, with their crc32 in the status. The QSPI read window is FREAD's or the RAM cart's, whichever op ran last.
- Timing: the cartridge read contract's multicycle now covers CART_WR / CART_RD / CART_DATA_DIR and the RAM cart's
  wr_q too (evt1_x2.sdc); their ends sit in a box beside the data capture flops (evt1_x2.cst).

### The recovery design (src/rec/top_rec2.v)
The ESP32 UART link (system_monitor_rec: version 18.31 with bit 13 set = recovery), the QSPI ring + window, the boot
flash ops (cart_xs with the same guards), a blue LED blinking once a second (four times while a flash op runs). No USB
(a unit stuck in it is rescued over the GWU2X JTAG), no screen, no PSRAM, no codec / charger I2C, no Game Boy core;
the cartridge slot stays off. (A bigger recovery with USB serial + UVC + the OSD screen exists, src/rec/top_rec.v,
544 KB: it does not fit the 3,040,000-byte game area.)

## v18.30 (Shiny Relay 1.1.0)

The FPGA image of Shiny Relay 1.1.0. Built with place option 2, route option 2 (build t1830k: 0 setup /
0 hold violations; logic 93%, CLS 97-98%, BSRAM 54 of 56). Like every Shiny Relay image it is a custom
build (the version's debug bit is set).

### Added
- The RAM cart (rtl/EMU/ramcart.v): a game in PSRAM (ROM at 0x100000 up to 4 MiB, save RAM at 0x040000
  up to 128 KiB) plays as the cartridge, MBC1 / MBC2 / MBC3 (+ clock) / MBC5, through a 4 KiB cache; the
  slot is powered off and isolated meanwhile (cartridge_interface disabled).
- The FPGA's own boot flash from the ESP32 (cart_xs ops 16-21): JEDEC id, 64 KiB erase / program at
  0x100000 and up, crc32, the jump header area (0x000-0x3FF, FF bytes left alone), its erase. With
  them the firmware updates this image over Wi-Fi (mcu cartflash/cf_fota.h: up to 16 jump headers,
  image slots A 0x100000 / B 0x200000; the USB installer lays the area down).
- QSPI reads from a PSRAM window (the RAM cart's checks), ops 32-34 (RCFG, RREAD, RRTC).

### Known: the USB serial bridge depends on placement
The Gowin USB 2.0 soft PHY (encrypted; its 960 MHz sampling clock is not timed) loses single bytes in the
ESP32 -> USB direction on some placements of the same RTL (t1830h, t1830j: 0 of 8 reads of 256 KiB exact
at 115200), while others are exact (t1830f / g / k / i_p2r2). Every image is gated on the units before it
ships (fpgaota1/bridge_gate.sh / bridge_soak.sh: 256 KiB read back 32 times at 921600 and 8 at 2000000 on
both units, each exact; 18.28 the same). t1830k passed (32 of 32 on both units).

## v18.28 (Shiny Relay 1.0.3)

The FPGA image of Shiny Relay 1.0.3: stock 18.21's network window (Wi-Fi link) plus the cart bus
engine (18.25), the express cart stream (18.26) and the USB bridge change below. Like every
Shiny Relay image it is a custom build: the version's debug bit is set (shown as a custom build;
the installer requires it).

### Added
- The express cart stream: the FPGA writes the cartridge flash from a 4 KiB ring the ESP32 fills
  over the QSPI bus (firmware 1.0.3). Ops and posts on UART address 0x12, status on system monitor
  channel 12 (address 0x0C).
- The cart bus engine (link cmd 9 ENGINE_ALLOW) and the boot image select for cart jobs (link
  cmd 10 BOOT_SELECT), as in 18.25: the stock CGB boot ROM on every normal boot.
- The USB serial bridge takes 1500000 and 2000000 from the host's line coding.

### Changed
- Opening the USB serial port no longer resets the ESP32: EN is held low only for RTS on with DTR
  off (the classic auto-reset circuit), not for DTR + RTS both on, which is how a browser opens a
  port. esptool's reset sequence is unchanged.

## v18.27 (cartspeed1, test; never released)

### Added
- The USB serial bridge follows esptool's CHANGE_BAUDRATE on its ESP32 side: after a CHANGE_BAUDRATE command (SLIP, op 0x0F, 8 bytes of data) and the ESP32's successful response, the bridge UART to the ESP32 runs at the new rate (115200, 460800, 921600, 1500000 or 2000000) while the host's port stays open. A browser (Web Serial) can then copy at 921600 or more without reopening the port, which on the Chromatic resets the ESP32. Any DTR/RTS change, RTS on, a USB reset or a CHANGE_BAUDRATE to 115200 returns to the host's line coding.
- The bridge UART takes 1500000 and 2000000 from the host's line coding too.

### Changed
- Reverted the cart ring's read-port fairness (it did not close timing and the firmware no longer sends long FF runs through the ring).

## v18.26 (cartspeed1, test; never released)

### Added
- The express cart stream: the FPGA writes the cartridge flash from a QSPI-fed ring (firmware 1.0.4-cs1 and later).

## v18.14

### Fixed

- Restore low-battery LED and icon behavior and align power-save thresholds across hardware versions.
- Correct the white charging indicator near full charge.

## v18.13

### Fixed
- Fixed a source of corrupted frames in OBS on macOS.

## v18.12

### Fixed
- Disable the charger watchdog so configured charging settings are not periodically reset.
- Correct temperature-based charge-current selection on hardware with a temperature sensor.
- Synchronize and latch hardware-revision detection before selecting the charging policy.
- Handle I2C NACKs and transaction timeouts, and retry failed charger writes.

## v18.11

### Added
- USB video streaming at 320x288 with 2x integer scaling, now the default. Native 160x144 streaming remains selectable in the capture application.

### Changed
- Reworked USB video capture, color conversion, and packetization to support both resolutions and recover from interrupted transfers.
- Identify USB game audio as a line input instead of a microphone.
- Reduced FPGA resource usage in the audio filters, USB serial FIFOs, and UART baud-rate calculation.
- Removed unused legacy modules, obsolete signal connections, and a stale clock constraint; cleaned up associated synthesis warnings and build references.

### Fixed
- Added hardware-revision-aware cartridge power and level-shifter sequencing to prevent leakage-driven cartridge signals while the updated hardware is switched off, including with USB connected.
- Hold the emulator in reset until cartridge startup sequencing completes.
- Corrected audio filter behavior when building with the updated Gowin tools.
- Improved menu-button synchronization and replaced a divided audio-system clock with a clock enable.

## v18.10

### Changed 
 - logic to support modified power switch network
 - modified USB charge functionality including temperature controlled charge current
 - add second version detect pin
 - add display id pin 
 - eliminate LCD ghosting
 - partial ESD mitigation
 - support for alternative PSRAM chip

## v18.9

### Changed 
 - added link-cable support for revised PCB
 - used `VERSION_DET` pin state for PCB-aware battery voltage thresholds
 - added bit to message payload sent to firmware for PCB-aware battery voltage estimate 
 - removed HDMI signals 
 - Additional `CART_RST` functionality.

### Fixed 
 - existing 3-wire interface for ST7785 LCD controller 

## v18.8

### Changed
- Enable audio until ESP32 can configure the FPGA

### Fixed
- IO0/En control sometimes fails to boot the ESP32
- The FPGA sometimes resets during play

## v18.7

### Fixed
- Custom sprite palettes distinguish betwen obj0 and obj1

## v18.6

### Added
- Support for streaming audio over USB.
- Support for custom palettes to override existing palette after boot.

### Changed
- Inverted audio polarity

## Fixed
- Reworked the design of frame blending

## v18.5

### Added
- Support for streaming on macOS and Discord.

### Changed
- The default palettes changed for `A + B + Right` and `A + B + Left`. All credit to **rayjt9**.
- Tuned settings for the Chromatic Power Core.

### Fixed
- LED indicator light now correct for AA, USB, and Chromatic Power Core use cases.
- Fixed an issue where the clock would skip in titles with an RTC (e.g. Pokemon Crystal).
- Fixed an issue where save data could be lost in some titles (e.g. Pokemon Crystal).
- Streaming no longer requires flipping the video in OBS.
- Miscellaneous compatibility fixes.

## v18.4

### Added
- Option to ignore diagonal inputs from the D-Pad
- Option to use the background palette 0 color (prevents screen transition flash)
- Option to set low battery icon display behavior

### Changed
- Tuned battery thresholds for 1.2V NiMH AA's

### Fixed
- Reduce backlight flashing when power source is nearly depleted
- Support for Kirby Tilt-n-Tumble
- Improved Chromatic firmware version detection
- Support streaming to Linux platforms

## v18.0

### Changed
- Improved button debouncing.

### Fixed
- Fully mute game audio when speaker wheel is turned to minimum.
- Silent mode mutes all device audio output.
- Suppress invalid DPAD inputs to correct character sprite glitch.
- Color decoding bug corrected (greyscale check).

## v17.0

### Note
The MCU must be updated to v0.12.X or newer as the underlying communication protocol changed.

### Changed
- Enable backlight switch on independent from MCU communication.
- Supports updated UART protocol.

## v16.0

### Note
The version of the IDE was switched to Gowin IDE v1.9.9.03. This resolves a synthensis bug which resulted in a palette flickering issue. Do not build with Gowin IDE v1.9.9.02 or older going forward.

### Added
- Reset the FPGA core when a cartridge is removed.
- Add hotkey for reset (Menu+A+B+Start+Select).
- Add hotkeys for brightness (Menu+Left/Right).

### Changed
- Only show icon overlays when the OSD is off.
- USB CDC descriptors for Linux & macOS.

### Fixed
- Improved AA/LiPo detection logic to resolve critical battery false positives.
- Fixes palette flickering issue seen on some games like Tetris.

## v13.1
This is the first production release.

## v13.0 and older
A preliminary engineering release.
