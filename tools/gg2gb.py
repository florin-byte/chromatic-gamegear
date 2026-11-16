#!/usr/bin/env python3
"""Wrap a Game Gear ROM so the unmodified Shiny Relay firmware loads it as a card game.

The Chromatic Game Gear FPGA design reads the game from the RAM cart area in PSRAM. The firmware only
loads files that look like Game Boy ROMs, so the Game Gear ROM gets a 32 KiB prefix with a minimal
Game Boy header (MBC5 + RAM + BATTERY, 32 KiB: the firmware then keeps the save in /SAVES) and a marker the FPGA checks:

    0x0134  title (ASCII, up to 15 characters)
    0x0147  0x1B (MBC5 + RAM + BATTERY), 0x0148 ROM size code of the whole file, 0x0149 0x03 (32 KiB of RAM)
    0x014D  header checksum, 0x014E global checksum (big-endian)
    0x0150  "GGCHROM1"
    0x0158  Game Gear ROM size in bytes (u32, little-endian)
    0x015C  CRC32 of the Game Gear ROM (u32, little-endian)
    0x8000  the Game Gear ROM, then 0xFF up to a power of two

Usage: gg2gb.py <game.gg> [more.gg ...] [-o OUT_DIR]
Output: <game>.gg.gb next to the input (or in OUT_DIR).
"""
import argparse
import re
import sys
import zlib
from pathlib import Path

PREFIX = 0x8000
MAGIC = b"GGCHROM1"
MAX_TOTAL = 4 * 1024 * 1024  # the RAM cart's room


def title_from_name(name: str) -> bytes:
    """Up to 15 upper-case ASCII characters made from the file name."""
    text = re.sub(r"[^A-Za-z0-9 ]+", " ", name).upper()
    text = re.sub(r" +", " ", text).strip()
    return (text[:15].rstrip() or "GAME GEAR").encode("ascii")


def wrap(rom: bytes, name: str = "GAME GEAR") -> bytes:
    """Return the wrapped image for a Game Gear ROM."""
    if len(rom) % 0x4000 == 512:  # 512-byte copier header
        rom = rom[512:]
    if len(rom) == 0:
        raise ValueError("empty ROM")
    total = 0x10000
    while total < PREFIX + len(rom):
        total *= 2
    if total > MAX_TOTAL:
        raise ValueError("ROM too big: %d bytes (the limit is %d)" % (len(rom), MAX_TOTAL - PREFIX))
    out = bytearray(b"\xFF" * total)
    out[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])
    out[0x104:0x134] = bytes(0x30)
    out[0x134:0x144] = title_from_name(name).ljust(16, b"\x00")
    out[0x144:0x147] = bytes(3)
    out[0x147] = 0x1B
    out[0x148] = (total // 0x8000).bit_length() - 1
    out[0x149] = 0x03
    out[0x14A] = 0x01
    out[0x14B] = 0x00
    out[0x14C] = 0x00
    out[0x150:0x158] = MAGIC
    out[0x158:0x15C] = len(rom).to_bytes(4, "little")
    out[0x15C:0x160] = (zlib.crc32(rom) & 0xFFFFFFFF).to_bytes(4, "little")
    out[PREFIX:PREFIX + len(rom)] = rom
    check = 0
    for i in range(0x134, 0x14D):
        check = (check - out[i] - 1) & 0xFF
    out[0x14D] = check
    out[0x14E:0x150] = bytes(2)
    out[0x14E:0x150] = (sum(out) & 0xFFFF).to_bytes(2, "big")
    return bytes(out)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Wrap Game Gear ROMs for the Chromatic Game Gear core.")
    parser.add_argument("roms", nargs="+", type=Path)
    parser.add_argument("-o", "--out-dir", type=Path)
    args = parser.parse_args(argv)
    for path in args.roms:
        try:
            image = wrap(path.read_bytes(), path.stem)
        except (OSError, ValueError) as error:
            print("%s: %s" % (path, error), file=sys.stderr)
            return 1
        out_dir = args.out_dir or path.parent
        out_dir.mkdir(parents=True, exist_ok=True)
        out = out_dir / (path.name + ".gb")
        out.write_bytes(image)
        print("%s -> %s (%d KiB)" % (path.name, out, len(image) // 1024))
    return 0


if __name__ == "__main__":
    sys.exit(main())
