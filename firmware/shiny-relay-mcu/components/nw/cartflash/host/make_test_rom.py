#!/usr/bin/env python3
"""make_test_rom.py: a SYNTHETIC Game Boy image for the cart install tests (lane cartflash1).
No commercial ROM is ever used in a test or kept in a repo. The image has a valid header and a
tiny program that shows nothing and loops; with --save it writes "CFTEST" + a counter to the
start of save RAM bank 0 on every boot, so a read of the cart RAM proves it ran.
  make_test_rom.py out.gb [--size 262144] [--title "CF TEST"] [--type 0x1B] [--ram 3] [--save] [--bad-header] [--ff-from 2031616]
"""
import argparse, sys
ap = argparse.ArgumentParser()
ap.add_argument("out")
ap.add_argument("--size", type=lambda x: int(x, 0), default=262144)
ap.add_argument("--title", default="CF TEST")
ap.add_argument("--type", type=lambda x: int(x, 0), default=0x1B)
ap.add_argument("--ram", type=lambda x: int(x, 0), default=3)
ap.add_argument("--save", action="store_true")
ap.add_argument("--bad-header", action="store_true")
ap.add_argument("--ff-from", type=lambda x: int(x, 0), default=0, help="FF from this offset to the end")
a = ap.parse_args()
if not 32768 <= a.size <= 2097152:
    sys.exit("size 32768..2097152")
LOGO = bytes.fromhex("CEED6666CC0D000B03730083000C000D0008111F8889000EDCCC6EE6DDDDD999BBBB67636E0EECCCDDDC999FBBB9333E")
r = bytearray((i * 31 + (i >> 9) * 7 + 1) & 0xFF for i in range(a.size))
if a.ff_from:
    r[a.ff_from:] = b"\xFF" * (a.size - a.ff_from)          # e.g. 2031616: a full 2 MiB image the cart can take
r[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])            # nop; jp $0150
r[0x104:0x134] = LOGO
r[0x134:0x144] = a.title.encode("ascii")[:15].ljust(15, b"\0") + b"\x80"
r[0x144:0x14D] = bytes([0, 0, 0, a.type, 0, a.ram, 1, 0x33, 0])
sb = 0
while (0x8000 << sb) < a.size:
    sb += 1
r[0x148] = sb
prog = bytearray([0xF3])                                     # di
if a.save:
    prog += bytes([0x3E, 0x0A, 0xEA, 0x00, 0x00,             # ld a,$0A ; ld [$0000],a   RAM on
                   0xAF, 0xEA, 0x00, 0x40,                   # xor a ; ld [$4000],a      bank 0
                   0x21, 0x00, 0xA0])                        # ld hl,$A000
    for ch in b"CFTEST":
        prog += bytes([0x3E, ch, 0x22])                      # ld a,ch ; ld [hl+],a
    prog += bytes([0x34,                                     # inc [hl]                  the boot counter
                   0xAF, 0xEA, 0x00, 0x00])                  # xor a ; ld [$0000],a      RAM off
prog += bytes([0x76, 0x18, 0xFD])                            # halt ; jr -3
r[0x150:0x150 + len(prog)] = prog
h = 0
for b in r[0x134:0x14D]:
    h = (h - b - 1) & 0xFF
r[0x14D] = h ^ (0x5A if a.bad_header else 0)
g = (sum(r) - r[0x14E] - r[0x14F]) & 0xFFFF
r[0x14E], r[0x14F] = g >> 8, g & 0xFF
open(a.out, "wb").write(r)
import zlib, hashlib
print("%s: %d bytes, crc32 %08x, sha256 %s" % (a.out, len(r), zlib.crc32(r), hashlib.sha256(r).hexdigest()))
