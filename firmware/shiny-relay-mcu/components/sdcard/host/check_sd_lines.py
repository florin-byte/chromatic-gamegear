"""Every OSD row of the microSD card's pages fits the panel (lane sdcard1): every upper-case string literal
in games.c, the card's result rows ("ROW1|ROW2") in sd.c, its state words in sd_card.c and the RAM cart's
card rows in cf_rc_dev.c, formats filled with their longest values. fr1: the lines marked "(fr1: whole screen)" are the GAMES
library's, drawn over the whole screen (x 8, edge 152, 36 characters). usage: check_sd_lines.py FONT.c FILES..."""
import re, sys
src = open(sys.argv[1]).read()
DSC = [tuple(int(v) for v in d) for d in re.findall(
    r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}", src)]
def end_x(t): return 77 + (sum(DSC[ord(c) - 32 + 1][1] for c in t) >> 4)
FILL = {"%d%%": "100%", "%d": "8888", "%lu": "8888", "%s": ""}
lits = set()
full = set()
for f in sys.argv[2:]:
    s = open(f).read()
    for line in s.split("\n"):
        if "(fr1: whole screen)" in line:
            for t in re.findall(r'"((?:[ >!]?)[A-Z0-9][A-Z0-9 ,.:?/%()\'\-|>]*)"', line):
                for part in t.split("|"):
                    for k, v in FILL.items(): part = part.replace(k, v)
                    if part: full.add(part)
    s = "\n".join(l for l in s.split("\n") if "(fr1: whole screen)" not in l)
    for t in re.findall(r'"((?:[ >!]?)[A-Z0-9][A-Z0-9 ,.:?/%()\'\-|>]*)"', s):
        if "  " not in t and "|" not in t and len(t) > 12: continue      # (not OSD copy: OSD rows use two spaces)
        for part in t.split("|"):
            if not part: continue
            for k, v in FILL.items(): part = part.replace(k, v)
            lits.add(part)
# rows made of two literals
lits.add(">" + "WRITE  TO  CART"); lits.add("1234  GAMES,  LOCKED"); lits.add("8888.8  GB  FREE"); lits.add("4  MB  ?  COLOR")
lits.add("1023  KB  MBC5  COLOR"); lits.add("GAMES  8888/8888"); lits.add("8888  GAMES,  EXFAT")
bad = 0
for t in sorted(lits):
    e = end_x(t); over = e > 148 or len(t) > 23
    if over: print("%3d px %2d ch OVER |%s|" % (e, len(t), t))
    bad += over
for t in sorted(full):
    e = end_x(t) - 77 + 8; over = e > 152 or len(t) > 36
    if over: print("%3d px %2d ch OVER the screen |%s|" % (e, len(t), t))
    bad += over
print("%d rows (%d over the whole screen), %d over" % (len(lits) + len(full), len(full), bad))
sys.exit(1 if bad else 0)
