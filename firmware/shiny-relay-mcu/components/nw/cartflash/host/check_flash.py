#!/usr/bin/env python3
"""check_flash.py BUILD_DIR SOURCE_DIR: flash operations of the firmware against the REAL partition table.

usb-cart-2 / -3 (2026-10-12) aborted on a unit when the staged ROM was to be erased: the staging
area (0x208000-0x400000) lies partly inside the running app's partition (stock table: factory
0x10000 + 0x300000), where the IDF refuses raw erase / write through the default flash chip
(CONFIG_SPI_FLASH_DANGEROUS_WRITE_ABORTS). The host simulation has no flash chip and no
partition table. This runs in the firmware build and fails it when:
  1. the staging area touches the bootloader, the partition table, or any partition other than
     the app's (NVS, PHY data: a staged ROM would destroy pairing or calibration);
  2. the staging area lies (partly) in a region the IDF guards, and any raw esp_flash erase /
     write in our sources uses the default chip (NULL) instead of the staging view (stage_chip());
  3. the staging offsets in the source are not the ones checked here.
"""
import os, re, struct, sys

build, src = sys.argv[1], sys.argv[2]
STAGE_LO, STAGE_HI = 0x208000, 0x400000
bad = 0
def fail(msg):
    global bad
    bad += 1
    print("check_flash: " + msg)

tab = open(os.path.join(build, "partition_table", "partition-table.bin"), "rb").read()
parts = []
for i in range(0, len(tab), 32):
    e = tab[i:i + 32]
    if len(e) < 32 or e[:2] != b"\xAA\x50":
        break
    typ, sub, off, size = struct.unpack("<BBII", e[2:12])
    parts.append((e[12:28].split(b"\0")[0].decode(), typ, sub, off, size))
apps = [p for p in parts if p[1] == 0]
# 1.1.0: one factory app (1.0.x), or two OTA slots ota_0 0x10000 + ota_1 0x210000 (partitions_ota.csv): the staged
# ROM then goes in the slot that is NOT running (cf_usb.h CF_STAGE_ROM_A 0x210000 / _B 0x010000), the save at 0x208000
ota = sorted(p for p in apps if p[1] == 0 and p[2] in (0x10, 0x11))
regions = [(0x208000, 0x210000, "the save")]
if len(apps) == 1:
    regions.append((0x210000, 0x400000, "the ROM"))
elif len(ota) == 2 and [p[0] for p in ota] == ["ota_0", "ota_1"] and ota[0][3] == 0x10000 and ota[1][3] == 0x210000:
    regions += [(0x210000, 0x400000, "the ROM while ota_0 runs"), (0x010000, 0x200000, "the ROM while ota_1 runs")]
    for name, rom_lo, rom_hi in (("ota_1", 0x210000, 0x400000), ("ota_0", 0x10000, 0x200000)):
        q = [p for p in ota if p[0] == name][0]
        if q[3] < rom_lo or q[3] + q[4] > rom_hi:
            fail("%s (%06x + %06x) is not inside the ROM's place for the other slot" % (name, q[3], q[4]))
else:
    fail("expected one factory app, or ota_0 at 0x10000 + ota_1 at 0x210000; found %s" % ", ".join("%s %06x" % (p[0], p[3]) for p in apps))
guarded = [(0, 0x9000, "bootloader + partition table")] + [(p[3], p[3] + p[4], "the app partition '%s'" % p[0]) for p in apps]
for name, typ, sub, off, size in parts:
    for lo, hi, what in regions:
        if typ != 0 and off < hi and off + size > lo:
            fail("%s (%06x-%06x) overlaps the data partition '%s' (%06x + %06x)" % (what, lo, hi, name, off, size))
for lo, hi, what in regions:
    if lo < 0x9000:
        fail("%s reaches the bootloader / partition table" % what)
inside = [g for g in guarded if any(g[0] < hi and g[1] > lo for lo, hi, _ in regions)]

hdr = open(os.path.join(src, "components/nw/cartflash/cf_usb.h")).read() + open(os.path.join(src, "components/nw/cartflash/cf_dev.c")).read()
m = re.search(r"#define\s+CF_STAGE_SAV_OFF\s+(0x[0-9A-Fa-f]+)", hdr)
if not m or int(m.group(1), 16) != STAGE_LO:
    fail("CF_STAGE_SAV_OFF in the source is not 0x%06x" % STAGE_LO)

raw = []
for root in ("main", "components"):
    for d, _, files in os.walk(os.path.join(src, root)):
        if "/host" in d or "/build" in d:
            continue
        for f in files:
            if not f.endswith(".c"):
                continue
            p = os.path.join(d, f)
            for n, line in enumerate(open(p, errors="ignore"), 1):
                if re.search(r"\besp_flash_(erase_region|write|erase_chip)\s*\(\s*(NULL|esp_flash_default_chip)\b", line) or re.search(r"\bspi_flash_(erase|write)", line):
                    raw.append("%s:%d" % (os.path.relpath(p, src), n))
if inside and raw:
    fail("raw flash erase / write through the default chip (the IDF aborts inside %s): %s" % (inside[0][2], ", ".join(raw)))
print("flash check: staging %s; guarded there: %s; raw default-chip erase/write calls: %d; partitions: %s" % (
    ", ".join("%s %06x-%06x" % (w, lo, hi) for lo, hi, w in regions), ", ".join("%s %06x-%06x" % (g[2], g[0], g[1]) for g in inside) or "nothing",
    len(raw), ", ".join("%s %06x+%06x" % (p[0], p[3], p[4]) for p in parts)))
sys.exit(1 if bad else 0)
