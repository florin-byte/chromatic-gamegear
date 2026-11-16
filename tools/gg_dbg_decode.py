#!/usr/bin/env python3
"""Decode the Game Gear probe words of build gg4diag (GG/gg_probe.v).

Input: text with lines from the console (`rc: dbg e=0 a=0861 fills=2980 flags=1b15 ...`) or from the
simulation (`probe 0A a=0861 fills=2980 flags=1b15`). Prints one decoded line per slot (the last value seen).

Usage: gg_dbg_decode.py <file> [<file> ...]
"""
import re
import sys

STATUS = ["loaded", "bad", "idle", "core_reset", "stall_r", "ce_seen", "crc_done", "seq_err", "fill_err",
          "ram_err", "reset_again", "cpu_seen", "consumed_invalid", "alt"]
NAMES = ["0A", "0B", "1A", "1B", "2A", "2B", "3A", "3B"]


def bits(value, hi, lo):
    return (value >> lo) & ((1 << (hi - lo + 1)) - 1)


def decode(word):
    slot = bits(word, 47, 45)
    p = word & ((1 << 45) - 1)
    if slot == 0:
        status = bits(p, 20, 0)
        flags = [n for i, n in enumerate(STATUS) if status >> i & 1]
        return "replay crc16=%04x fills=%d status=%s" % (bits(p, 44, 29), bits(p, 28, 21), ",".join(flags) or "-")
    if slot == 1:
        return "replayed=%d bytes fetches=%d%s" % (bits(p, 44, 23), bits(p, 22, 4), " (saturated)" if bits(p, 22, 22) else "")
    if slot == 2:
        return "h64=%04x h1024=%04x fetches[12:0]=%d" % (bits(p, 44, 29), bits(p, 28, 13), bits(p, 12, 0))
    if slot == 3:
        v = bits(p, 4, 0)
        return "h16384=%04x last_pc=%04x last_op=%02x halt=%d valid(262144,16384,1024,64)=%d%d%d%d" % (
            bits(p, 44, 29), bits(p, 28, 13), bits(p, 12, 5), v >> 4 & 1, v >> 3 & 1, v >> 2 & 1, v >> 1 & 1, v & 1)
    if slot == 4:
        regs = bits(p, 44, 29)
        return "vdp_regs=%04x (display_on=%d M4=%d frame_irq=%d) cram0=%03x cram16=%03x" % (
            regs, regs >> 7 & 1, regs >> 5 & 1, regs >> 8 & 1, bits(p, 28, 17), bits(p, 16, 5))
    if slot == 5:
        return "ram first error addr=%04x expected=%02x got=%02x compares=%d ram_err=%d" % (
            bits(p, 44, 32), bits(p, 31, 24), bits(p, 23, 16), bits(p, 15, 4), p & 1)
    if slot == 6:
        return "h262144=%04x sp=%04x pc~=%04x" % (bits(p, 44, 29), bits(p, 28, 13), bits(p, 12, 0) << 3)
    return "osd row 72: first=%04x last=%04x count=%d seen=%d" % (bits(p, 44, 29), bits(p, 28, 13), bits(p, 12, 5), bits(p, 4, 4))


def main():
    pattern = re.compile(r"a=([0-9a-f]{4}) fills=(\d+|[0-9a-f]{4}) flags=([0-9a-f]{4})")
    for path in sys.argv[1:]:
        last, history = {}, {}
        for line in open(path, encoding="utf-8", errors="replace"):
            m = pattern.search(line)
            if not m:
                continue
            fills = m.group(2)
            # the console prints fills in decimal, the simulation in hex (4 digits after "probe")
            fills = int(fills, 16) if line.lstrip().startswith("probe") else int(fills)
            word = int(m.group(3), 16) << 32 | fills << 16 | int(m.group(1), 16)
            slot = word >> 45
            last[slot] = word
            history.setdefault(slot, []).append(word)
        print("== %s" % path)
        for slot in sorted(last):
            changes = len(set(history[slot]))
            print("%s %012x  %s%s" % (NAMES[slot], last[slot], decode(last[slot]),
                                     "  [%d different values]" % changes if changes > 1 else ""))


if __name__ == "__main__":
    main()
