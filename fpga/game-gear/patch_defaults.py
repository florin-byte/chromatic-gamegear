#!/usr/bin/env python3
"""Experiment CG-113, variant C: remove the default value of every VHDL input port that all instantiations connect.

Gowin 1.9.12.04 built restore logic driven by an unrelated counter for entity inputs that have a default value and
are connected (seen on `video`: removing the defaults removed that logic). Ports that an instantiation leaves out
keep their default.

Usage: patch_defaults.py <working copy root> [--dry]
"""
import io
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
dry = "--dry" in sys.argv
base = root / "esp32t/src/rtl/EMU/GG"
# Only the files the Gowin project compiles: an entity can exist in a second, unused file (xilinx_ram.vhd).
gprj = io.open(root / "esp32t/evt1_x2.gprj", encoding="utf-8").read()
files = [root / "esp32t" / m for m in re.findall(r'<File path="(src/rtl/EMU/GG/[^"]+\.vhd)" type="file.vhdl" enable="1"/>', gprj)]

# The CPU inputs system.vhd left to the entity defaults get explicit constants.
sysv = base / "sms_mister/system.vhd"
text = io.open(sysv, encoding="latin-1", newline="").read()
if "WAIT_n\t=> '1'" not in text:
    old = "CEN\t\t=> ce_z80,"
    if text.count(old) != 1:
        raise SystemExit("system.vhd: CEN association not found")
    nl = "\r\n" if "\r\n" in text else "\n"
    text = text.replace(old, old + nl + "\t\tWAIT_n\t=> '1',   -- CG-113: explicit, no entity default" + nl +
                        "\t\tBUSRQ_n\t=> '1'," + nl + "\t\tOUT0\t\t=> '0',")
    if not dry:
        io.open(sysv, "w", encoding="latin-1", newline="").write(text)
    print("system.vhd: WAIT_n, BUSRQ_n, OUT0 connected explicitly")

PORT = re.compile(r"^(\s*)([\w\s,]+?)\s*:\s*(in|out|inout|buffer)\s+(.*?)\s*(?::=\s*(.*?))?\s*(;?)\s*(--.*)?$", re.I)


def load(p):
    return io.open(p, encoding="latin-1", newline="").read()


def nocomment(text):
    return re.sub(r"--[^\n]*", "", text)


def port_section(text, m):
    s = re.compile(r"\bport\s*\(", re.I).search(text, m.end())
    e = re.compile(r"\)\s*;\s*\n\s*end\b", re.I).search(text, s.end())
    return s.end(), e.start()


def close(text, i):
    depth = 1
    while depth:
        depth += {"(": 1, ")": -1}.get(text[i], 0)
        i += 1
    return i


def formals(body):
    out, depth, cur = [], 0, ""
    for ch in body + ",":
        if ch == "," and depth == 0:
            if "=>" in cur:
                out.append(re.match(r"\s*(\w+)", cur).group(1).lower())
            cur = ""
            continue
        depth += {"(": 1, ")": -1}.get(ch, 0)
        cur += ch
    return set(out)


texts = {p: load(p) for p in files}
entities = {}   # name -> (path, [(port, default)])
for p, text in texts.items():
    t = text.replace("\r\n", "\n")
    for m in re.finditer(r"(?im)^\s*entity\s+(\w+)\s+is", t):
        try:
            s, e = port_section(t, m)
        except AttributeError:
            continue
        ports = []
        for line in t[s:e].split("\n"):
            pm = PORT.match(line)
            if pm and pm.group(3).lower() == "in" and pm.group(5) is not None:
                ports += [n.strip().lower() for n in pm.group(2).split(",")]
        entities[m.group(1).lower()] = (p, ports)

connected = {}  # entity -> list of formal sets, one per instantiation
for p, text in texts.items():
    t = nocomment(text.replace("\r\n", "\n"))
    for m in re.finditer(r"(?i):\s*entity\s+work\.(\w+)", t):
        pm = re.compile(r"\bport\s+map\s*\(", re.I).search(t, m.end())
        end = close(t, pm.end())
        connected.setdefault(m.group(1).lower(), []).append(formals(t[pm.end():end - 1]))

total = 0
for name, (p, ports) in sorted(entities.items()):
    insts = connected.get(name)
    if not ports or not insts:
        continue
    strip = [q for q in ports if all(q in f for f in insts)]
    keep = [q for q in ports if q not in strip]
    print("%-22s %d instantiation(s): strip %d, keep %d %s" % (name, len(insts), len(strip), len(keep), keep if keep else ""))
    total += len(strip)
    if dry or not strip:
        continue
    text = texts[p]
    crlf = "\r\n" in text
    t = text.replace("\r\n", "\n")
    m = re.search(r"(?im)^\s*entity\s+%s\s+is" % name, t)
    s, e = port_section(t, m)
    lines = t[s:e].split("\n")
    last = max(i for i, line in enumerate(lines) if PORT.match(line))
    out = []
    for i, line in enumerate(lines):
        pm = PORT.match(line)
        if pm and pm.group(3).lower() == "in" and pm.group(5) is not None:
            semi = "" if i == last else ";"
            names = [n.strip() for n in pm.group(2).split(",")]
            parts = []
            for k, n in enumerate(names):
                default = "" if n.lower() in strip else " := " + pm.group(5)
                parts.append("%s%s : in %s%s%s" % (pm.group(1), n, pm.group(4), default, ";" if k < len(names) - 1 else semi))
            line = "\n".join(parts)
        out.append(line)
    t = t[:s] + "\n".join(out) + t[e:]
    texts[p] = t.replace("\n", "\r\n") if crlf else t
    io.open(p, "w", encoding="latin-1", newline="").write(texts[p])
print("total defaults %s: %d" % ("to strip" if dry else "stripped", total))
# What is left: only ports of entities nothing instantiates from VHDL, or component declarations.
left = 0
for p in files:
    for n, line in enumerate(io.open(p, encoding="latin-1").read().split("\n"), 1):
        if re.search(r":\s*in\s+std_logic.*:=", line, re.I):
            left += 1
            print("left: %s:%d: %s" % (p.name, n, line.strip()))
print("input ports with a default left in the compiled files: %d" % left)
