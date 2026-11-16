// check.c BUNDLE CHAIN.pem [idf|fw]: the certificate check on the host: the chain a server sends, verified against a
// generated x509_crt_bundle as esp_crt_bundle_attach sets it up (a dummy CA) with the IDF's own callback (idf) or the
// firmware's (fw: components/cwl/cwl_tls.c CwlTls_Verify, the default). Counts the RSA signature checks it made.
#include "esp_crt_bundle.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/rsa.h"
#include <string.h>
const uint8_t *x509_crt_imported_bundle_bin_start, *x509_crt_imported_bundle_bin_end;
int CwlTls_Verify(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags);
/* run.sh builds rsa.c with mbedtls_rsa_pkcs1_verify renamed: every RSA signature check (pk_wrap.c) comes through here */
static int g_rsa_verifies;
int real_mbedtls_rsa_pkcs1_verify(mbedtls_rsa_context *ctx, mbedtls_md_type_t md_alg, unsigned int hashlen, const unsigned char *hash, const unsigned char *sig);
int mbedtls_rsa_pkcs1_verify(mbedtls_rsa_context *ctx, mbedtls_md_type_t md_alg, unsigned int hashlen, const unsigned char *hash, const unsigned char *sig)
{
    g_rsa_verifies++;
    return real_mbedtls_rsa_pkcs1_verify(ctx, md_alg, hashlen, hash, sig);
}
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb"); static uint8_t b[200000]; size_t n = fread(b, 1, sizeof b, f); fclose(f);
    x509_crt_imported_bundle_bin_start = b; x509_crt_imported_bundle_bin_end = b + n;
    const int fw = argc < 4 || strcmp(argv[3], "idf");
    mbedtls_ssl_config conf; mbedtls_ssl_config_init(&conf);
    if (esp_crt_bundle_attach(&conf) != ESP_OK) { printf("attach failed\n"); return 2; }
    mbedtls_x509_crt chain; mbedtls_x509_crt_init(&chain);
    if (mbedtls_x509_crt_parse_file(&chain, argv[2]) != 0) { printf("chain parse failed\n"); return 2; }
    mbedtls_x509_crt dummy; mbedtls_x509_crt_init(&dummy);
    uint32_t flags = 0;
    int r = mbedtls_x509_crt_verify(&chain, &dummy, NULL, NULL, &flags, fw ? CwlTls_Verify : esp_crt_verify_callback, NULL);
    printf("%s [%s]: verify %d flags %08x, RSA signature checks %d -> %s\n", argv[2], fw ? "fw" : "idf", r, flags, g_rsa_verifies, r == 0 && flags == 0 ? "OK" : "REFUSED");
    return r == 0 && flags == 0 ? 0 : 1;
}
