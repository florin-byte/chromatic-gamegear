#!/usr/bin/env python3
"""lv_indexed.py (lane fwshrink1): store an LVGL 8 image as LV_IMG_CF_INDEXED_{1,2,4,8}BIT, pixel for pixel.

The images in this folder came from the LVGL image converter as RGB565 (TRUE_COLOR, TRUE_COLOR_CHROMA_KEYED)
or RGB565 + alpha (TRUE_COLOR_ALPHA), every colour depth in one file. Each has 2 to 18 distinct pixels, so a
palette of 2/4/16/256 entries holds it exactly in a quarter to an eighth of the bytes.

Exactness (LV_COLOR_DEPTH 16, no swap): the palette entry of an RGB565 value (r5, g6, b5) is the lv_color32
(b5 << 3, g6 << 2, r5 << 3, alpha) and LVGL's decoder makes lv_color_make(r8, g8, b8) = (r8 >> 3, g8 >> 2, b8 >> 3)
of it: the same 16-bit value. Alpha: 255 for TRUE_COLOR; 0 for the chroma key (0xF81F) and 255 for the rest of a
CHROMA_KEYED image (what its draw mask is); the pixel's own alpha for TRUE_COLOR_ALPHA. The decoder hands LVGL
(colour, alpha) per pixel: what the draw of the original produced.

  lv_indexed.py convert SRC.c OUT.c   write the indexed form of SRC (an original converter .c), check it, print sizes
  lv_indexed.py check ORIG.c NEW.c    decode NEW (the indexed .c) and compare every pixel with ORIG
An image that is not shorter indexed is left as it is (convert copies it).
"""
import re, sys

CHROMA = 0xF81F  # CONFIG_LV_COLOR_CHROMA_KEY_HEX 0xFF00FF in RGB565


def parse_orig(path):
    s = open(path).read()
    name = re.search(r'uint8_t (\w+)_map\[\]', s).group(1)
    cf = re.search(r'\.header\.cf = (\w+)', s).group(1)
    w = int(re.search(r'\.header\.w = (\d+)', s).group(1))
    h = int(re.search(r'\.header\.h = (\d+)', s).group(1))
    body = re.search(r'#if LV_COLOR_DEPTH == 16 && LV_COLOR_16_SWAP == 0(.*?)#endif', s, re.S).group(1)
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    b = bytes(int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]{2}', body))
    bpp = 3 if cf == 'LV_IMG_CF_TRUE_COLOR_ALPHA' else 2
    assert cf in ('LV_IMG_CF_TRUE_COLOR', 'LV_IMG_CF_TRUE_COLOR_CHROMA_KEYED', 'LV_IMG_CF_TRUE_COLOR_ALPHA'), cf
    assert len(b) == w * h * bpp, (path, len(b), w * h * bpp)
    px = []  # what LVGL draws: (rgb565, alpha); a transparent pixel's colour is not drawn
    for i in range(w * h):
        c = b[i * bpp] | (b[i * bpp + 1] << 8)
        if bpp == 3:
            a = b[i * bpp + 2]
        elif cf == 'LV_IMG_CF_TRUE_COLOR_CHROMA_KEYED' and c == CHROMA:
            a = 0
        else:
            a = 255
        px.append((c, a))
    return dict(name=name, cf=cf, w=w, h=h, px=px, nbytes=len(b), text=s)


def entry(c, a):  # lv_color32_t in memory: blue, green, red, alpha
    return bytes((((c & 0x1F) << 3), (((c >> 5) & 0x3F) << 2), (((c >> 11) & 0x1F) << 3), a))


def encode(d):
    cols = sorted(set(d['px']), key=lambda p: (p[1] != 0, p))  # transparent first
    n = len(cols)
    bits = next(b for b in (1, 2, 4, 8) if n <= (1 << b))
    idx = {p: i for i, p in enumerate(cols)}
    pal = b''.join(entry(c, a) for c, a in cols) + bytes(4 * ((1 << bits) - n))
    rowb = (d['w'] * bits + 7) // 8
    out = bytearray()
    for y in range(d['h']):
        row = bytearray(rowb)
        for x in range(d['w']):
            v = idx[d['px'][y * d['w'] + x]]
            pos = 8 - bits - (x * bits) % 8
            row[(x * bits) // 8] |= v << pos
        out += row
    return bits, pal + bytes(out)


def decode(data, bits, w, h):
    pal = []
    for i in range(1 << bits):
        bb, g, r, a = data[4 * i:4 * i + 4]
        pal.append((((r >> 3) << 11) | ((g >> 2) << 5) | (bb >> 3), a))
    rowb = (w * bits + 7) // 8
    base = 4 << bits
    px = []
    for y in range(h):
        for x in range(w):
            byte = data[base + y * rowb + (x * bits) // 8]
            pos = 8 - bits - (x * bits) % 8
            px.append(pal[(byte >> pos) & ((1 << bits) - 1)])
    return px


def same(orig_px, new_px):
    # a pixel with alpha 0 is not drawn whatever its colour
    return all((o[1] == n[1]) and (o[1] == 0 or o[0] == n[0]) for o, n in zip(orig_px, new_px)) and len(orig_px) == len(new_px)


def write_c(d, bits, data, out):
    name = d['name']
    lines = []
    for i in range(0, len(data), 24):
        lines.append('  ' + ', '.join('0x%02x' % x for x in data[i:i + 24]) + ',')
    attr = 'LV_ATTRIBUTE_IMG_' + name.upper()
    src = f"""// {name}: {d['w']} x {d['h']}, LV_IMG_CF_INDEXED_{bits}BIT ({len(data)} bytes: a {1 << bits}-entry palette + {bits} bit per pixel).
// Made by tools/lv_indexed.py from the LVGL converter's {d['cf']} output ({d['nbytes']} bytes at 16 bit):
// pixel for pixel the same at LV_COLOR_DEPTH 16 (lane fwshrink1). The source picture is {name}.png.
#ifdef __has_include
    #if __has_include("lvgl.h")
        #ifndef LV_LVGL_H_INCLUDE_SIMPLE
            #define LV_LVGL_H_INCLUDE_SIMPLE
        #endif
    #endif
#endif

#if defined(LV_LVGL_H_INCLUDE_SIMPLE)
    #include "lvgl.h"
#else
    #include "lvgl/lvgl.h"
#endif

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

#ifndef {attr}
#define {attr}
#endif

const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST {attr} uint8_t {name}_map[] = {{
{chr(10).join(lines)}
}};

const lv_img_dsc_t {name} = {{
  .header.cf = LV_IMG_CF_INDEXED_{bits}BIT,
  .header.always_zero = 0,
  .header.reserved = 0,
  .header.w = {d['w']},
  .header.h = {d['h']},
  .data_size = {len(data)},
  .data = {name}_map,
}};
"""
    open(out, 'w').write(src)


def parse_new(path):
    s = open(path).read()
    bits = int(re.search(r'LV_IMG_CF_INDEXED_(\d)BIT,', s).group(1))
    w = int(re.search(r'\.header\.w = (\d+)', s).group(1))
    h = int(re.search(r'\.header\.h = (\d+)', s).group(1))
    n = int(re.search(r'\.data_size = (\d+)', s).group(1))
    arr = re.search(r'_map\[\] = \{(.*?)\};', s, re.S).group(1)
    data = bytes(int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]{2}', arr))
    assert len(data) == n
    return bits, w, h, data


def main():
    mode, a, b = sys.argv[1:4]
    if mode == 'convert':
        d = parse_orig(a)
        bits, data = encode(d)
        if len(data) >= d['nbytes']:
            open(b, 'w').write(d['text'])
            print(f"{d['name']}: kept {d['cf']} ({d['nbytes']} bytes; indexed would be {len(data)})")
            return
        write_c(d, bits, data, b)
        bits2, w, h, data2 = parse_new(b)
        assert same(d['px'], decode(data2, bits2, w, h)), d['name']
        print(f"{d['name']}: {d['cf']} {d['nbytes']} -> INDEXED_{bits}BIT {len(data)} (saves {d['nbytes'] - len(data)}), every pixel the same")
    elif mode == 'check':
        d = parse_orig(a)
        if 'LV_IMG_CF_INDEXED_' not in open(b).read():
            ok = open(a).read() == open(b).read()
        else:
            bits, w, h, data = parse_new(b)
            ok = (w, h) == (d['w'], d['h']) and same(d['px'], decode(data, bits, w, h))
        print(f"{d['name']}: {'same' if ok else 'DIFFERS'}")
        sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
