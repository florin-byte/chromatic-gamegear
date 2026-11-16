"""Every page word of SYSTEM > GAMES / GET GAMES fits the panel (lane ui110dev): the upper-case string literals of the files
given (the page itself, and the job / install code's "ROW1|ROW2" results), each row measured in the font (77 + width <= 148).
A literal with %s / %d is measured with its value left out; those rows are checked on the renders (games-shots).
fr1: a line marked "(fr1: whole screen)" is a row of the GAMES library, drawn over the whole screen (8 + width <= 152).
usage: check_words.py FONT.c FILE ...  (exit 1 on a row that does not fit, or a word the device never shows)"""
import re
import sys

src = open(sys.argv[1]).read()
D = [tuple(int(v) for v in d) for d in re.findall(
    r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}", src)]
def w(t): return sum(D[ord(c) - 32 + 1][1] for c in t if 32 <= ord(c) < 127) >> 4
NEVER = re.compile(r"\b(FPGA|FLASH|LIBRARY|COMPRESS\w*|PSRAM|STAGING|SGZ\w*|KEPT  GAMES|RAM  CART|1\.0\.\d|1\.1\.\d)\b")
bad = 0
for f in sys.argv[2:]:
    for n, line in enumerate(open(f), 1):
        if line.lstrip().startswith(("//", "/*", "*", "printf", "logf_")) or ("printf(" in line and "snprintf" not in line) or "scrolls)" in line or "ota_meter" in line:
            continue
        if not re.search(r'snprintf|Line|R\[kRows\]|result\(|Result\(|why|Rows|\{ "|Job\(', line):
            continue
        for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', line):
            if not re.search(r"[A-Z]{2}.*  ", lit) and not re.fullmatch(r"[A-Z0-9 :.,!?'+%-]+\|?[A-Z0-9 :.,!?'+%-]*", lit or "x"):
                continue
            if re.search(r"[a-z]", lit):
                continue
            for row in lit.split("|"):
                t = re.sub(r"%[-0-9.]*[a-z]", "", row)
                if NEVER.search(t):
                    print("%s:%d: a word the device never shows: |%s|" % (f, n, row)); bad += 1
                x0, edge = (8, 152) if "(fr1: whole screen)" in line else (77, 148)
                if x0 + w(t) > edge:
                    print("%s:%d: %d px, over the %s by %d: |%s|" % (f, n, w(t), "screen" if x0 == 8 else "panel", x0 + w(t) - edge, row)); bad += 1
print("check_words: %d rows over or words not allowed" % bad)
sys.exit(1 if bad else 0)
