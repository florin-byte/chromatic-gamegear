// cwl_tls.c (lane fwshrink1): every HTTPS connection's certificate check (esp_http_client .crt_bundle_attach =
// CwlTls_Attach). The IDF's bundle attach as before, then this file's verify callback in front of the IDF's:
//
// The IDF's bundle check trusts a chain by the issuer of its LAST certificate: it looks that issuer up in the bundle
// and checks the last certificate's signature with the bundled key. Since 2026-10 the shinyrelay.com edge (Let's
// Encrypt) sends leaf -> YE1 -> Root YE (signed by ISRG Root X2) -> ISRG Root X2 (signed by ISRG Root X1), so that
// last check is an RSA-4096 signature by X1, and on this ESP32 it runs out of heap mid-handshake (PK verify
// -0x4290: MPI alloc failed, lowest heap ~1 KB from ~24 KB).
//
// A certificate whose subject AND public key are those of a root in the bundle is that root, re-signed by another
// CA (a cross-sign): its key is already trusted, so its own signature need not be checked. This callback runs first
// for every certificate of the chain (mbedTLS calls it from the top down): such a certificate is accepted on the
// spot (only when "not trusted" is its only flag, as the IDF's callback has it), and every certificate below it has
// already been checked by mbedTLS against its parent in the chain (ECDSA P-384 for the YE chain). Anything else goes
// to the IDF's callback as before. Roots for this: ISRG Root X2 and ISRG Root YE (certs/make_roots.py).
//
// Each handshake's check prints one line: where it ended (depth, the certificate's CN, how).
#include <stdio.h>
#include <string.h>
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/oid.h"
#include "esp_crt_bundle.h"
#include "cwl_tls.h"

#ifndef CWL_TLS_HOST
extern const uint8_t x509_crt_imported_bundle_bin_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_imported_bundle_bin_end[] asm("_binary_x509_crt_bundle_end");
#else   /* certs/host/run.sh: the bundle file read at run time */
extern const uint8_t *x509_crt_imported_bundle_bin_start, *x509_crt_imported_bundle_bin_end;
#endif
int esp_crt_verify_callback(void *buf, mbedtls_x509_crt *crt, int depth, uint32_t *flags);   // esp_crt_bundle.c

// the bundle as gen_crt_bundle.py writes it: u16 count, then per root u16 name length, u16 key length (big-endian),
// the subject Name (DER) and the SubjectPublicKeyInfo (DER)
int CwlTls_IsRoot(const uint8_t *bundle, size_t len, const mbedtls_x509_crt *crt)
{
    if (len < 2) return 0;
    const uint8_t *p = bundle + 2, *end = bundle + len;
    for (unsigned i = 0, n = (unsigned)(bundle[0] << 8 | bundle[1]); i < n && p + 4 <= end; i++) {
        const size_t nl = (size_t)(p[0] << 8 | p[1]), kl = (size_t)(p[2] << 8 | p[3]);
        const uint8_t *name = p + 4, *key = name + nl;
        if (key + kl > end) return 0;
        if (nl == crt->subject_raw.len && kl == crt->pk_raw.len && !memcmp(name, crt->subject_raw.p, nl) && !memcmp(key, crt->pk_raw.p, kl))
            return 1;
        p = key + kl;
    }
    return 0;
}

static void cn_of(const mbedtls_x509_name *n, char *out, size_t cap)
{
    out[0] = 0;
    for (; n; n = n->next) {
        if (!MBEDTLS_OID_CMP(MBEDTLS_OID_AT_CN, &n->oid)) {
            const size_t l = n->val.len < cap - 1 ? n->val.len : cap - 1;
            memcpy(out, n->val.p, l);
            out[l] = 0;
            return;
        }
    }
}

int CwlTls_Verify(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    char cn[40];
    if ((*flags & ~(uint32_t)MBEDTLS_X509_BADCERT_BAD_MD) == MBEDTLS_X509_BADCERT_NOT_TRUSTED &&
        CwlTls_IsRoot(x509_crt_imported_bundle_bin_start, (size_t)(x509_crt_imported_bundle_bin_end - x509_crt_imported_bundle_bin_start), crt)) {
        *flags = 0;
        cn_of(&crt->subject, cn, sizeof cn);
        printf("tls: chain trusted at depth %d: %s is a bundled root (subject + key), its own signature not checked\n", depth, cn);
        return 0;
    }
    const uint32_t before = *flags;
    const int r = esp_crt_verify_callback(ctx, crt, depth, flags);
    if (before & MBEDTLS_X509_BADCERT_NOT_TRUSTED) {   // where the IDF's bundle check ran (or would have)
        cn_of(&crt->issuer, cn, sizeof cn);
        if (r == 0 && *flags == 0) printf("tls: chain trusted at depth %d: the bundle's %s signed it\n", depth, cn);
        else printf("tls: chain REFUSED at depth %d (flags %08lx, issuer %s)\n", depth, (unsigned long)(r ? (uint32_t)-1 : *flags), cn);
    }
    return r;
}

esp_err_t CwlTls_Attach(void *conf)
{
    const esp_err_t e = esp_crt_bundle_attach(conf);   // the bundle, the dummy CA, the IDF's callback
    if (e == ESP_OK && conf) mbedtls_ssl_conf_verify((mbedtls_ssl_config *)conf, CwlTls_Verify, NULL);
    return e;
}
