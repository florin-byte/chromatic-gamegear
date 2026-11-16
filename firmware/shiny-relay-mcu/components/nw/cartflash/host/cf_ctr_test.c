/* cf_ctr_test.c (wifiinst1): cf_ctr_xor (cf_install.c) against OpenSSL's AES-256-CTR and the wire
 * format's own test vector (docs/CART_INSTALL_WIRE_FORMAT.md section 6), for every kind of range the
 * device asks: unaligned starts, block edges, the counter's carry, 1 byte, 2,048 bytes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include "../cf_install.h"

static EVP_CIPHER_CTX *ecbx;
static int ecb(void *u, const uint8_t in[16], uint8_t out[16]) { (void)u; int l = 0; return EVP_EncryptUpdate(ecbx, out, &l, in, 16) == 1 && l == 16 ? 0 : -1; }
static void hex(const char *s, uint8_t *o) { for (size_t i = 0; i < strlen(s) / 2; i++) { unsigned v; sscanf(s + 2 * i, "%2x", &v); o[i] = (uint8_t)v; } }
static int ok_n, bad_n;
static void check(int c, const char *w) { if (c) ok_n++; else { bad_n++; printf("  FAIL %s\n", w); } }

int main(void)
{
    uint8_t key[32], iv[16], ref[8192], plain[8192], buf[8192], want[48];
    hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key);
    hex("f0f1f2f3f4f5f6f7f8f9fafbfffffffe", iv);
    ecbx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(ecbx, EVP_aes_256_ecb(), NULL, key, NULL);
    EVP_CIPHER_CTX_set_padding(ecbx, 0);
    for (int i = 0; i < 8192; i++) plain[i] = (uint8_t)(i * 7 + 3);
    /* OpenSSL's CTR over the whole image */
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new(); int l = 0;
    EVP_EncryptInit_ex(c, EVP_aes_256_ctr(), NULL, key, iv); EVP_EncryptUpdate(c, ref, &l, plain, 8192); EVP_CIPHER_CTX_free(c);
    hex("7144e65f24f2ea451e3e423bc7adcbf6", want);
    check(!memcmp(ref, want, 16), "OpenSSL = the wire format's vector (block 0)");
    memcpy(buf, plain, 8192);
    check(cf_ctr_xor(ecb, NULL, iv, 0, buf, 8192) == 0 && !memcmp(buf, ref, 8192), "the whole image = OpenSSL");
    hex("15c90080463cdd90619cfd2ef44f70991fd950e8cc8b5b2b73ae0b6972652033", want);
    check(!memcmp(buf + 16, want, 32), "blocks 1-2: the carry out of the low 32 bits (vector)");
    hex("939f8f830506009347e66e3b0404cd5e", want);
    memcpy(buf, plain + 4101, 16); cf_ctr_xor(ecb, NULL, iv, 4101, buf, 16);
    check(!memcmp(buf, want, 16), "4101..4117: an unaligned range (vector)");
    /* any range: the same bytes as the same range of the whole */
    srand(7); int bad = 0;
    for (int t = 0; t < 4000; t++) {
        uint32_t a = (uint32_t)(rand() % 8192), n = 1 + (uint32_t)(rand() % 2048);
        if (t % 3 == 0) a &= ~15u;
        if (a + n > 8192) n = 8192 - a;
        memcpy(buf, plain + a, n);
        cf_ctr_xor(ecb, NULL, iv, a, buf, n);
        if (memcmp(buf, ref + a, n)) bad++;
    }
    check(bad == 0, "4,000 random ranges (1..2,048 bytes, aligned and not)");
    /* decrypting is the same operation */
    memcpy(buf, ref, 8192); cf_ctr_xor(ecb, NULL, iv, 0, buf, 8192);
    check(!memcmp(buf, plain, 8192), "ciphertext back to plaintext");
    /* the counter wraps mod 2^128 */
    uint8_t ivf[16]; memset(ivf, 0xff, 16); uint8_t z[16] = {0}, a2[32] = {0}, b2[16] = {0};
    cf_ctr_xor(ecb, NULL, ivf, 0, a2, 32); cf_ctr_xor(ecb, NULL, z, 0, b2, 16);
    check(!memcmp(a2 + 16, b2, 16), "ff..ff + 1 = 00..00");
    /* an offset far into a 2 MiB image (counter + 131,071) = starting there from iv + n */
    uint8_t ivn[16]; memcpy(ivn, iv, 16); { uint32_t add = 2097136u / 16u; for (int i = 15; i >= 0 && add; i--) { uint32_t v = ivn[i] + (add & 0xff); ivn[i] = (uint8_t)v; add = (add >> 8) + (v >> 8); } }
    uint8_t p1[16] = {0}, p2[16] = {0}; cf_ctr_xor(ecb, NULL, iv, 2097136u, p1, 16); cf_ctr_xor(ecb, NULL, ivn, 0, p2, 16);
    check(!memcmp(p1, p2, 16), "offset 2,097,136 (the last block of 2 MiB)");
    printf("cf_ctr_test: %d ok, %d failed\n", ok_n, bad_n);
    return bad_n ? 1 : 0;
}
