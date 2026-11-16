#!/bin/zsh
# run.sh BUILD_DIR [TMP]: lane fwshrink1. The certificate check on this Mac with host mbedTLS 3.6.0: the IDF's esp_crt_bundle.c
# (copied unchanged at build time) and the firmware's callback (components/cwl/cwl_tls.c), against the bundle a build made
# (BUILD_DIR/esp-idf/mbedtls/x509_crt_bundle) and the chains our hosts sent (chains/). Every host must verify; the
# Let's Encrypt chains (shinyrelay.com, 4 certificates ending in X2 cross-signed by X1) with NO RSA signature check through
# the firmware's callback; www.swisssign.com (SwissSign Gold CA - G2, a root the trim left out) must be refused.
# Refresh a chain: echo | openssl s_client -connect H:443 -servername H -showcerts | awk '/BEGIN CERT/,/END CERT/'
set -e
H=${0:A:h}; B=${1:A}; T=${2:-/tmp/fwshrink1_bundlecheck}; M=$IDF_PATH/components/mbedtls
[ -d "$M" ] || M=/Users/dwalter/esp/esp-idf-v5.3/components/mbedtls
CWL=$H/../../components/cwl
I=(-I$H/shim -I$M/mbedtls/include -I$M/mbedtls/library)
mkdir -p $T/obj
if [ ! -f $T/libmbed2.a ]; then
  for f in $M/mbedtls/library/*.c; do
    x=(); [ $(basename $f) = rsa.c ] && x=(-Dmbedtls_rsa_pkcs1_verify=real_mbedtls_rsa_pkcs1_verify)
    clang -O1 -w -c $I $x $f -o $T/obj/$(basename $f .c).o
  done
  ar rcs $T/libmbed2.a $T/obj/*.o 2>/dev/null
fi
sed -e 's/^extern const uint8_t x509_crt_imported_bundle_bin_.*;//' $M/esp_crt_bundle/esp_crt_bundle.c > $T/bundle_copy.c
clang -O1 -w $I -DCWL_TLS_HOST -I$CWL $H/check.c $T/bundle_copy.c $CWL/cwl_tls.c $T/libmbed2.a -o $T/check
bad=0
for c in $H/chains/*.pem; do
  n=$(basename $c .pem); want=OK; [ $n = www.swisssign.com ] && want=REFUSED
  for mode in fw idf; do
    r=$($T/check $B/esp-idf/mbedtls/x509_crt_bundle $c $mode 2>/dev/null | tail -1) || true
    rsa=$(echo "$r" | grep -oE "RSA signature checks [0-9]+")
    echo "$n [$mode]: ${r##* -> } (want $want), $rsa"
    [ "${r##* -> }" = $want ] || bad=1
    case $n in *shinyrelay.com) [ $mode = fw ] && [ $want = OK ] && [ "$rsa" != "RSA signature checks 0" ] && { echo "  ^ an RSA check on a Let's Encrypt chain"; bad=1; };; esac
  done
done
$T/check $B/esp-idf/mbedtls/x509_crt_bundle $H/chains/api.shinyrelay.com.pem fw 2>&1 | grep -E "^tls:" | sed 's/^/  api.shinyrelay.com fw: /'
exit $bad
