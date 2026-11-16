#!/usr/bin/env python3
"""Make the System E hardware of the vendored SMS_MiSTer core optional (Game Gear on the Chromatic).

The GW5A-25 has 56 block RAMs; the unmodified core needs far more (second VDP with its VRAM, 32 KiB of
VRAM per VDP for the System E bank, the boot ROM). This adds a generic `SYSTEM_E` (default 1 = unchanged
behavior) to `system` and `SE_VRAM` to `vdp`. With SYSTEM_E = 0: no second VDP, no second PSG, no boot
ROM, 16 KiB of VRAM.

Usage: slim_gg_core.py <dir with system.vhd and vdp.vhd>     (edits the two files in place, once)
"""
import re
import sys
from pathlib import Path


def sub_once(text, pattern, repl, flags=re.S):
    new, count = re.subn(pattern, repl, text, count=1, flags=flags)
    if count != 1:
        raise SystemExit("pattern not found: " + pattern[:60])
    return new


def patch_system(text):
    if "SYSTEM_E" in text:
        raise SystemExit("system.vhd is already patched")
    text = sub_once(text, r"(entity system is\s+generic \(\s*)", r"\1SYSTEM_E : integer := 1;\n\t\t")
    # Second VDP
    text = sub_once(text, r"\tvdp2_inst: entity work\.vdp\n(.*?\n\t\);\n)",
                    lambda m: "\tsysteme_vdp : if SYSTEM_E /= 0 generate\n\tvdp2_inst: entity work.vdp\n" + m.group(1) +
                    "\tend generate;\n"
                    "\tno_systeme_vdp : if SYSTEM_E = 0 generate\n"
                    "\t\tvdp2_IRQ_n <= '1';\n\t\tvdp2_D_out <= (others => '1');\n\t\tvdp2_color <= (others => '0');\n"
                    "\t\tvdp2_y1 <= '0';\n\t\tvdp2_regs_out_i <= (others => '0');\n\t\tvdp2_cram_out_i <= (others => '0');\n"
                    "\t\tvdp2_vram_D_i <= (others => '0');\n\tend generate;\n")
    # First VDP: 16 KiB of VRAM without System E
    text = sub_once(text, r"(\tvdp_inst: entity work\.vdp\n\tgeneric map\(\n)", r"\1\t\tSE_VRAM => SYSTEM_E,\n")
    # Second PSG
    text = sub_once(text, r"\tpsg2_inst: jt89\n(.*?\n\t\);\n)",
                    lambda m: "\tsysteme_psg : if SYSTEM_E /= 0 generate\n\tpsg2_inst: jt89\n" + m.group(1) +
                    "\tend generate;\n"
                    "\tno_systeme_psg : if SYSTEM_E = 0 generate\n"
                    "\t\tPSG2_outL <= (others => '0');\n\t\tPSG2_outR <= (others => '0');\n"
                    "\t\tpsg2_out_i <= (others => '0');\n\t\tpsg2_ext_out <= (others => '0');\n"
                    "\t\tpsg_div2 <= (others => '0');\n\t\tpsg_quiet2 <= '1';\n\tend generate;\n")
    # Boot ROM (the built-in loader of the MiSTer core; not used with bios_en = 0)
    text = sub_once(text, r"\tboot_rom_inst : entity work\.sprom\n(.*?\n\t\);\n)",
                    lambda m: "\tboot_rom_on : if SYSTEM_E /= 0 generate\n\tboot_rom_inst : entity work.sprom\n" + m.group(1) +
                    "\tend generate;\n"
                    "\tboot_rom_off : if SYSTEM_E = 0 generate\n\t\tboot_rom_D_out <= (others => '1');\n\tend generate;\n")
    return text


def patch_vdp(text):
    if "SE_VRAM" in text:
        raise SystemExit("vdp.vhd is already patched")
    text = sub_once(text, r"(entity vdp is\s+generic \(\s*)", r"\1SE_VRAM : integer := 1;\n\t\t")
    match = re.search(r"\tvdp_vram_inst : entity work\.dpram\n.*?\n    \);\n", text, flags=re.S)
    if not match:
        raise SystemExit("vdp_vram_inst not found")
    full = match.group(0)
    small = full.replace("vdp_vram_inst", "vdp_vram16_inst")
    for old, new in (("widthad_a\t\t=> 15", "widthad_a\t\t=> 14"),
                     ("=> vram_portA_address,", "=> vram_portA_address(13 downto 0),"),
                     ("=> se_bank & vram_vdp_A,", "=> vram_vdp_A,")):
        if old not in small:
            raise SystemExit("vdp_vram_inst: text not found: " + old)
        small = small.replace(old, new)
    block = ("\tvram_se : if SE_VRAM /= 0 generate\n" + full + "\tend generate;\n"
             "\tvram_16k : if SE_VRAM = 0 generate\n" + small + "\tend generate;\n")
    return text.replace(full, block)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    root = Path(sys.argv[1])
    for name, patch in (("system.vhd", patch_system), ("vdp.vhd", patch_vdp)):
        path = root / name
        data = path.read_bytes().decode("latin-1")
        crlf = "\r\n" in data
        text = patch(data.replace("\r\n", "\n"))
        path.write_bytes((text.replace("\n", "\r\n") if crlf else text).encode("latin-1"))
        print("patched", path)


if __name__ == "__main__":
    main()
