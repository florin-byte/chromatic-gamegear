#!/usr/bin/env python3
"""make_menu_colortemp.py: menu_colortemp.png + menu_colortemp.c, the COLOR TEMP page of a Game Boy game.

It is menu_palette_gbc (the page a Game Boy Color game shows: its header says PALETTE) with the header's word
replaced by COLOR TEMP in the same block letters, so that in Game Boy mode, where PALETTE has its own tab, the two
pages can be told apart. Only palette entries the picture already has are used (black, the header's purple), so the
indexed 4-bit data is the original's with the header's pixels changed.
usage: python3 make_menu_colortemp.py   (from this directory; needs Pillow for the .png)
"""
import re

SRC = "menu_palette_gbc.c"
W, H = 143, 106
BLACK, PURPLE = 0, 3          # palette indices of menu_palette_gbc (checked below)
HDR_Y0, HDR_Y1 = 22, 29        # the letters' rows
CLEAR_X0, CLEAR_X1 = 10, 100   # the old word PALETTE spans x 12..73; the dither starts near x 108
TEXT_X = 12
PITCH = 9

G = {
    "C": [".#######", "########", "###.....", "###.....", "###.....", "###.....", "########", ".#######"],
    "O": [".######.", "########", "###..###", "###..###", "###..###", "###..###", "########", ".######."],
    "L": ["###.....", "###.....", "###.....", "###.....", "###.....", "###.....", "########", "########"],
    "R": ["#######.", "########", "###..###", "########", "#######.", "###.###.", "###..###", "###..###"],
    "T": ["########", "########", "..###...", "..###...", "..###...", "..###...", "..###...", "..###..."],
    "E": ["########", "########", "###.....", "#####...", "#####...", "###.....", "########", "########"],
    "M": ["##....##", "###..###", "########", "########", "###..###", "###..###", "###..###", "###..###"],
    "P": ["#######.", "########", "###..###", "########", "#######.", "###.....", "###.....", "###....."],
}
TEXT = "COLOR TEMP"

src = open(SRC).read()
m = re.search(r"menu_palette_gbc_map\[\] = \{(.*?)\n\};", src, re.S)
data = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m.group(1)))
assert len(data) == 7696
pal, pix = data[:64], bytearray(data[64:])
assert pal[0:4] == bytes([0, 0, 0, 255]) and pal[12:16] == bytes([0xd8, 0x50, 0x90, 255]), "palette changed"
stride = (W + 1) // 2


def get(x, y):
    b = pix[y * stride + x // 2]
    return (b >> 4) if x % 2 == 0 else (b & 15)


def put(x, y, v):
    i = y * stride + x // 2
    pix[i] = (pix[i] & 0x0F) | (v << 4) if x % 2 == 0 else (pix[i] & 0xF0) | v


for y in range(HDR_Y0, HDR_Y1 + 1):
    for x in range(CLEAR_X0, CLEAR_X1):
        put(x, y, PURPLE)
for k, ch in enumerate(TEXT):
    if ch == " ":
        continue
    for dy, row in enumerate(G[ch]):
        for dx, c in enumerate(row):
            if c == "#":
                put(TEXT_X + k * PITCH + dx, HDR_Y0 + dy, BLACK)

# the picture as a .png (from the indexed data)
try:
    from PIL import Image
    im = Image.new("RGBA", (W, H))
    for y in range(H):
        for x in range(W):
            i = get(x, y) * 4
            b, g, r, a = pal[i:i + 4]
            im.putpixel((x, y), (r, g, b, a))
    im.save("menu_colortemp.png")
except ImportError:
    print("Pillow missing: menu_colortemp.png not written")

out = bytes(pal) + bytes(pix)
lines = []
for i in range(0, len(out), 24):
    lines.append("  " + ", ".join("0x%02x" % b for b in out[i:i + 24]) + ",")
head = src[:src.index("const LV_ATTRIBUTE_MEM_ALIGN")]
head = head.replace("menu_palette_gbc: 143 x 106", "menu_colortemp: 143 x 106")
head = re.sub(r"// Made by .*?menu_palette_gbc\.png\.\n", "// Made by make_menu_colortemp.py from menu_palette_gbc.c: the header's word PALETTE is COLOR TEMP (Game Boy mode, where\n// PALETTE has its own tab). The source picture is menu_colortemp.png.\n", head, flags=re.S)
head = head.replace("MENU_PALETTE_GBC", "MENU_COLORTEMP")
body = ("const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_MENU_COLORTEMP uint8_t menu_colortemp_map[] = {\n"
        + "\n".join(lines) + "\n};\n\n"
        "const lv_img_dsc_t menu_colortemp = {\n  .header.cf = LV_IMG_CF_INDEXED_4BIT,\n  .header.always_zero = 0,\n  .header.reserved = 0,\n"
        "  .header.w = 143,\n  .header.h = 106,\n  .data_size = 7696,\n  .data = menu_colortemp_map,\n};\n")
open("menu_colortemp.c", "w").write(head + body)
print("wrote menu_colortemp.c (%d bytes of data)" % len(out))
