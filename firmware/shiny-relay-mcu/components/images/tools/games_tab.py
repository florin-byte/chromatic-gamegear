#!/usr/bin/env python3
"""fr1: the GAMES tab's pictures (no Pillow needed).

Every menu picture gets a 6th tab on its strip (x 89..103: a cartridge, not selected), and menu_games.c is made from
menu_system.c: the SYSTEM tab drawn not selected, the GAMES tab selected, its title GAMES and its own accent colour.
The pictures are LV_IMG_CF_INDEXED_4BIT C arrays (a 16-entry B G R A palette, then 4 bits a pixel); they are read and
written in RGBA, each palette made again from the colours a picture uses (its old order kept, new colours after).

usage: games_tab.py IMAGES_DIR [PREVIEW_DIR]   (run once on the 1.1.5 pictures: it checks the slot is still empty)
"""
import os
import re
import struct
import sys
import zlib

ACCENT = (0x30, 0xE0, 0x70, 0xFF)          # the GAMES tab's colour (R G B A)
SLOT_X = 99                                # the new tab's block: x 99..115 (the tabs are 18 apart: 9, 27, 45, 63, 81)
SYS_X = 81                                 # the SYSTEM tab's block
DISP_X = 27                                # DISPLAY's block (not selected on the STATUS picture)
BLOCK_W = 17
NAMES = ['menu_status', 'menu_display', 'menu_controls', 'menu_palette', 'menu_palette_gbc', 'menu_system']

# a cartridge, 9 x 9 ('#' the icon's colour)
ICON = [
    '.#######.',
    '.#.....#.',
    '.#.###.#.',
    '.#.###.#.',
    '.#.....#.',
    '.#.....#.',
    '.#.#.#.#.',
    '.#######.',
    '.........',
]
# G in the title font (8 x 8); A, M, E, S are taken from the DISPLAY and SYSTEM titles
G = [
    '.#######',
    '########',
    '###.....',
    '###.####',
    '###.####',
    '###..###',
    '########',
    '.#######',
]


def load(path):
    """-> src, w, h, palette (R G B A), pixels; the format (4 or 8 bits a pixel) kept in the palette's length."""
    s = open(path, encoding='utf-8').read()
    w = int(re.search(r'\.header\.w = (\d+)', s).group(1))
    h = int(re.search(r'\.header\.h = (\d+)', s).group(1))
    bits = 8 if 'LV_IMG_CF_INDEXED_8BIT' in s else 4          # (fr1 fix: menu_controls is 8 bit, 256 colours)
    n = 1 << bits
    body = s[s.index('_map[] = {') + 10:]
    body = body[:body.index('};')]
    data = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', body))
    pal = [(data[i * 4 + 2], data[i * 4 + 1], data[i * 4], data[i * 4 + 3]) for i in range(n)]   # -> R G B A
    px = data[n * 4:]
    if bits == 8:
        img = [[pal[px[y * w + x]] for x in range(w)] for y in range(h)]
    else:
        stride = (w + 1) // 2
        img = [[pal[(px[y * stride + x // 2] >> (4 if x % 2 == 0 else 0)) & 15] for x in range(w)] for y in range(h)]
    return s, w, h, pal, img


def save(path, src, w, h, oldpal, img):
    n = len(oldpal)                                    # (the picture's own format: 16 or 256 colours)
    used = []
    for row in img:
        for c in row:
            if c not in used:
                used.append(c)
    pal = [c for c in oldpal if c in used]
    pal += [c for c in used if c not in pal]
    if len(pal) > n:
        raise SystemExit('%s: %d colours' % (path, len(pal)))
    pal += [(0, 0, 0, 0)] * (n - len(pal))
    idx = {c: i for i, c in reversed(list(enumerate(pal)))}
    out = bytearray()
    for r, g, b, a in pal:
        out += bytes((b, g, r, a))
    for y in range(h):
        if n == 256:
            out += bytes(idx[img[y][x]] for x in range(w))
            continue
        row = bytearray((w + 1) // 2)
        for x in range(w):
            row[x // 2] |= idx[img[y][x]] << (4 if x % 2 == 0 else 0)
        out += row
    lines = ['  ' + ', '.join('0x%02x' % v for v in out[i:i + 24]) + ',' for i in range(0, len(out), 24)]
    a = src.index('_map[] = {') + 10
    b = src.index('};', a)
    new = src[:a] + '\n' + '\n'.join(lines) + '\n' + src[b:]
    new = re.sub(r'\.data_size = \d+', '.data_size = %d' % len(out), new)
    open(path, 'w', encoding='utf-8', newline='\n').write(new)


def png(path, img, scale):
    h, w = len(img), len(img[0])
    raw = bytearray()
    for y in range(h * scale):
        raw.append(0)
        for x in range(w * scale):
            r, g, b, a = img[y // scale][x // scale]
            raw += bytes((r, g, b, 255)) if a else bytes((40, 40, 40, 255))
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w * scale, h * scale, 8, 6, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b''))


def block(img, x0, y0, y1):
    return [row[x0:x0 + BLOCK_W] for row in img[y0:y1]]


def empty(c):
    return c[:3] == (0xF8, 0x00, 0xF8)                 # (the chroma key: see-through, kept opaque in some pictures)


def put(img, blk, x0, y0):
    clear = img[0][0]                                  # (this picture's own see-through pixel)
    for dy, row in enumerate(blk):
        img[y0 + dy][x0:x0 + BLOCK_W] = [clear if empty(c) else c for c in row]


def icon(img, x0, colour, fill):
    """The tab block's inside (x0+3..x0+11, rows 4..12) cleared to `fill`, the cartridge drawn in `colour`."""
    for y in range(4, 13):                             # (the old icon may be wider than the cartridge: SYSTEM's gear)
        for x in range(x0 + 3, x0 + 14):
            img[y][x] = fill
    for dy, row in enumerate(ICON):
        for dx, ch in enumerate(row):
            img[4 + dy][x0 + 3 + dx] = colour if ch == '#' else fill


def main():
    d = sys.argv[1]
    prev = sys.argv[2] if len(sys.argv) > 2 else None
    pics = {n: load(os.path.join(d, n + '.c')) for n in NAMES}
    # the not-selected look of a tab (its block, rows 0..15) from STATUS, where DISPLAY (x 25) is not selected
    s_img = pics['menu_status'][4]
    black = (0, 0, 0, 255)
    grey = (0x98, 0x98, 0x98, 255)
    for n in NAMES:
        src, w, h, pal, img = pics[n]
        if any(not empty(img[y][x]) for y in range(0, 13) for x in range(SLOT_X, SLOT_X + BLOCK_W)):
            raise SystemExit('%s: the slot at x %d is not empty (made already?)' % (n, SLOT_X))
        put(img, block(s_img, DISP_X, 0, 13), SLOT_X, 0)
        icon(img, SLOT_X, grey, black)
    # GAMES: SYSTEM's picture with SYSTEM not selected (its block from STATUS) and GAMES selected
    src, w, h, pal, img = load(os.path.join(d, 'menu_system.c'))
    sys_accent = (0xF8, 0x34, 0x98, 0xFF)
    sel = block(img, SYS_X, 0, 16)                          # the selected look, SYSTEM's
    status_accent = (0xF8, 0x00, 0x00, 0xFF)
    put(img, [[sys_accent if c == status_accent else c for c in row] for row in block(s_img, SYS_X, 0, 16)], SYS_X, 0)
    put(img, sel, SLOT_X, 0)
    icon(img, SLOT_X, (0xF8, 0xFC, 0xF8, 0xFF), black)
    for y in range(h):
        for x in range(w):
            if img[y][x] == sys_accent:
                img[y][x] = ACCENT
    # the title: GAMES over the old one (rows 22..29 from x 12; the bar's pattern from x 100 stays)
    disp = load(os.path.join(d, 'menu_display.c'))[4]
    sysi = load(os.path.join(d, 'menu_system.c'))[4]
    def glyph(src_img, x0, x1):
        return [''.join('#' if src_img[y][x] == black else '.' for x in range(x0, x1 + 1)) for y in range(22, 30)]
    letters = [G, glyph(disp, 56, 63), glyph(sysi, 57, 66), glyph(sysi, 48, 55), glyph(sysi, 12, 19)]
    for y in range(22, 30):
        for x in range(12, 100):
            img[y][x] = ACCENT
    x = 12
    for gl in letters:
        for dy, row in enumerate(gl):
            for dx, ch in enumerate(row):
                if ch == '#':
                    img[22 + dy][x + dx] = black
        x += len(gl[0]) + 1
    games_src = src.replace('menu_system', 'menu_games').replace('MENU_SYSTEM', 'MENU_GAMES')
    games_src = games_src.replace('The source picture is menu_games.png.', 'Made by tools/games_tab.py (fr1) from menu_system.')
    save(os.path.join(d, 'menu_games.c'), games_src, w, h, pal, img)
    for n in NAMES:
        src0, w0, h0, pal0, img0 = pics[n]
        save(os.path.join(d, n + '.c'), src0, w0, h0, pal0, img0)
    if prev:
        os.makedirs(prev, exist_ok=True)
        for n in NAMES + ['menu_games']:
            png(os.path.join(prev, n + '.png'), load(os.path.join(d, n + '.c'))[4], 4)
    print('done: 6 pictures with the GAMES tab, menu_games.c')


if __name__ == '__main__':
    main()
