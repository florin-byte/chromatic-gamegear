// See wifi_qr.h.
#include "wifi_qr.h"

#include <string.h>

#ifdef WIFIQR_HOST
#include "qrcodegen.h"
#else
#include "extra/libs/qrcode/qrcodegen.h"
#endif

int WifiQr_Render(const char *Text, uint8_t *Buf, int W, int Stride)
{
    memset(Buf, 0xFF, (size_t)Stride * (size_t)W);
    const size_t len = strlen(Text);
    static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(kWifiQr_MaxVersion)];
    static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(kWifiQr_MaxVersion)];
    if (len > sizeof(tmp)) return -1;
    memcpy(tmp, Text, len);
    if (!qrcodegen_encodeBinary(tmp, len, qr, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, kWifiQr_MaxVersion,
                                qrcodegen_Mask_AUTO, true)) {
        memset(tmp, 0, sizeof(tmp));
        return -1;
    }
    memset(tmp, 0, sizeof(tmp));   // the setup password was in it
    const int size = qrcodegen_getSize(qr);
    const int scale = W / size;
    if (scale < 2) return -1;
    const int margin = (W - size * scale) / 2;
    for (int y = 0; y < size * scale; y++) {
        for (int x = 0; x < size * scale; x++) {
            if (!qrcodegen_getModule(qr, x / scale, y / scale)) continue;
            const int px = x + margin, py = y + margin;
            Buf[py * Stride + (px >> 3)] &= (uint8_t)~(0x80u >> (px & 7));
        }
    }
    return (size - 17) / 4;
}
