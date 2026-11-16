// The phone-setup Wi-Fi QR code (0.17.2, lane fwux1). Pure C over LVGL's
// own qrcodegen, so components/cwl/host can render exactly what the OSD shows.
//
// Why not lv_qrcode_update: it always uses ECC M, which puts the 50-byte
// "WIFI:T:WPA;S:ShinyRelay-XXXX;P:XXXXX-XXXXX-XXXXX;;" at version 4 (33
// modules): 2 px modules need 66+ px, the whole panel height, leaving no row
// for "B: BACK". ECC L fits it in version 3 (29 modules): 2 px modules in
// 58 px + a 2 px white margin = 62 px, and a text row below. A QR shown on a
// clean LCD needs no M-level damage recovery (phone Wi-Fi QR generators use L).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { kWifiQr_Px = 62, kWifiQr_MaxVersion = 3 };

// Renders Text into a W x W 1-bit image (MSB first, Stride bytes per row,
// bit 1 = light, 0 = dark: lv_canvas LV_IMG_CF_INDEXED_1BIT with palette
// index 0 dark, 1 light, as lv_qrcode_create sets it up). Modules are
// W / size px, centred; the margin is light. Returns the QR version used
// (1..kWifiQr_MaxVersion), or -1 (Buf all light) when Text does not fit
// with modules of 2 px or more.
int WifiQr_Render(const char *Text, uint8_t *Buf, int W, int Stride);

#ifdef __cplusplus
}
#endif
