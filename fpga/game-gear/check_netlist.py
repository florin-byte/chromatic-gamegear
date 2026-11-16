#!/usr/bin/env python3
"""Check a Gowin synthesis netlist (impl/gwsynthesis/evt1_x2.vg) of the Game Gear build for the defect of CG-113.

Gowin 1.9.12.04 connected VHDL entity inputs that have a default value to secondCounter[15] / [16] of top.v.
The check: the only cells that read a secondCounter bit are the counter's own adder (ALU), its compare logic
(nets n87_* / n111_*, which feed secondEna / halfSecondEna and the counter reset) and the increment inverter.
It also reports what drives the video RAM write enable and whether any module other than `top` has a
secondCounter port.

Usage: check_netlist.py <netlist.vg>      exit code 0 = clean, 1 = foreign use found
"""
import re
import sys

text = open(sys.argv[1], encoding="latin-1").read()
cells = re.findall(r"^  (\w+) (\S+) \(\n(.*?)^\);", text, re.S | re.M)
bad = []
readers = 0
for kind, name, body in cells:
    ins = re.findall(r"\.(I\d?|D|CE|RESET|SET|CLEAR|PRESET|CIN|WRE\w*|AD\w*|DI\w*)\(([^)]*secondCounter[^)]*)\)", body)
    if not ins:
        continue
    readers += 1
    if kind == "ALU" or re.match(r"(n87|n111|n135)_s\d+$", name) or name.startswith("secondCounter_"):
        continue
    bad.append("%s %s <- %s" % (kind, name, ", ".join(v for _, v in ins)))
ports = re.findall(r"^module (\w+) \((?:(?!^endmodule).)*?^input [^\n]*secondCounter", text, re.S | re.M)
ports = [m for m in ports if m != "top"]
wre = re.search(r"^module dpram \(.*?\.WREA\((\w+)\)", text, re.S | re.M)
print("cells reading secondCounter: %d, foreign: %d" % (readers, len(bad)))
for line in bad:
    print("FOREIGN:", line)
print("modules with a secondCounter input:", ", ".join(ports) or "none")
print("video RAM write enable:", wre.group(1) if wre else "dpram not found")
# The LCD init table (regs.bin, $readmemb in ST7785_init.v): a missing file is only a synthesis warning and leaves
# an empty table (build gg8: 39 LUT/ROM cells in the module instead of 139, the panel never left sleep).
lcd = re.search(r"^module ST7785_init \(.*?^endmodule", text, re.S | re.M)
lcd_cells = len(re.findall(r"^  (?:LUT\d|ROM\d+) ", lcd.group(0), re.M)) if lcd else 0
print("LCD init module LUT/ROM cells: %d (needs more than 100)" % lcd_cells)
ok = not bad and not ports and wre and wre.group(1) == "vram_cpu_WE" and lcd_cells > 100
print("netlist check:", "OK" if ok else "FAILED")
sys.exit(0 if ok else 1)
