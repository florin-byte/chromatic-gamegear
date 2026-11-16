"""Width of OSD text in the fingfai font (the OSD's labels), against the panel.

usage: osd_fit.py FONT.c X0 "text" ...       prints each text's end x (the panel edge is 148)
       osd_fit.py FONT.c X0 --c FILE.c       every string literal in FILE.c (rough check)
"""
import re
import sys

font_c, x0 = sys.argv[1], int(sys.argv[2])
src = open(font_c).read()
dsc = [tuple(int(v) for v in d) for d in re.findall(
    r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}", src)]


def end_x(text):
    pen = 0
    for ch in text:
        o = ord(ch)
        if o < 32 or o > 126:
            raise SystemExit("not in the font: %r in %r" % (ch, text))
        pen += dsc[o - 32 + 1][1]
    return x0 + (pen >> 4)


texts = sys.argv[3:]
bad = 0
for t in texts:
    e = end_x(t)
    ok = e <= 148
    bad += not ok
    print("%3d %s |%s|" % (e, "OK  " if ok else "OVER", t))
sys.exit(1 if bad else 0)
