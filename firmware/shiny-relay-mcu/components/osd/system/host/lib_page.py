#!/usr/bin/env python3
"""fr1: games_shots' screens as PNGs, without Pillow: the GAMES tab's panel (its picture, menu_games.c) and the library
over the whole screen (a title, 12 rows, the buttons), in the OSD's font (fingfai.c, LVGL 1 bpp), 160 x 144, saved 4x.

usage: games_shots | lib_page.py FONT.c GAMES_PICTURE.c OUT_DIR [PREFIX]
  a line: NAME|FULL|row0..row13 (the whole screen) or NAME|TABS|row0..row6 (the panel); a row: "!" white, "~" a label
  with a width (cut at its edge; "~." with dots: drawn cut here, as LVGL's dots are 3 pixels)
"""
import os
import re
import struct
import sys
import zlib

font_c, pic_c, out = sys.argv[1:4]
prefix = sys.argv[4] if len(sys.argv) > 4 else 'L'
src = open(font_c).read()
bm_txt = src[src.index("glyph_bitmap[] = {"):]
bm_txt = re.sub(r"/\*.*?\*/", "", bm_txt[:bm_txt.index("};")], flags=re.S)
bitmap = [int(v, 16) for v in re.findall(r"0x[0-9a-fA-F]+", bm_txt)]
dsc = [tuple(int(v) for v in d) for d in re.findall(
    r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}", src)]
LH = int(re.search(r"\.line_height = (\d+)", src).group(1))
BL = int(re.search(r"\.base_line = (\d+)", src).group(1))
WHITE, GREY, GREEN, GAME = (255, 255, 255), (150, 150, 150), (0x30, 0xE0, 0x70), (24, 56, 40)


def picture(path):
    s = open(path).read()
    w = int(re.search(r'\.header\.w = (\d+)', s).group(1)); h = int(re.search(r'\.header\.h = (\d+)', s).group(1))
    body = s[s.index('_map[] = {') + 10:]; body = body[:body.index('};')]
    data = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', body))
    pal = [(data[i * 4 + 2], data[i * 4 + 1], data[i * 4], data[i * 4 + 3]) for i in range(16)]
    stride = (w + 1) // 2
    return w, h, [[pal[(data[64 + y * stride + x // 2] >> (4 if x % 2 == 0 else 0)) & 15] for x in range(w)] for y in range(h)]


def width(t):
    return sum(dsc[ord(c) - 32 + 1][1] for c in t) >> 4


def text(px, x0, top, t, col, clip):
    pen = 0
    for ch in t:
        bi, adv, bw, bh, ox, oy = dsc[ord(ch) - 32 + 1]
        gx, gy = x0 + (pen >> 4) + ox, top + LH - BL - bh - oy
        bit = 0
        for yy in range(bh):
            for xx in range(bw):
                if (bitmap[bi + (bit >> 3)] >> (7 - (bit & 7))) & 1 and gx + xx < clip and 0 <= gy + yy < 144:
                    px[gy + yy][gx + xx] = col
                bit += 1
        pen += adv


def row(px, x, y, r, clip_w):
    col = WHITE if r.startswith('!') else GREY
    r = r.lstrip('!')
    cut = r.startswith('~')
    dots = r.startswith('~.')
    r = r[2:] if dots else r[1:] if cut else r
    clip = x + clip_w if cut else 160
    if dots and width(r) > clip_w:
        while r and width(r + '...') > clip_w:
            r = r[:-1]
        r += '...'
    text(px, x, y, r, col, clip)
    return width(r)


def png(path, px):
    raw = bytearray()
    for y in range(144 * 4):
        raw.append(0)
        for x in range(160 * 4):
            raw += bytes(px[y // 4][x // 4])
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 640, 576, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b''))


pw, ph, pic = picture(pic_c)
os.makedirs(out, exist_ok=True)
for line in sys.stdin:
    f = line.rstrip('\n').split('|')
    if not f[0].startswith(prefix):
        continue
    px = [[GAME] * 160 for _ in range(144)]            # (the game behind: a dark green)
    if f[1] == 'FULL':
        for y in range(144):
            px[y] = [(0, 0, 0)] * 160
        for x in range(8, 152):
            px[14][x] = GREEN; px[130][x] = GREEN
        rows = f[2:16]
        for i, r in enumerate(rows):
            y = 4 if i == 0 else 133 if i == 13 else 18 + 9 * (i - 1)
            row(px, 8, y, r, 144)
    else:
        for y in range(ph):
            for x in range(pw):
                c = pic[y][x]
                if c[3] and c[:3] != (0xF8, 0, 0xF8):
                    px[7 + y][8 + x] = c[:3]
        for i, r in enumerate(f[2:9]):
            row(px, 77, 46 + 9 * i, r, 72)
    png(os.path.join(out, f[0] + '.png'), px)
    print(f[0])
