#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "mbedtls/ssl.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NO_MEM 0x101
#define CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_MAX_CERTS 200
extern const uint8_t *x509_crt_imported_bundle_bin_start;
extern const uint8_t *x509_crt_imported_bundle_bin_end;
esp_err_t esp_crt_bundle_attach(void *conf);
void esp_crt_bundle_detach(mbedtls_ssl_config *conf);
esp_err_t esp_crt_bundle_set(const uint8_t *x509_bundle, size_t bundle_size);
int esp_crt_verify_callback(void *buf, mbedtls_x509_crt *crt, int depth, uint32_t *flags);
