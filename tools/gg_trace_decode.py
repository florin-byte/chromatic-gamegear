#!/usr/bin/env python3
"""Decode the trace recorder of build gg5diag (GG/gg_trace.v) and compare two traces.

Input: text with lines from the console (`rc: dbg e=0 a=.... fills=<decimal> flags=....`) or from the
simulation (`word a=.... fills=<hex> flags=....`).

Usage:
  gg_trace_decode.py <trace>                 print summary, main trace and pclk trace
  gg_trace_decode.py <trace> --ref <ref>     compare the main traces entry by entry (what the CPU did), print the
                                             entries around the first difference from both
"""
import re
import sys

PATTERN = re.compile(r"a=([0-9a-f]{4}) fills=(\d+|[0-9a-f]{4}) flags=([0-9a-f]{4})")
STATUS = ["loaded", "bad", "idle", "core_reset", "stall_r", "alt", "crc_done", "seq_err", "fill_err", "reset_again",
          "main_full", "pclk_triggered", "pclk_frozen"]


def bits(value, hi, lo):
    return (value >> lo) & ((1 << (hi - lo + 1)) - 1)


def read(path):
    main, pclk, summary = {}, {}, {}
    count = 0
    for line in open(path, encoding="utf-8", errors="replace"):
        m = PATTERN.search(line)
        if not m:
            continue
        fills = int(m.group(2), 16) if line.lstrip().startswith("word") else int(m.group(2))
        word = int(m.group(3), 16) << 32 | fills << 16 | int(m.group(1), 16)
        count += 1
        kind = word >> 46
        if kind == 0:
            main.setdefault(bits(word, 45, 38), {})[bits(word, 37, 36)] = word & 0xFFFFFFFFF
        elif kind == 1:
            pclk.setdefault(bits(word, 45, 37), {})[bits(word, 36, 36)] = word & 0xFFFFFFFFF
        elif kind == 2:
            summary[bits(word, 45, 43)] = word & ((1 << 43) - 1)
    return {"main": main, "pclk": pclk, "summary": summary, "words": count}


def main_entry(parts):
    if len(parts) != 4:
        return None
    p0, p1, p2, p3 = parts[0], parts[1], parts[2], parts[3]
    return {
        "pc": bits(p0, 35, 20), "A": bits(p0, 19, 4), "MC": bits(p0, 3, 1), "M1_n": p0 & 1,
        "rom_do": bits(p1, 35, 28), "D_out": bits(p1, 27, 20), "DI": bits(p1, 19, 12), "IR": bits(p1, 11, 4),
        "TS": bits(p1, 3, 1), "MREQ_n": p1 & 1,
        "sp": bits(p2, 35, 20), "ACC": bits(p2, 19, 12), "F": bits(p2, 11, 4),
        "IORQ_n": bits(p2, 3, 3), "RD_n": bits(p2, 2, 2), "WR_n": bits(p2, 1, 1), "halt": p2 & 1,
        "rom_a": bits(p3, 35, 14), "rom_stall": bits(p3, 13, 13), "stall_r": bits(p3, 12, 12), "rom_ok": bits(p3, 11, 11),
        "boot_n": bits(p3, 10, 10), "cart_sel": bits(p3, 9, 9), "ram_we": bits(p3, 8, 8), "IFF1": bits(p3, 7, 7),
        "IRQ_n": bits(p3, 6, 6), "IntCyc": bits(p3, 5, 5), "dpclk": bits(p3, 4, 0),
    }


# what the CPU did, independent of PSRAM timing
CPU_FIELDS = ["pc", "A", "MC", "TS", "M1_n", "MREQ_n", "IORQ_n", "RD_n", "WR_n", "IR", "DI", "D_out", "rom_do", "sp", "ACC",
              "F", "halt", "IFF1", "IRQ_n", "IntCyc", "boot_n", "cart_sel", "ram_we", "rom_a"]


def fmt_main(i, e):
    return ("%3d pc=%04x A=%04x M%d T%d m1=%d mreq=%d iorq=%d rd=%d wr=%d | rom_a=%06x rom_do=%02x D_out=%02x DI=%02x IR=%02x | "
            "sp=%04x A=%02x F=%02x halt=%d iff1=%d irq_n=%d int=%d | boot_n=%d cart=%d ramwe=%d | stall=%d stall_r=%d ok=%d d=%d" % (
                i, e["pc"], e["A"], e["MC"], e["TS"], e["M1_n"], e["MREQ_n"], e["IORQ_n"], e["RD_n"], e["WR_n"],
                e["rom_a"], e["rom_do"], e["D_out"], e["DI"], e["IR"], e["sp"], e["ACC"], e["F"], e["halt"], e["IFF1"],
                e["IRQ_n"], e["IntCyc"], e["boot_n"], e["cart_sel"], e["ram_we"], e["rom_stall"], e["stall_r"],
                e["rom_ok"], e["dpclk"]))


def fmt_pclk(i, parts, trig):
    a, b = parts.get(0), parts.get(1)
    if a is None or b is None:
        return "%3d (incomplete)" % i
    return ("%3d%s A=%04x rom_do=%02x DI=%02x IR=%02x M%d T%d ce=%d run=%d stall=%d stall_r=%d ok=%d q=%d | MCycles=%d TStates=%d "
            "ISet=%d RTR=%02x ALU=%x save=%d m1=%d" % (
                i, "*" if i == trig else " ", bits(a, 35, 20), bits(a, 19, 12), bits(a, 11, 4), bits(b, 35, 28), bits(a, 3, 1),
                bits(b, 27, 25), a & 1, bits(b, 24, 24), bits(b, 23, 23), bits(b, 22, 22), bits(b, 21, 21), bits(b, 20, 19),
                bits(b, 18, 16), bits(b, 15, 13), bits(b, 12, 11), bits(b, 10, 6), bits(b, 5, 2), bits(b, 1, 1), b & 1))


def summary_text(s):
    out = []
    if 0 in s:
        st = bits(s[0], 18, 0)
        out.append("replay crc16=%04x fills=%d status=%s" % (bits(s[0], 42, 27), bits(s[0], 26, 19),
                                                           ",".join(n for i, n in enumerate(STATUS) if st >> i & 1) or "-"))
    if 1 in s:
        out.append("replayed=%d main_entries=%d pclk_trigger_entry=%d" % (bits(s[1], 42, 21), bits(s[1], 20, 12), bits(s[1], 11, 3)))
    if 2 in s:
        regs = bits(s[2], 42, 27)
        out.append("vdp_regs=%04x (display_on=%d) cram0=%03x cram16=%03x" % (regs, regs >> 7 & 1, bits(s[2], 26, 15), bits(s[2], 14, 3)))
    if 3 in s:
        out.append("osd row 72: first=%04x last=%04x count=%d seen=%d" % (bits(s[3], 42, 27), bits(s[3], 26, 11), bits(s[3], 10, 3), bits(s[3], 2, 2)))
    if 4 in s:
        out.append("live pc=%04x sp=%04x" % (bits(s[4], 42, 27), bits(s[4], 26, 11)))
    return out


def main():
    args = sys.argv[1:]
    ref_path = None
    if "--ref" in args:
        i = args.index("--ref")
        ref_path = args[i + 1]
        del args[i:i + 2]
    trace = read(args[0])
    print("== %s: %d words, %d main entries, %d pclk entries" % (args[0], trace["words"], len(trace["main"]), len(trace["pclk"])))
    for line in summary_text(trace["summary"]):
        print("   " + line)
    entries = {i: main_entry(p) for i, p in trace["main"].items()}
    if ref_path:
        ref = read(ref_path)
        print("== %s (reference): %d words" % (ref_path, ref["words"]))
        for line in summary_text(ref["summary"]):
            print("   " + line)
        ref_entries = {i: main_entry(p) for i, p in ref["main"].items()}
        first = None
        for i in range(256):
            a, b = entries.get(i), ref_entries.get(i)
            if a is None or b is None:
                continue
            diff = [f for f in CPU_FIELDS if a[f] != b[f]]
            if diff:
                first = (i, diff)
                break
        if first is None:
            print("main trace: no difference in the CPU fields over the entries both have")
            return
        i, diff = first
        print("first difference at entry %d, fields: %s" % (i, ", ".join(diff)))
        for k in range(max(0, i - 6), min(256, i + 10)):
            if entries.get(k):
                print("unit " + fmt_main(k, entries[k]))
            if ref_entries.get(k):
                print("ref  " + fmt_main(k, ref_entries[k]))
        return
    for i in sorted(entries):
        if entries[i]:
            print(fmt_main(i, entries[i]))
    trig = bits(trace["summary"].get(1, 0), 11, 3)
    print("-- pclk trace (trigger entry %d marked *; oldest entry is trigger+257)" % trig)
    order = [(trig + 257 + k) % 512 for k in range(512)]
    for i in order:
        if i in trace["pclk"]:
            print(fmt_pclk(i, trace["pclk"][i], trig))


if __name__ == "__main__":
    main()
