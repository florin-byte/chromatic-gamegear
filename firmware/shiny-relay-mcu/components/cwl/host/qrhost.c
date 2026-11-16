// Prints the phone-setup QR exactly as the OSD draws it (wifi_qr.c):
// W lines of W chars, '#' dark, '.' light.   qrhost W "WIFI:..."
#include <stdio.h>
#include <stdlib.h>
#include "wifi_qr.h"
int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    const int w = atoi(argv[1]), stride = (w + 7) / 8;
    unsigned char *buf = malloc((size_t)stride * (size_t)w);
    const int v = WifiQr_Render(argv[2], buf, w, stride);
    fprintf(stderr, "version %d (%d modules) in %d px\n", v, v > 0 ? 17 + 4 * v : 0, w);
    for (int y = 0; y < w; y++) {
        for (int x = 0; x < w; x++) putchar((buf[y * stride + (x >> 3)] >> (7 - (x & 7))) & 1 ? '.' : '#');
        putchar('\n');
    }
    return v > 0 ? 0 : 1;
}
