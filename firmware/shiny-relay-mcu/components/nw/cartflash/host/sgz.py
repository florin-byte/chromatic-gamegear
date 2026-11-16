#!/usr/bin/env python3
"""SGZ1: a compressed Game Boy game (lane gamecomp1). The reference encoder / decoder and the golden
vectors every side tests against (mcu cf_sgz.c, shiny_game_data sgz.rs, release web lib/sgz.js).

The format (all numbers little-endian):

  off  size  field
    0     4  magic "SGZ1"
    4     1  version = 1
    5     1  block_shift: 12, 13 or 14 (a block is 1 << block_shift bytes of the ROM: 4, 8 or 16 KiB)
    6     2  nblocks = ceil(rom_size / block)
    8     4  rom_size (bytes; 0x150 .. 4 MiB)
   12     4  rom_crc32 (zlib crc32 of the whole ROM)
   16    32  rom_sha256 (of the whole ROM)
   48    28  the ROM's cartridge header bytes 0x134 .. 0x14F (title, CGB flag, type 0x147, sizes 0x148/0x149, ...)
   76     4  data_size (the bytes of all blocks together)
   80     8  reserved, 0
   88     4  flags, 0 (a decoder refuses any other value)
   92     4  header_crc32: crc32 of bytes 0..91 followed by the whole block table
   96  8*nb  block table, one entry per block:
               u32 w: bits 0..23 = stored length L (1 .. block), bit 31 = RAW (the block is stored as it is), bits 24..30 = 0
               u32 crc32 of the block's ROM bytes
  96+8*nb    the blocks, back to back in order (block i starts at the sum of L of the blocks before it)

A block's ROM bytes are block bytes long (the last one: rom_size - i * block). A RAW block is those bytes as they are
(L = that length). Any other block is ONE raw DEFLATE stream (RFC 1951; no zlib or gzip wrapper) of L bytes that
inflates to exactly that length; each block is compressed on its own (no dictionary from the blocks before it), so a
unit inflates it with 1 block of output buffer. An encoder stores a block RAW when deflate does not make it shorter.
The container is data_start + data_size bytes long (data_start = 96 + 8 * nblocks), nothing after it.

A decoder checks, and refuses on any miss: magic, version, block_shift, nblocks, rom_size, flags, reserved, the header
crc32, each L (1 .. block; RAW: L == the block's length), data_size == sum of L, the container's length, each block
inflating to exactly its length with all its L bytes used, each block's crc32, then rom_crc32 and rom_sha256.

An encoder gives no container when it would not be shorter than the ROM (the caller sends / keeps the ROM itself).

  sgz.py pack ROM OUT [--shift 12]     a container (4 KiB blocks by default)
  sgz.py unpack SGZ OUT                the ROM, every check made
  sgz.py info SGZ
  sgz.py golden DIR                    the golden vectors (ROM + container pairs, and refusals)
"""
import hashlib, os, struct, sys, zlib

MAGIC = b"SGZ1"
VERSION = 1
HDR = 96
RAW = 0x80000000
SHIFTS = (12, 13, 14)
ROM_MIN = 0x150
ROM_MAX = 4 * 1024 * 1024
DEFAULT_SHIFT = 12   # 4 KiB: the unit unpacks with ~7.5 KB of heap (8 KiB blocks: ~11.5 KB, too much at a library play, unit 1 2026-10-22)


class SgzError(Exception):
    pass


def raw_deflate(b, level=9):
    c = zlib.compressobj(level, zlib.DEFLATED, -15, 9)
    return c.compress(b) + c.flush()


def pack(rom, shift=DEFAULT_SHIFT, force=False):
    """The container for rom, or None when it would not be shorter (force: always one, for tests)."""
    if shift not in SHIFTS:
        raise SgzError("block_shift")
    if not (ROM_MIN <= len(rom) <= ROM_MAX):
        raise SgzError("rom size")
    bs = 1 << shift
    nb = (len(rom) + bs - 1) // bs
    table, data = b"", b""
    for i in range(nb):
        blk = rom[i * bs:(i + 1) * bs]
        z = raw_deflate(blk)
        if len(z) < len(blk):
            table += struct.pack("<II", len(z), zlib.crc32(blk))
            data += z
        else:
            table += struct.pack("<II", RAW | len(blk), zlib.crc32(blk))
            data += blk
    head = MAGIC + struct.pack("<BBHII", VERSION, shift, nb, len(rom), zlib.crc32(rom)) + hashlib.sha256(rom).digest()
    head += rom[0x134:0x150] + struct.pack("<I", len(data)) + b"\0" * 8 + struct.pack("<I", 0)
    assert len(head) == 92
    head += struct.pack("<I", zlib.crc32(head + table))
    out = head + table + data
    if not force and len(out) >= len(rom):
        return None
    return out


def parse(z):
    if len(z) < HDR or z[:4] != MAGIC:
        raise SgzError("magic")
    ver, shift, nb, size, crc = struct.unpack_from("<BBHII", z, 4)
    if ver != VERSION:
        raise SgzError("version")
    if shift not in SHIFTS:
        raise SgzError("block_shift")
    if not (ROM_MIN <= size <= ROM_MAX):
        raise SgzError("rom size")
    bs = 1 << shift
    if nb != (size + bs - 1) // bs:
        raise SgzError("nblocks")
    (data_size,) = struct.unpack_from("<I", z, 76)
    if z[80:88] != b"\0" * 8 or struct.unpack_from("<I", z, 88)[0] != 0:
        raise SgzError("flags")
    tend = HDR + 8 * nb
    if len(z) < tend:
        raise SgzError("short")
    (hcrc,) = struct.unpack_from("<I", z, 92)
    if zlib.crc32(z[:92] + z[HDR:tend]) != hcrc:
        raise SgzError("header crc")
    ents, tot = [], 0
    for i in range(nb):
        w, c = struct.unpack_from("<II", z, HDR + 8 * i)
        n = min(bs, size - i * bs)
        L = w & 0xFFFFFF
        if w & 0x7F000000 or L < 1 or L > bs or (w & RAW and L != n):
            raise SgzError("block %d length" % i)
        ents.append((w & RAW != 0, L, c, n))
        tot += L
    if tot != data_size:
        raise SgzError("data size")
    if len(z) != tend + data_size:
        raise SgzError("container length")
    return dict(shift=shift, nblocks=nb, rom_size=size, rom_crc32=crc, rom_sha256=z[16:48], cart=z[48:76],
                data_size=data_size, data_start=tend, blocks=ents)


def unpack(z):
    h = parse(z)
    out, at = bytearray(), h["data_start"]
    for i, (raw, L, c, n) in enumerate(h["blocks"]):
        src = z[at:at + L]
        at += L
        if raw:
            blk = src
        else:
            d = zlib.decompressobj(-15)
            try:
                blk = d.decompress(src, n + 1)
            except zlib.error:
                raise SgzError("block %d inflate" % i)
            if not d.eof or d.unused_data or d.unconsumed_tail or len(blk) != n:
                raise SgzError("block %d inflate" % i)
        if zlib.crc32(blk) != c:
            raise SgzError("block %d crc" % i)
        out += blk
    if zlib.crc32(out) != h["rom_crc32"]:
        raise SgzError("rom crc")
    if hashlib.sha256(out).digest() != h["rom_sha256"]:
        raise SgzError("rom sha256")
    return bytes(out)


# ---------------------------------------------------------------- golden vectors
def _rom(size, seed, ctype=0x1B, rs=None):
    """A deterministic synthetic ROM: code-like runs, a table, random bytes, FF padding, a valid header."""
    r = bytearray()
    x = seed & 0xFFFFFFFF
    while len(r) < size:
        sect = (0, 2, 3, 3, 1, 1, 1, 1, 0, 0, 2, 2, 3, 3, 3, 3)[(len(r) // 4096) % 16]
        for _ in range(4096):
            x = (x * 1103515245 + 12345) & 0xFFFFFFFF
            if sect == 0:
                r.append((len(r) * 7 + (len(r) >> 8)) & 0xFF)      # compressible pattern
            elif sect == 1:
                r.append((x >> 16) & 0xFF)                         # random: stored raw
            elif sect == 2:
                r.append(b"SHINY RELAY GAME "[len(r) % 17])         # text
            else:
                r.append(0xFF)                                     # padding
    r = r[:size]
    logo = bytes.fromhex("CEED6666CC0D000B03730083000C000D0008111F8889000EDCCC6EE6DDDDD999BBBB67636E0EECCCDDDC999FBBB9333E")
    r[0x104:0x134] = logo
    r[0x134:0x144] = b"GOLDEN TEST\0\0\0\0\x80"
    r[0x147] = ctype
    r[0x148] = rs if rs is not None else max(0, (size // 0x8000).bit_length() - 1)
    r[0x149] = 3
    hc = 0
    for b in r[0x134:0x14D]:
        hc = (hc - b - 1) & 0xFF
    r[0x14D] = hc
    return bytes(r)


def golden(d):
    os.makedirs(d, exist_ok=True)
    cases = [("g1_32k_s13", _rom(0x8000, 1), 13), ("g2_odd_s12", _rom(40000, 2), 12), ("g3_64k_s14", _rom(0x10000, 3), 14),
             ("g4_128k_s13", _rom(0x20000, 4), 13)]
    lines = []
    for name, rom, sh in cases:
        z = pack(rom, sh, force=True)
        assert unpack(z) == rom
        open(os.path.join(d, name + ".rom"), "wb").write(rom)
        open(os.path.join(d, name + ".sgz"), "wb").write(z)
        h = parse(z)
        raws = sum(1 for b in h["blocks"] if b[0])
        lines.append("ok %s rom %d sgz %d blocks %d raw %d crc32 %08x sha256 %s" % (name, len(rom), len(z), h["nblocks"], raws, h["rom_crc32"], h["rom_sha256"].hex()))
    # refusals, from g1 (each: the reason a decoder must give up; the name says what was changed)
    z = bytearray(pack(cases[0][1], 13, force=True))
    h = parse(bytes(z))
    first_z = next(i for i, b in enumerate(h["blocks"]) if not b[0])
    zoff = h["data_start"] + sum(b[1] for b in h["blocks"][:first_z])
    def bad(name, f):
        b = bytearray(z)
        f(b)
        try:
            unpack(bytes(b))
        except SgzError as e:
            open(os.path.join(d, name + ".sgz"), "wb").write(bytes(b))
            lines.append("bad %s %s" % (name, e))
            return
        raise AssertionError(name + " was not refused")
    def recrc(b):
        nb = struct.unpack_from("<H", b, 6)[0]
        struct.pack_into("<I", b, 92, zlib.crc32(bytes(b[:92]) + bytes(b[HDR:HDR + 8 * nb])))
    bad("x1_magic", lambda b: b.__setitem__(slice(0, 4), b"SGZ2"))
    bad("x2_version", lambda b: (b.__setitem__(4, 2), recrc(b)))
    bad("x3_header_crc", lambda b: b.__setitem__(40, b[40] ^ 1))
    bad("x4_block_data", lambda b: b.__setitem__(zoff + 3, b[zoff + 3] ^ 0x55))
    def blkcrc(b):
        e = HDR + 8 * first_z + 4
        struct.pack_into("<I", b, e, struct.unpack_from("<I", b, e)[0] ^ 1)
        recrc(b)
    bad("x5_block_crc", blkcrc)
    bad("x6_sha256", lambda b: (b.__setitem__(20, b[20] ^ 1), recrc(b)))
    bad("x7_truncated", lambda b: b.__delitem__(slice(len(b) - 1, len(b))))
    bad("x8_trailing", lambda b: b.extend(b"\0"))
    def rawlen(b):
        i = next(k for k, e in enumerate(h["blocks"]) if e[0])
        e = HDR + 8 * i
        struct.pack_into("<I", b, e, struct.unpack_from("<I", b, e)[0] - 1)
        recrc(b)
    bad("x9_raw_length", rawlen)
    bad("xa_flags", lambda b: (struct.pack_into("<I", b, 88, 1), recrc(b)))
    open(os.path.join(d, "GOLDEN.txt"), "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))


def main(a):
    if len(a) >= 3 and a[0] == "pack":
        sh = int(a[a.index("--shift") + 1]) if "--shift" in a else DEFAULT_SHIFT
        rom = open(a[1], "rb").read()
        z = pack(rom, sh)
        if z is None:
            print("not shorter: no container")
            return 1
        open(a[2], "wb").write(z)
        print("%d -> %d (%.3f)" % (len(rom), len(z), len(z) / len(rom)))
    elif len(a) == 3 and a[0] == "unpack":
        open(a[2], "wb").write(unpack(open(a[1], "rb").read()))
    elif len(a) == 2 and a[0] == "info":
        h = parse(open(a[1], "rb").read())
        print("rom %d crc32 %08x blocks %d x %d raw %d data %d title %r type %02x" % (h["rom_size"], h["rom_crc32"], h["nblocks"], 1 << h["shift"],
              sum(1 for b in h["blocks"] if b[0]), h["data_size"], h["cart"][:16].split(b"\0")[0], h["cart"][0x147 - 0x134]))
    elif len(a) == 2 and a[0] == "golden":
        golden(a[1])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
