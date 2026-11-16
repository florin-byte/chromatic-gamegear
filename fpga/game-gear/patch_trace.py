#!/usr/bin/env python3
"""One-off edit of the CG-102 working copy: diagnostic build gg5diag (trace recorder, reviews CG-111 / CG-112).

- T80.vhd / T80s.vhd: a DBG output with the instruction register and the internal phase.
- system.vhd: a dbg_out port with the CPU bus, the data byte at three levels and the cartridge selection.
- emu_system_top.v: gg_trace.v instead of gg_probe.v; top.v / cart_xs.v: every read of the bench word (op 35)
  toggles a line that moves the recorder to its next word.
- The simulation dumps the same words.

Usage: patch_trace.py <working copy root>      (apply after patch_probes.py)
"""
import io
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])


def load(rel, enc="utf-8"):
    return io.open(root / rel, encoding=enc, newline="").read()


def save(rel, text, enc="utf-8"):
    io.open(root / rel, "w", encoding=enc, newline="").write(text)
    print("patched", rel)


def edit(rel, pairs, enc="utf-8"):
    text = load(rel, enc)
    crlf = "\r\n" in text
    if crlf:
        text = text.replace("\r\n", "\n")
    for old, new in pairs:
        if text.count(old) != 1:
            raise SystemExit("%s: expected exactly one match (found %d) for: %s" % (rel, text.count(old), old[:70]))
        text = text.replace(old, new)
    save(rel, text.replace("\n", "\r\n") if crlf else text, enc)


def sub(rel, pattern, repl, enc="latin-1"):
    text = load(rel, enc)
    crlf = "\r\n" in text
    if crlf:
        text = text.replace("\r\n", "\n")
    new, count = re.subn(pattern, repl, text, count=1, flags=re.S)
    if count != 1:
        raise SystemExit("%s: pattern not found: %s" % (rel, pattern[:70]))
    save(rel, new.replace("\n", "\r\n") if crlf else new, enc)


T80 = "esp32t/src/rtl/EMU/GG/sms_mister/T80/T80.vhd"
T80S = "esp32t/src/rtl/EMU/GG/sms_mister/T80/T80s.vhd"
SYS = "esp32t/src/rtl/EMU/GG/sms_mister/system.vhd"

# ---- T80: internal state out (diagnostic) ----
sub(T80, r"(ISet_out   : out std_logic_vector\(1 downto 0\))(\s*\);\s*end T80;)",
    r"\1;\n\t\tDBG        : out std_logic_vector(35 downto 0)  -- gg5diag\2")
sub(T80, r"(\tISet_out <= ISet;\n)",
    r"\1\t-- gg5diag: IntCycle, NMICycle, Halt_FF, IntE_FF1, Save_ALU_r, ALU_Op_r, Read_To_Reg_r, ISet, TStates, MCycles, TState, MCycle, IR\n"
    r"\tDBG <= IntCycle & NMICycle & Halt_FF & IntE_FF1 & Save_ALU_r & ALU_Op_r & Read_To_Reg_r & ISet & TStates & MCycles &\n"
    r"\t       std_logic_vector(TState) & MCycle & IR;\n")
sub(T80S, r"(ISet_out   : out std_logic_vector\(1 downto 0\))(\s*\);\s*end T80s;)",
    r"\1;\n\t\tDBG        : out std_logic_vector(35 downto 0)  -- gg5diag\2")
sub(T80S, r"(\t\t\tISet_out => ISet_out)", r"\1,\n\t\t\tDBG => DBG")

# ---- system: debug port ----
sub(SYS, r"(\t\tz80_iset     : out STD_LOGIC_VECTOR\(1 downto 0\);\n)",
    r"\1\t\t-- gg5diag: {4'd0, cart_memory_selected, bootloader_n, IRQ_n, M1_n, MREQ_n, IORQ_n, RD_n, WR_n,\n"
    r"\t\t--           D_out, GENIE_DI, A, T80 DBG}\n"
    r"\t\tdbg_out      : out STD_LOGIC_VECTOR(79 downto 0);\n")
sub(SYS, r"(\tsignal z80_iset_int:\t\tstd_logic_vector\(1 downto 0\);\n)",
    r"\1\tsignal t80_dbg:\t\t\tstd_logic_vector(35 downto 0);\n")
sub(SYS, r"(\t\tISet_out => z80_iset_int\n)", r"\t\tISet_out => z80_iset_int,\n\t\tDBG => t80_dbg\n")
sub(SYS, r"(\tz80_iset   <= z80_iset_int;\n)",
    r"\1\tdbg_out <= \"0000\" & cart_memory_selected & bootloader_n & IRQ_n & M1_n & MREQ_n & IORQ_n & RD_n & WR_n &\n"
    r"\t           D_out & GENIE_DI & A & t80_dbg;\n")

# ---- emu_system_top: recorder instead of the probes ----
text = load("esp32t/src/rtl/EMU/emu_system_top.v")
start = text.index("`ifdef VERILATOR\n    gg_probe")
end = text.index(".word(gg_dbg));", start) + len(".word(gg_dbg));")
text = text[:start] + (
    "    wire [79:0] core_dbg;\n"
    "    gg_trace trace (\n"
    "        .clk(pclk), .clear(hold_s[1]), .core_reset(core_reset), .loaded(loaded), .bad(bad), .idle(idle),\n"
    "        .stall_r(stall_r), .rom_ok(rom_ok), .alt(alt), .run(run), .rom_stall(rc_stall), .q_cnt(q_cnt),\n"
    "        .ce_cpu(ce_cpu), .pc(z80_reg[79:64]), .sp(z80_reg[63:48]), .acc(z80_reg[7:0]), .f(z80_reg[15:8]),\n"
    "        .halt(z80_reg[229]), .iff1(z80_reg[210]), .rom_a(rom_a[21:0]), .rom_do(idle ? 8'h00 : rom_do),\n"
    "        .core(core_dbg), .ram_we(ram_we && run), .dl_en(dl_en), .dl_a(dl_a), .dl_d(dl_d),\n"
    "        .vdp_regs(vdp_regs[15:0]), .cram0(vdp_cram[11:0]), .cram16(vdp_cram[203:192]),\n"
    "        .fill_err(rc_fill_dbg[8]), .fills(rc_fill_dbg[7:0]), .osd(osd_dbg), .rd_tog(dbg_rd_tog), .word(gg_dbg));"
) + text[end:]
save("esp32t/src/rtl/EMU/emu_system_top.v", text)
edit("esp32t/src/rtl/EMU/emu_system_top.v", [
    ("    input   [44:0]      osd_dbg,\n", "    input   [44:0]      osd_dbg,\n    input               dbg_rd_tog,\n"),
    ("        .z80_iset(),", "        .z80_iset(),\n        .dbg_out(core_dbg),"),
])

# ---- cart_xs / top: a toggle at every read of the bench word ----
edit("esp32t/src/rtl/BSP/cart_xs.v", [
    ("    input            [47:0]  r_dbg,\n",
     "    input            [47:0]  r_dbg,\n"
     "    output reg               r_dbg_tog = 1'b0,   // gg5diag: toggles at every RDBG (op 35)\n"),
    ("                    8'd35: begin crc_res <= r_dbg[31:0]; f <= {8'd0, r_dbg[47:32]}; done_seq <= qseq[q_head[QD_AW-1:0]]; end",
     "                    8'd35: begin crc_res <= r_dbg[31:0]; f <= {8'd0, r_dbg[47:32]}; done_seq <= qseq[q_head[QD_AW-1:0]]; r_dbg_tog <= ~r_dbg_tog; end"),
])
edit("esp32t/src/top.v", [
    ("    wire rc_fill_err;\n", "    wire rc_fill_err;\n    wire rc_dbg_tog;\n"),
    ("        .osd_dbg            (osd_dbg),", "        .osd_dbg            (osd_dbg),\n        .dbg_rd_tog         (rc_dbg_tog),"),
])
sub("esp32t/src/top.v", r"(        \.r_dbg          \(rc_dbg_g2\),)", r"\1\n        .r_dbg_tog      (rc_dbg_tog),", "utf-8")

edit("esp32t/evt1_x2.gprj", [
    ('        <File path="src/rtl/EMU/GG/gg_probe.v" type="file.verilog" enable="1"/>',
     '        <File path="src/rtl/EMU/GG/gg_probe.v" type="file.verilog" enable="0"/>\n'
     '        <File path="src/rtl/EMU/GG/gg_trace.v" type="file.verilog" enable="1"/>'),
])

# ---- simulation ----
edit("sim_gg/tb_gg.sv", [
    (" input sel, output [47:0] gg_dbg);", " input sel, input dbg_tog, output [47:0] gg_dbg);"),
    (" .osd_dbg(45'd0),", " .osd_dbg(45'd0), .dbg_rd_tog(dbg_tog),"),
])
text = load("sim_gg/main.cpp")
start = text.index("    // the probe word as `rc dbg` would show it")
end = text.index("    t.final(); return panel_ok ? 0 : 3;")
text = text[:start] + (
    "    // the trace recorder as `rc dbg` would read it: one word per read, 2056 words\n"
    "    { std::ofstream pr(out/\"trace.txt\");\n"
    "      for(int w=0;w<2056;w++) {\n"
    "          for(int i=0;i<16;i++) { t.pclk=0; t.hclk=((cycle+i)&1); t.eval(); t.hclk=!((cycle+i)&1); t.pclk=1; t.eval(); }\n"
    "          char line[96];\n"
    "          snprintf(line,sizeof(line),\"word a=%04llx fills=%04llx flags=%04llx\\n\",(unsigned long long)(t.gg_dbg&0xffff),\n"
    "              (unsigned long long)((t.gg_dbg>>16)&0xffff),(unsigned long long)((t.gg_dbg>>32)&0xffff));\n"
    "          pr<<line; t.dbg_tog=!t.dbg_tog; } }\n"
) + text[end:]
save("sim_gg/main.cpp", text)
text = load("sim_gg/run.sh")
if text.count('"$gg"/gg_probe.v') != 1:
    raise SystemExit("run.sh: gg_probe.v not found")
save("sim_gg/run.sh", text.replace('"$gg"/gg_probe.v', '"$gg"/gg_trace.v'))
