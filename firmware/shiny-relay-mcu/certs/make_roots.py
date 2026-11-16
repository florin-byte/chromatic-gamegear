#!/usr/bin/env python3
"""make_roots.py (lane fwshrink1): certs/shiny_relay_roots.pem, the TLS roots the firmware keeps, copied byte for byte
from ESP-IDF's Mozilla list (components/mbedtls/esp_crt_bundle/cacrt_all.pem of the IDF that builds the release).

Every https host the firmware talks to (the device API api.shinyrelay.com / betaapi.shinyrelay.com and the hosts the
backend can name for blobs and updates: dl / betadl.shinyrelay.com, install.shinyrelay.com; api.shinygen.ai of the
older releases) sits behind Cloudflare's edge or Google's. Today: Let's Encrypt (YE1 -> Root YE -> ISRG Root X2 ->
ISRG Root X1) and Google Trust Services (WE1 -> GTS Root R4 -> GlobalSign Root CA). Cloudflare issues its edge
certificates from Let's Encrypt, Google Trust Services, SSL.com and Sectigo, and has used DigiCert; every root of
those CAs (and the roots that cross-sign them) is kept, so a change of CA at the edge still verifies.
The IDF's bundle check wants the issuer of the LAST certificate a server sends in the bundle: cross-signing roots
(ISRG Root X1, GlobalSign Root CA, AAA Certificate Services) stay for that reason.

Plus ISRG Root YE (extra/isrg_root_ye.pem, from letsencrypt.org/certs/gen-y/root-ye.pem; sha256 pinned below), newer
than the IDF's list: the root of today's YE1 chain, an ECDSA P-384 key. With it and ISRG Root X2 in the bundle the firmware's
verify callback (components/cwl/cwl_tls.c) ends a Let's Encrypt chain at a certificate whose subject and key ARE a kept root
(the Root YE or X2 cross-signs the edge sends), so the last check is an ECDSA one, never X1's RSA-4096 signature.

  python3 make_roots.py IDF_PATH   writes shiny_relay_roots.pem next to this file
"""
import os, re, subprocess, sys

KEEP = [
    'ISRG Root X1', 'ISRG Root X2',                                              # Let's Encrypt
    'GTS Root R1', 'GTS Root R2', 'GTS Root R3', 'GTS Root R4', 'GlobalSign Root CA',  # Google Trust Services
    'SSL.com Root Certification Authority RSA', 'SSL.com Root Certification Authority ECC',
    'SSL.com TLS RSA Root CA 2022', 'SSL.com TLS ECC Root CA 2022',               # SSL.com
    'USERTrust RSA Certification Authority', 'USERTrust ECC Certification Authority',
    'AAA Certificate Services',                                                  # Sectigo
    'DigiCert Global Root CA', 'DigiCert Global Root G2', 'DigiCert Global Root G3',  # DigiCert
]

EXTRA = [('ISRG Root YE (Let\'s Encrypt gen Y)', 'extra/isrg_root_ye.pem',
          'E14FFCAD5B0025731006CAA43A121A22D8E9700F4FB9CF852F02A708AA5D5666')]

def main():
    src = os.path.join(sys.argv[1], 'components/mbedtls/esp_crt_bundle/cacrt_all.pem')
    text = open(src).read()
    out, found = [], {}
    for m in re.finditer(r'(-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----)', text, re.S):
        pem = m.group(1)
        subj = subprocess.run(['openssl', 'x509', '-noout', '-subject', '-nameopt', 'multiline'], input=pem.encode(),
                              capture_output=True, check=True).stdout.decode()
        cn = re.search(r'commonName\s*=\s*(.*)', subj)
        ou = re.search(r'organizationalUnitName\s*=\s*(.*)', subj)
        name = (cn or ou).group(1).strip()
        if name in KEEP and name not in found:
            found[name] = pem
    missing = [k for k in KEEP if k not in found]
    if missing:
        sys.exit('not in the IDF list: ' + ', '.join(missing))
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, 'shiny_relay_roots.pem'), 'w') as f:
        f.write('# Shiny Relay TLS roots (certs/make_roots.py): %d of the IDF Mozilla list + %d pinned extra\n' % (len(KEEP), len(EXTRA)))
        for k in KEEP:
            f.write('\n' + k + '\n' + found[k] + '\n')
        for name, rel, fp in EXTRA:
            pem = open(os.path.join(here, rel)).read().strip()
            got = subprocess.run(['openssl', 'x509', '-noout', '-fingerprint', '-sha256'], input=pem.encode(),
                                 capture_output=True, check=True).stdout.decode().split('=')[1].strip().replace(':', '')
            if got != fp:
                sys.exit('%s: sha256 %s, want %s' % (rel, got, fp))
            f.write('\n' + name + '\n' + pem + '\n')
    print('%d roots written' % (len(KEEP) + len(EXTRA)))

if __name__ == '__main__':
    main()
