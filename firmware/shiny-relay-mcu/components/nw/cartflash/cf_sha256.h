/* cf_sha256.h: SHA-256, small and portable (lane cartflash1). The cart job engine hashes the
 * image as it reads the cart back; the same code runs in the host simulation. */
#ifndef CF_SHA256_H
#define CF_SHA256_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t bytes;
    uint8_t  buf[64];
    uint32_t fill;
} cf_sha256_t;

void cf_sha256_init(cf_sha256_t *s);
void cf_sha256_update(cf_sha256_t *s, const uint8_t *p, size_t n);
void cf_sha256_final(cf_sha256_t *s, uint8_t out[32]);
/* "0123...": 64 lowercase hex -> 32 bytes. 0 ok. */
int  cf_hex32(const char *hex64, uint8_t out[32]);
#endif
