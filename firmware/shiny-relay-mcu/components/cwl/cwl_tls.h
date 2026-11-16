// cwl_tls.h (lane fwshrink1): the certificate check every HTTPS connection uses (cwl_tls.c says how and why).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "mbedtls/x509_crt.h"

// esp_http_client_config_t.crt_bundle_attach: the IDF's bundle, plus the bundled-root-by-key check in front of it
esp_err_t CwlTls_Attach(void *conf);
// 1 when crt's subject and public key are those of a root in the bundle (gen_crt_bundle.py's format)
int CwlTls_IsRoot(const uint8_t *bundle, size_t len, const mbedtls_x509_crt *crt);
// the verify callback CwlTls_Attach sets (mbedTLS f_vrfy)
int CwlTls_Verify(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags);
