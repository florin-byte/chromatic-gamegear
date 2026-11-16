"""Host render of the OSD's SYSTEM > WI-FI phone-setup screens on the real panel.

usage: osd_phone.py FONT.c BG.png OUT.png [--qr QRFILE X Y] "row0" "row1" ... (up to 7 rows)
A row starting with "!" is drawn white (a Line(.., true, ..)), others grey; a
leading "#" (before "!") also draws the padlock (a secured network in the list).
BG.png: a 4x OSD screenshot from the device (shots/ble1/phone2.png); the right-hand
content area (x 74..147, y 39..109) is cleared to the panel's black first.
Labels at x 77, y 46 + 9 * row (wifi_net.c kX_px / kY_px / kRow_px), font fingfai
(LVGL 1 bpp). QRFILE = qrhost output ('#' dark, '.' light) drawn at X, Y.
Prints where each row ends (the panel edge is x 148).
"""
import re
import sys
from PIL import Image

a = sys.argv[1:]
font_c, bg, out = a[:3]
a = a[3:]
qr = None
if a and a[0] == "--qr":
    qr = (a[1], int(a[2]), int(a[3]))
    a = a[4:]
src = open(font_c).read()
bm_txt = src[src.index("glyph_bitmap[] = {"):]
bm_txt = re.sub(r"/\*.*?\*/", "", bm_txt[:bm_txt.index("};")], flags=re.S)
bitmap = [int(v, 16) for v in re.findall(r"0x[0-9a-fA-F]+", bm_txt)]
dsc = [tuple(int(v) for v in d) for d in re.findall(
    r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}", src)]
LH = int(re.search(r"\.line_height = (\d+)", src).group(1))
BL = int(re.search(r"\.base_line = (\d+)", src).group(1))

im = Image.open(bg).convert("RGB").resize((160, 144), Image.NEAREST)
px = im.load()
panel = px[110, 100]
for y in range(39, 110):
    for x in range(74, 148):
        px[x, y] = panel
WHITE, GREY = (255, 255, 255), (150, 150, 150)

if qr:
    rows = open(qr[0]).read().split()
    qx, qy = qr[1], qr[2]
    for y, r in enumerate(rows):
        for x, c in enumerate(r):
            px[qx + x, qy + y] = (0, 0, 0) if c == "#" else WHITE
    print("QR %dx%d at x %d..%d, y %d..%d" % (len(rows[0]), len(rows), qx, qx + len(rows[0]) - 1, qy, qy + len(rows) - 1))
LOCK = [0x70, 0x88, 0x88, 0xF8, 0xD8, 0xF8]   # wifi_net.c kLockMap, 5 x 6
for row, text in enumerate(a):
    lock = text.startswith("#")
    text = text.lstrip("#")
    col = WHITE if text.startswith("!") else GREY
    text = text.lstrip("!")
    if lock:   # Lock(): x kLockX_px 90, y kY_px + row * kRow_px + 1
        for yy, bits in enumerate(LOCK):
            for xx in range(5):
                if bits & (0x80 >> xx):
                    px[90 + xx, 46 + row * 9 + 1 + yy] = col
    pen, top = 0, 46 + 9 * row
    lit_max = 0
    for ch in text:
        bi, adv, bw, bh, ox, oy = dsc[ord(ch) - 32 + 1]
        gx, gy = 77 + (pen >> 4) + ox, top + LH - BL - bh - oy
        bit = 0
        for yy in range(bh):
            for xx in range(bw):
                if (bitmap[bi + (bit >> 3)] >> (7 - (bit & 7))) & 1 and gx + xx < 160:
                    px[gx + xx, gy + yy] = col
                    lit_max = max(lit_max, gy + yy)
                bit += 1
        pen += adv
    end = 77 + (pen >> 4)
    if text:
        print("row %d ends at x=%d (panel edge 148) %s, lowest pixel y=%d |%s|" % (row, end, "OK" if end <= 148 else "OVER", lit_max, text))
im.resize((640, 576), Image.NEAREST).save(out)
