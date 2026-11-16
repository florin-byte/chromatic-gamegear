#!/usr/bin/env python3
"""Compose or describe the Chromatic FPGA's 4 MiB boot flash image (layout "rf" with two full designs).

Layout (firmware: components/nw/cartflash/cf_fota.h):
    0x000000-0x0007BF  31 full-design jump headers, 64 bytes each, written from 0x780 down; the lowest complete one boots
    0x0007C0           the recovery header (-> 0x000800)
    0x000800-0x05BFFF  the recovery design
    0x05C000-0x25DFFF  the game area
    0x260000-0x32FFFF  slot GG: the Game Gear design, at the top of the slot
    0x330000-0x3FFFFF  slot GB: the Game Boy design, at the top of the slot

`compose` takes the recovery part from a dump of a unit's flash, puts the two designs in their slots and writes
one fresh level-1 header at 0x780 to selector A and its level-2 header to the live design
(30 places left at each level). --scheme direct retains the original single-level layout. The image is written with the
Gowin cable:  openFPGALoader -c gwu2x --external-flash -o 0 --verify image.bin

Usage:
    chromatic_flash_image.py describe IMAGE
    chromatic_flash_image.py compose --base DUMP --gb GB.bin --gg GG.bin --live gb|gg -o IMAGE
"""
import argparse
import sys
from pathlib import Path

FLASH = 0x400000
HDR_STEP, HDR_AREA, REC_HDR, FULL_TOP = 0x40, 0x800, 0x7C0, 0x780
REC_BASE, GAME_BASE = 0x000800, 0x05C000
SLOT_MAX = 0x0D0000                      # CF_FOTA_FULL_MAX
SLOT_END = {"gb": 0x400000, "gg": 0x330000}
GAME_END = SELECTOR_A = 0x25E000
SELECTOR_B = 0x25F000
SECTOR = 0x1000
IDCHK = bytes([0x06, 0x00, 0x00, 0x00, 0x00, 0x01, 0x28, 0x1B])


def crc16(data: bytes) -> int:
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return c


def header(target: int) -> bytes:
    """The 64-byte place of a jump header to `target` (cf_fota_header)."""
    out = bytearray(b"\xFF" * 64)
    out[24:28] = b"\xDE" * 4
    out[34:36] = b"\xA5\xC3"
    body = IDCHK + bytes([0x6E, 0x80, 0x00, 0x00]) + target.to_bytes(4, "big")
    out[36:52] = body
    out[52:54] = crc16(body).to_bytes(2, "little")
    return bytes(out)


def slot_base(slot: str, size: int) -> int:
    if size > SLOT_MAX:
        raise ValueError("design of %d bytes: bigger than a slot (%d)" % (size, SLOT_MAX))
    return SLOT_END[slot] - ((size + SECTOR - 1) & ~(SECTOR - 1))


def slot_of(target: int):
    for name, end in SLOT_END.items():
        if end - SLOT_MAX <= target < end:
            return name
    return None


def headers(img: bytes, sector=0):
    """[(place, state, target)] for every written place; state: live / invalidated / partial."""
    res = []
    for off in range(REC_HDR if not sector else FULL_TOP, -1, -HDR_STEP):
        at = sector + off
        h = img[at:at + 64]
        if h == b"\xFF" * 64:
            continue
        target = int.from_bytes(h[48:52], "big")
        body_ok = h[:34] + h[36:] == header(target)[:34] + header(target)[36:]
        state = "live" if body_ok and h[34:36] in (b"\xA5\xC3", b"\xA5\xCB") else "invalidated" if body_ok and h[34:36] == b"\x00\x00" else "partial"
        res.append((at, state, target))
    return res


def describe(img: bytes) -> str:
    if len(img) != FLASH:
        raise ValueError("an image is 4 MiB")
    lines = []
    blank_selector = False
    hs = headers(img)
    full = [h for h in hs if h[0] < REC_HDR]
    live = [h for h in full if h[1] == "live"]
    boots = min(live)[2] if live else None
    for at, state, target in hs:
        lines.append("header %03x %-11s -> %06x%s" % (at, state, target, "  (recovery)" if at == REC_HDR else
                                                     "  BOOTS" if live and at == min(live)[0] else ""))
    free = min((at for at, _, _ in full), default=REC_HDR) // HDR_STEP
    lines.append("free full header places: %d" % free)
    if boots in (SELECTOR_A, SELECTOR_B):
        sector = boots
        lines.append("level-1 target: selector %s at %06x" % ("A" if sector == SELECTOR_A else "B", sector))
        l2 = headers(img, sector)
        for at, state, target in l2:
            lines.append("selector header %06x %-11s -> %06x" % (at, state, target))
        lowest = min((at - sector for at, _, _ in l2), default=FULL_TOP + HDR_STEP)
        lines.append("free level-2 header places: %d" % (lowest // HDR_STEP))
        complete = [h for h in l2 if h[1] == "live"]
        boots = min(complete)[2] if complete else None
        if boots is None:
            blank_selector = True
            lines.append("boots: nothing (selector has no complete header)")
    else:
        lines.append("scheme: direct")
    if not blank_selector:
        lines.append("boots: %s" % ("slot %s at %06x" % (slot_of(boots) or "?", boots) if boots is not None else "the recovery design"))
    used = [img[s:s + SECTOR] != b"\xFF" * SECTOR for s in range(0, FLASH, SECTOR)]
    s = 0
    while s < len(used):
        if used[s]:
            e = s
            while e < len(used) and used[e]:
                e += 1
            lines.append("used %06x-%06x (%d KiB)" % (s * SECTOR, e * SECTOR, (e - s) * SECTOR // 1024))
            s = e
        else:
            s += 1
    return "\n".join(lines)


def compose(base: bytes, gb: bytes, gg: bytes, live: str, scheme="chain") -> bytes:
    if scheme not in ("chain", "direct"):
        raise ValueError("unknown scheme")
    if len(base) != FLASH:
        raise ValueError("the base dump is 4 MiB")
    if base[REC_HDR:REC_HDR + 64] != header(REC_BASE):
        raise ValueError("the base dump has no recovery header at 0x7C0")
    img = bytearray(b"\xFF" * FLASH)
    img[REC_HDR:GAME_BASE] = base[REC_HDR:GAME_BASE]            # the recovery header and design, as on the unit
    for slot, design in (("gb", gb), ("gg", gg)):
        if design[:64].find(b"\xA5\xC3") < 0:
            raise ValueError("the %s design does not start like a Gowin bitstream" % slot)
        at = slot_base(slot, len(design))
        img[at:at + len(design)] = design
    target = slot_base(live, len(gb if live == "gb" else gg))
    if scheme == "chain":
        img[SELECTOR_A + FULL_TOP:SELECTOR_A + FULL_TOP + 64] = header(target)
        target = SELECTOR_A
    img[FULL_TOP:FULL_TOP + 64] = header(target)
    return bytes(img)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("describe")
    d.add_argument("image", type=Path)
    c = sub.add_parser("compose")
    c.add_argument("--base", type=Path, required=True)
    c.add_argument("--gb", type=Path, required=True)
    c.add_argument("--gg", type=Path, required=True)
    c.add_argument("--scheme", choices=("chain", "direct"), default="chain")
    c.add_argument("--live", choices=("gb", "gg"), required=True)
    c.add_argument("-o", "--out", type=Path, required=True)
    a = ap.parse_args(argv)
    try:
        if a.cmd == "describe":
            print(describe(a.image.read_bytes()))
        else:
            img = compose(a.base.read_bytes(), a.gb.read_bytes(), a.gg.read_bytes(), a.live, a.scheme)
            a.out.write_bytes(img)
            print(describe(img))
    except (OSError, ValueError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
