"""fw_fit.py FONT.c [IMG.c BG.png OUTDIR] < fw_shots output: SYSTEM INFO draws every character of every label.

Each label is drawn with the unit's font (fingfai, LVGL 1 bpp) at the place fw.c gave it. A lit pixel is LOST when it
is outside the box that cuts the label (LVGL does not draw a child outside its parent: the details box), outside the
panel's right-hand area (x 74..148, y 39..109), or on the picture of the Chromatic or right beside it (no pixel
between them). Any lost pixel fails the page.
With IMG.c BG.png OUTDIR each page is also saved as a PNG, cut the way the unit cuts it.
"""
import re
import sys

font_c = sys.argv[1]
shots = sys.argv[2:5] if len(sys.argv) >= 5 else None
src = open(font_c).read()
bm_txt = src[src.index("glyph_bitmap[] = {"):]
bm_txt = re.sub(r"/\*.*?\*/", "", bm_txt[:bm_txt.index("};")], flags=re.S)
bitmap = [int(v, 16) for v in re.findall(r"0x[0-9a-fA-F]+", bm_txt)]
dsc = [tuple(int(v) for v in d) for d in re.findall(
    r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}", src)]
LH = int(re.search(r"\.line_height = (\d+)", src).group(1))
BL = int(re.search(r"\.base_line = (\d+)", src).group(1))
PANEL = (74, 39, 149, 110)   # x0, y0, x1, y1 (exclusive): the right-hand area of the SYSTEM frame


def lit(x0, top, t):
    """every lit pixel of text t drawn at x0, top: (x, y, index of the character)"""
    pen = 0
    for n, ch in enumerate(t):
        bi, adv, bw, bh, ox, oy = dsc[ord(ch) - 32 + 1]
        gx, gy = x0 + (pen >> 4) + ox, top + LH - BL - bh - oy
        bit = 0
        for yy in range(bh):
            for xx in range(bw):
                if (bitmap[bi + (bit >> 3)] >> (7 - (bit & 7))) & 1:
                    yield gx + xx, gy + yy, n
                bit += 1
        pen += adv


pages, cur = [], None
for line in sys.stdin:
    f = line.rstrip("\n").split("|")
    if f[0] == "page":
        cur = {"name": f[1], "ver": f[2], "labels": [], "img": None}
        pages.append(cur)
    elif f[0] == "img":
        cur["img"] = tuple(int(v) for v in f[1:5])
    elif f[0] == "label":
        cur["labels"].append((int(f[1]), int(f[2]), tuple(int(v) for v in f[3:7]), f[7] == "1", "|".join(f[8:])))

bad = n = 0
for p in pages:
    ix, iy, iw, ih = p["img"] or (0, 0, 0, 0)
    for x, y, c, white, t in p["labels"]:
        n += 1
        lost = set()
        why = set()
        for px, py, i in lit(x, y, t):
            if not (c[0] <= px < c[2] and c[1] <= py < c[3]):
                lost.add(i); why.add("cut by its box at x %d" % c[2] if px >= c[2] else "cut by its box")
            elif not (PANEL[0] <= px < PANEL[2] and PANEL[1] <= py < PANEL[3]):
                lost.add(i); why.add("outside the panel")
            elif ix <= px < ix + iw and iy <= py < iy + ih:
                lost.add(i); why.add("on the picture")
            elif ix - 1 <= px < ix + iw + 1 and iy - 1 <= py < iy + ih + 1:
                lost.add(i); why.add("touches the picture")
        if lost:
            bad += 1
            shown = "".join(ch for i, ch in enumerate(t) if i not in lost)
            print("LOST %s: |%s| draws as |%s| (%d of %d characters hit: %s)" % (p["name"], t, shown, len(lost), len(t), ", ".join(sorted(why))))
print("fw_fit: %d pages, %d labels, %d not drawn whole" % (len(pages), n, bad))

if shots:
    from PIL import Image
    img_c, bg, outdir = shots
    isrc = open(img_c).read()
    ib = [int(v, 16) for v in re.findall(r"0x[0-9a-fA-F]{2}", isrc[isrc.index("img_chromatic_map[] = {"):isrc.index("};")])]
    pal = [(ib[4 * i + 2], ib[4 * i + 1], ib[4 * i], ib[4 * i + 3]) for i in range(16)]   # B G R A
    for p in pages:
        im = Image.open(bg).convert("RGB").resize((160, 144), Image.NEAREST)
        px = im.load()
        panel = px[110, 100]
        for y in range(39, 110):
            for x in range(74, 148):
                px[x, y] = panel
        for x, y, c, white, t in p["labels"]:
            for qx, qy, _ in lit(x, y, t):
                if c[0] <= qx < c[2] and c[1] <= qy < c[3] and 0 <= qx < 160 and 0 <= qy < 144:
                    px[qx, qy] = (255, 255, 255) if white else (150, 150, 150)
        if p["img"]:
            ix, iy, iw, ih = p["img"]
            for yy in range(ih):
                for xx in range(iw):
                    v = ib[64 + yy * (iw // 2) + xx // 2]
                    r, g, b, a = pal[(v >> 4) if xx % 2 == 0 else (v & 15)]
                    if a:
                        px[ix + xx, iy + yy] = (r, g, b)
        out = "%s/%s.png" % (outdir, p["name"])
        im.resize((640, 576), Image.NEAREST).save(out)
        print(out)
sys.exit(1 if bad else 0)
