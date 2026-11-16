/* cf_wire_check.c (wifirecv1): the REAL server's receive-mode answers through the device's own
 * code. recv_e2e.py drives a local shiny_server_rs as a unit and as a sender would, and hands this
 * the poll answer and the downloaded ciphertext:
 *   cf_wire_check start <start.json>              the code and the poll interval, as cf_install.c reads them
 *   cf_wire_check offer <poll.json> <blob.bin>    cf_install_parse (every field the device needs), then
 *                                                 cf_ctr_xor over the ciphertext and the crc32 + sha256 of it
 * Exit 0 = the device takes it. Never prints the key. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include "../cf_install.h"
#include "../cf_sha256.h"

static EVP_CIPHER_CTX *ecbx;
static int ecb(void *u, const uint8_t in[16], uint8_t out[16]) { (void)u; int l = 0; return EVP_EncryptUpdate(ecbx, out, &l, in, 16) == 1 && l == 16 ? 0 : -1; }
static char *slurp(const char *p, long *n)
{
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(*n + 1); if (fread(b, 1, *n, f) != (size_t)*n) { fclose(f); free(b); return NULL; }
    fclose(f); b[*n] = 0; return b;
}
static uint32_t crc32(const uint8_t *b, long n) { uint32_t c = 0xFFFFFFFFu; for (long i = 0; i < n; i++) { c ^= b[i]; for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xEDB88320u : c >> 1; } return ~c; }

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "start")) {
        long n; char *j = slurp(argv[2], &n); if (!j) return 2;
        /* the same reads as cf_install.c CI_RX_START */
        const char *k = strstr(j, "\"code\""); char code[8] = "";
        if (k && sscanf(k, "\"code\" : \"%7[^\"]\"", code) != 1) sscanf(k, "\"code\":\"%7[^\"]\"", code);
        printf("start: code %s (%s)\n", cf_rx_code_ok(code) ? "valid" : "NOT VALID", cf_rx_code_ok(code) ? "6 of the alphabet" : code);
        return cf_rx_code_ok(code) ? 0 : 1;
    }
    if (argc < 4 || strcmp(argv[1], "offer")) { fprintf(stderr, "usage: cf_wire_check start <json> | offer <json> <blob>\n"); return 2; }
    long jn, bn;
    char *j = slurp(argv[2], &jn); uint8_t *b = (uint8_t *)slurp(argv[3], &bn);
    if (!j || !b) { fprintf(stderr, "cannot read the files\n"); return 2; }
    if (!strstr(j, "\"session\":\"sent\"") && !strstr(j, "\"session\": \"sent\"")) { printf("offer: the answer is not session sent\n"); return 1; }
    cf_offer_t o; const char *why;
    const int r = cf_install_parse(j, "https://api.shinyrelay.com/gbnet/v1", &o, &why);
    if (r != 1) { printf("offer: cf_install_parse = %d (%s): the device would NOT take it\n", r, why); return 1; }
    printf("offer: taken: id %s, title \"%s\", %u bytes, crc32 %08x, blob url ok\n", o.id, o.title, (unsigned)o.size, (unsigned)o.crc32);
    if ((long)o.size != bn) { printf("offer: the blob is %ld bytes, the offer says %u\n", bn, (unsigned)o.size); return 1; }
    ecbx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(ecbx, EVP_aes_256_ecb(), NULL, o.key, NULL);
    EVP_CIPHER_CTX_set_padding(ecbx, 0);
    /* the device's ranges: 2,048 bytes each, as it asks them */
    for (long off = 0; off < bn; off += 2048) {
        const uint32_t len = (uint32_t)(bn - off < 2048 ? bn - off : 2048);
        if (cf_ctr_xor(ecb, NULL, o.iv, (uint32_t)off, b + off, len)) { printf("ctr failed\n"); return 1; }
    }
    const uint32_t c = crc32(b, bn);
    cf_sha256_t s; uint8_t d[32]; char hex[65];
    cf_sha256_init(&s); cf_sha256_update(&s, b, (uint32_t)bn); cf_sha256_final(&s, d);
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", d[i]);
    const int ok = c == o.crc32 && !strcmp(hex, o.sha256);
    printf("decrypt: crc32 %08x %s, sha256 %s\n", (unsigned)c, c == o.crc32 ? "== offer" : "!= OFFER", !strcmp(hex, o.sha256) ? "== offer" : "!= OFFER");
    cf_offer_wipe(&o);
    if (argc >= 5) { FILE *f = fopen(argv[4], "wb"); if (f) { fwrite(b, 1, bn, f); fclose(f); } }
    return ok ? 0 : 1;
}
