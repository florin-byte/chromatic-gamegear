"""Host render of one SYSTEM page as the unit draws it (lane ui110dev): the SYSTEM frame, the tab list on the left and
the page's seven rows on the right, font fingfai (LVGL 1 bpp), 160 x 144, saved 4x.

usage: osd_page.py FONT.c OUT.png "TABS" "row0" ... "row6"
  TABS: the visible tab names, comma separated, the selected one starting with "*" ("GAMES,*GET  GAMES,DOWNLOAD")
  a row: "!" first = white (else grey); then "~" = a label with a width (LVGL LONG_DOT / LONG_SCROLL): cut at the panel's
  edge (x 149), "." after: with LVGL's three dots
Prints where each row ends (the panel edge is x 148) and OVER for a row that does not fit and is not cut.
"""
import re
import sys
from PIL import Image

font_c, out, tabs = sys.argv[1:4]
rows = sys.argv[4:11]
src = open(font_c).read()
bm_txt = src[src.index("glyph_bitmap[] = {"):]
bm_txt = re.sub(r"/\*.*?\*/", "", bm_txt[:bm_txt.index("};")], flags=re.S)
bitmap = [int(v, 16) for v in re.findall(r"0x[0-9a-fA-F]+", bm_txt)]
dsc = [tuple(int(v) for v in d) for d in re.findall(
    r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}", src)]
LH = int(re.search(r"\.line_height = (\d+)", src).group(1))
BL = int(re.search(r"\.base_line = (\d+)", src).group(1))

BG, PANEL, PINK, WHITE, GREY, DIV = (40, 40, 40), (0, 0, 0), (255, 51, 153), (255, 255, 255), (150, 150, 150), (102, 102, 102)
im = Image.new("RGB", (160, 144), BG)
px = im.load()
for y in range(22, 111):
    for x in range(12, 150):
        px[x, y] = PANEL
for x in range(12, 150):
    px[x, 22] = PINK; px[x, 110] = PINK
for y in range(22, 111):
    px[12, y] = PINK; px[149, y] = PINK
for y in range(27, 38):
    for x in range(16, 146):
        px[x, y] = PINK
for y in range(42, 106, 2):
    px[72, y] = DIV


def text(x0, top, t, col, clip=None):
    pen = 0
    for ch in t:
        bi, adv, bw, bh, ox, oy = dsc[ord(ch) - 32 + 1]
        gx, gy = x0 + (pen >> 4) + ox, top + LH - BL - bh - oy
        bit = 0
        for yy in range(bh):
            for xx in range(bw):
                if (bitmap[bi + (bit >> 3)] >> (7 - (bit & 7))) & 1 and (clip is None or gx + xx < clip) and gx + xx < 160:
                    px[gx + xx, gy + yy] = col
                bit += 1
        pen += adv
    return x0 + (pen >> 4)


def width(t):
    return sum(dsc[ord(c) - 32 + 1][1] for c in t) >> 4


# "SYSTEM" on the bar, 2x
big = Image.new("RGB", (80, 12), PINK)
bp = big.load()
pen = 0
for ch in "SYSTEM":
    bi, adv, bw, bh, ox, oy = dsc[ord(ch) - 32 + 1]
    bit = 0
    for yy in range(bh):
        for xx in range(bw):
            if (bitmap[bi + (bit >> 3)] >> (7 - (bit & 7))) & 1:
                bp[(pen >> 4) + ox + xx, 1 + LH - BL - bh - oy + yy - 2] = PANEL
            bit += 1
    pen += adv
big = big.resize((160, 24), Image.NEAREST).crop((0, 0, 130, 22))
im.paste(big.resize((65, 11), Image.NEAREST), (19, 27))

for i, name in enumerate(n for n in tabs.split(",") if n):
    sel = name.startswith("*")
    name = name.lstrip("*")
    x, y = 18, 44 + 13 * i
    if sel:
        for xx in range(x - 3, x - 3 + 55):
            px[xx, y - 2] = PINK; px[xx, y - 2 + 10] = PINK
        for yy in range(y - 2, y - 2 + 11):
            px[x - 3, yy] = PINK; px[x - 3 + 54, yy] = PINK
    text(x, y, name, WHITE if sel else GREY)

for r, t in enumerate(rows):
    col = WHITE if t.startswith("!") else GREY
    t = t[1:] if t.startswith("!") else t
    cut = t.startswith("~")
    t = t[1:] if cut else t
    dots = cut and t.startswith(".")
    t = t[1:] if dots else t
    top = 46 + 9 * r
    w = width(t)
    if cut and w > 72 and dots:
        while t and width(t + "...") > 72:
            t = t[:-1]
        t = t + "..."
    end = text(77, top, t, col, clip=149 if cut else None)
    if t:
        print("row %d ends at x=%d %s |%s|" % (r, end, "OK" if end <= 148 else ("cut" if cut else "OVER"), t))
im.resize((640, 576), Image.NEAREST).save(out)
