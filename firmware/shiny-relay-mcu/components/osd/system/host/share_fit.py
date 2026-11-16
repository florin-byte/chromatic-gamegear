"""share_fit.py FONT.c < share_shots output: every row of every share page fits the panel (end x <= 148, <= 23 characters)."""
import re, sys
src = open(sys.argv[1]).read()
DSC = [tuple(int(v) for v in d) for d in re.findall(r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}", src)]
def end_x(t): return 77 + (sum(DSC[ord(c) - 32 + 1][1] for c in t) >> 4)
bad = n = 0
for line in sys.stdin:
    f = line.rstrip("\n").split("|")
    for i, r in enumerate(f[1:]):
        t = r[1:] if r.startswith("!") else r
        if not t: continue
        n += 1
        e = end_x(t)
        if e > 148 or len(t) > 23:
            bad += 1; print("OVER %s row %d: %d px %d ch |%s|" % (f[0], i, e, len(t), t))
print("share_fit: %d rows, %d over" % (n, bad))
sys.exit(1 if bad else 0)
