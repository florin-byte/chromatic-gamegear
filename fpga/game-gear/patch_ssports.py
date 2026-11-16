#!/usr/bin/env python3
"""Experiment CG-113: the Gowin netlist of gg5diag drives the core's save-state inputs from an unrelated counter.

The two VHDL entities instantiated from Verilog (`system`, `video`) have input ports with default values.
  list   print every input port, its default and what emu_system_top.v connects to it
  a      remove the default values from the input ports of both entities (the Verilog side connects all of them)
  b      like a, and the save-state inputs of `system` are no longer ports: constants inside the architecture

Usage: patch_ssports.py <working copy root> list|a|b
"""
import io
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
mode = sys.argv[2]
GG = "esp32t/src/rtl/EMU/GG/sms_mister/"
EMU = "esp32t/src/rtl/EMU/emu_system_top.v"

SS = """z80_dir z80_set vdp_regs_in vdp_regs_set ss_cram_wr ss_cram_A ss_cram_D ss_vram_en ss_vram_A ss_vram_WE
ss_vram_WA ss_vram_WD psg_in psg_set psg_ext_in psg2_ext_in psg_div_in audio_control_in audio_phase_in
audio_phase_set audio_ext_set mapper_in mapper_set evolution_ss_in evolution_ss_set eeprom_ss_in eeprom_ss_set
vdp2_regs_in vdp2_regs_set ss_cram2_wr ss_cram2_A ss_cram2_D ss_vram2_en ss_vram2_A ss_vram2_WE ss_vram2_WA
ss_vram2_WD psg2_in psg2_set io_state_in io_state_set se_pause ss_freeze""".split()


def load(rel):
    return io.open(root / rel, encoding="latin-1", newline="").read()


def save(rel, text):
    io.open(root / rel, "w", encoding="latin-1", newline="").write(text)
    print("patched", rel)


def port_section(text, entity):
    m = re.search(r"entity\s+%s\s+is" % entity, text, re.I)
    start = re.compile(r"\bport\s*\(", re.I).search(text, m.end()).end()
    end = re.compile(r"\)\s*;\s*\n\s*end\b", re.I).search(text, start).start()
    return start, end


PORT = re.compile(r"^(\s*)([\w\s,]+?)\s*:\s*(in|out|inout)\s+(.*?)\s*(?::=\s*(.*?))?\s*(;?)\s*(--.*)?$", re.I)


def ports(section):
    out = []
    for line in section.split("\n"):
        m = PORT.match(line.rstrip("\r"))
        if m:
            out.append((line, [n.strip() for n in m.group(2).split(",")], m.group(3).lower(), m.group(4), m.group(5)))
    return out


def instance(text, name):
    # the last match: the synthesis branch of the `ifdef VERILATOR` pair
    m = list(re.finditer(r"(?m)^[ \t]*%s\s+(?:#\s*\([^;\n]*\)\s*)?\w+\s*\(" % name, text))[-1]
    depth, i = 1, m.end()
    while depth:
        if text.startswith("//", i):      # parentheses in comments do not count
            i = text.index("\n", i)
            continue
        depth += {"(": 1, ")": -1}.get(text[i], 0)
        i += 1
    body = re.sub(r"//[^\n]*", "", text[m.end():i])
    return m.start(), i, dict(re.findall(r"\.(\w+)\s*\(([^()]*(?:\([^()]*\)[^()]*)*)\)", body))


def strip_defaults(rel, entity, drop=()):
    text = load(rel)
    crlf = "\r\n" in text
    text = text.replace("\r\n", "\n")
    start, end = port_section(text, entity)
    lines = []
    for line in text[start:end].split("\n"):
        m = PORT.match(line)
        if m and m.group(3).lower() == "in":
            names = [n.strip() for n in m.group(2).split(",") if n.strip() not in drop]
            if not names:
                continue
            line = "%s%s : in %s;" % (m.group(1), ", ".join(names), m.group(4))
        lines.append(line)
    # the last port has no semicolon
    for i in range(len(lines) - 1, -1, -1):
        if PORT.match(lines[i]):
            lines[i] = re.sub(r";(\s*(--.*)?)$", r"\1", lines[i])
            break
    text = text[:start] + "\n".join(lines) + text[end:]
    save(rel, text.replace("\n", "\r\n") if crlf else text)


if mode == "list":
    emu = load(EMU)
    for rel, entity, inst in ((GG + "system.vhd", "system", "system"), (GG + "video.vhd", "video", "video")):
        text = load(rel).replace("\r\n", "\n")
        start, end = port_section(text, entity)
        _, _, conn = instance(emu, inst)
        print("==", entity)
        for _, names, direction, typ, default in ports(text[start:end]):
            for n in names:
                if direction == "in":
                    actual = next((v for k, v in conn.items() if k.lower() == n.lower()), None)
                    print("%-26s default=%-18s actual=%s" % (n, default, "MISSING" if actual is None else actual.strip()))
    sys.exit(0)

strip_defaults(GG + "video.vhd", "video")
if mode == "a":
    strip_defaults(GG + "system.vhd", "system")
else:
    strip_defaults(GG + "system.vhd", "system", set(SS))
    text = load(GG + "system.vhd")
    crlf = "\r\n" in text
    text = text.replace("\r\n", "\n")
    src = load(GG + "system.vhd").replace("\r\n", "\n")
    consts = []
    m = re.search(r"architecture\s+\w+\s+of\s+system\s+is\n", text, re.I)
    # types of the removed ports, taken from the file as it was before this run is not possible any more:
    # they are listed here
    widths = {"z80_dir": 230, "vdp_regs_in": 128, "ss_cram_A": 5, "ss_cram_D": 12, "ss_vram_A": 15, "ss_vram_WA": 15,
              "ss_vram_WD": 8, "psg_in": 56, "psg_ext_in": 96, "psg2_ext_in": 96, "psg_div_in": 4,
              "audio_control_in": 11, "mapper_in": 64, "evolution_ss_in": 96, "eeprom_ss_in": 64,
              "vdp2_regs_in": 128, "ss_cram2_A": 5, "ss_cram2_D": 12, "ss_vram2_A": 15, "ss_vram2_WA": 15,
              "ss_vram2_WD": 8, "psg2_in": 56, "io_state_in": 32}
    for n in SS:
        if n in widths:
            consts.append("\tconstant %s : std_logic_vector(%d downto 0) := (others => '0');  -- CG-113: no save states"
                          % (n, widths[n] - 1))
        else:
            consts.append("\tconstant %s : std_logic := '0';  -- CG-113: no save states" % n)
    text = text[:m.end()] + "\n".join(consts) + "\n" + text[m.end():]
    save(GG + "system.vhd", text.replace("\n", "\r\n") if crlf else text)
    emu = load(EMU)
    crlf = "\r\n" in emu
    emu = emu.replace("\r\n", "\n")
    s, e, _ = instance(emu, "system")
    body = emu[s:e]
    for n in SS:
        body, count = re.subn(r"\n[ \t]*\.%s\s*\([^\n]*\),?[ \t]*(?=\n)" % n, "", body, flags=re.I)
        if count != 1:
            raise SystemExit("emu_system_top.v: %s connected %d times" % (n, count))
    body = re.sub(r",(\s*\)\s*;?\s*)$", r"\1", body)
    emu = emu[:s] + body + emu[e:]
    save(EMU, emu.replace("\n", "\r\n") if crlf else emu)
