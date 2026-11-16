# Bluetooth LE transport: PARKED

Parked 2026-10-10 by the owner (focus on Wi-Fi: ESP-NOW + LAN + relay). Nothing
here is compiled; `CONFIG_BT_ENABLED` stays off, so it costs no RAM or flash.

## Where it stands

- `cwl_ble.c` / `cwl_ble.h`: a NimBLE transport, written but **never compiled
  against NimBLE and never run**. GATT service `43574C32-B1E0-4C49-4E4B-000000000000`:
  `...0001` C2P (write / write without response, central to peripheral),
  `...0002` P2C (notify, peripheral to central), `...0003` STAT (read, status text).
  Each write/notification is one `cwl_lx` frame (CWL2 packets unchanged, plus `L`
  control units), so `cwl_proto.c` stays the one core. Both sides advertise + scan;
  lower 48-bit address connects (`CwlLx_BleShouldInitiate`, host-tested). Connection
  parameters: 7.5 ms / latency 0 / 1 s supervision at full power, 15 ms / latency 19
  at low power. Peripheral asks the central (phone, Mac) for the fast interval.
- Intended future job (coordinator, 10-10): Chromatic-to-phone link for the Shiny Gen
  app (Chromatic = GATT peripheral, phone = central). Chromatic-to-Chromatic over BLE
  is low priority: ESP-NOW does that better.

## What it would cost (measured statically, IDF 5.3, this firmware)

- `CONFIG_BT_ENABLED` + NimBLE + BLE-only controller: the controller reserves
  **56 KB** at the start of DRAM (`CONFIG_BTDM_RESERVE_DRAM` 0xdb5c). The static DRAM
  segment then overflows by ~26 KB unless the 46 KB OSD frame buffer (`buffy`,
  main/main.c) moves to the heap (tried: builds, IRAM 16.7 KB left, DRAM 13.9 KB left).
- Runtime heap with Wi-Fi + TLS is only ~32 KB free (min ~7 KB) today, so Bluetooth
  and Wi-Fi cannot both be up: a Bluetooth mode must keep the controller memory
  (`esp_bt_controller_mem_release(CLASSIC_BT)`, ~22 KB back) and leave Wi-Fi off;
  every other mode must give all 56 KB back at boot (`esp_bt_mem_release(BTDM)`),
  so switching into or out of Bluetooth means an ESP32 restart.
- Expected latency: an exchange is request at one connection event, reply at the
  next, so ~2 x interval (host sim `tools/cwl_lx_test.c`: 15 ms at 7.5 ms, 30 ms at
  15 ms). macOS/iOS centrals usually grant 15 ms or 30 ms.

## To resume

1. `CONFIG_BT_ENABLED=y`, `CONFIG_BT_NIMBLE_ENABLED=y`, `CONFIG_BTDM_CTRL_MODE_BLE_ONLY=y`,
   max connections 1, security off, small msys pools (see git history of ble1).
2. Move `buffy` to `heap_caps_calloc(MALLOC_CAP_DMA)`.
3. Add `parked/cwl_ble.c` to the component SRCS and `bt` to REQUIRES; call
   `CwlBle_BootMemory()` first thing in `CWL_Initialize`.
4. Add a BLUETOOTH link mode (Wi-Fi off) that routes frames to `CwlBle_Send` and
   drains BLE frames from the same `CwlRadioRx_t` queue as ESP-NOW.
5. Mac stand-in for the phone: bleak central writing C2P / subscribing P2C.
