"""Every text literal drawn on SYSTEM > NETWORK, and every help page row, fits the panel.

usage: check_lines.py FONT.c network.c|download.c [cf_install.c]
Literals: the last string argument of each Line(...) call and every snprintf format
in the file that draws a row (the %s parts are checked where their values are known:
the help pages and section names are in play_help.c, checked by make_play_help.py).
"""
import re
import sys

font_c, net_c = sys.argv[1], sys.argv[2]
src = open(font_c).read()
DSC = [tuple(int(v) for v in d) for d in re.findall(
    r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}", src)]


def end_x(t):
    return 77 + (sum(DSC[ord(c) - 32 + 1][1] for c in t) >> 4)


s = open(net_c).read()
# a row is kept in a char[kTextLen]: at most kTextLen - 1 characters are drawn, whatever their
# width (0.17.9's help footer was 25 characters and lost its last two on a unit, 2026-10-11)
MAXCH = int(re.search(r"kTextLen = (\d+)", s).group(1)) - 1
lits = re.findall(r'Line\(pScreen, \d+, [^;]*?"([^"]*)"\)', s)
lits += re.findall(r'snprintf\((?:Status|Detail), kTextLen, "([^"%]*)"', s)
# formats with a value in them: checked with the longest value each can take
fmts = re.findall(r'snprintf\(\w+, (?:kTextLen|sizeof\(?\w+\)?), "([^"]*%[^"]*)"', s)
def worst(f):
    # %s: a mark (1 char), a room code (6), a name; %u: one or two digits. The longest known:
    # "%s  %s" = "HOSTING  ABCDEF"; "%sJOIN %s" = ">JOIN ABCDEF"; "PLAYERS  %u  OF  %u" = 2 digits each.
    if f == "%s  %s": return "HOSTING  WWWWWW"
    if f == "%s%s": return ">" + "W" * 0          # mark + a section name (checked by make_play_help.py)
    return f.replace("%sJOIN %s", ">JOIN WWWWWW").replace("%s", ">").replace("%u", "88").replace("%d", "8")
lits += [worst(f) for f in fmts if worst(f)]
# SYSTEM > DOWNLOAD (download.c, wifirecv1): the rows are arrays R[kRows] = { ... }, and the two
# rows of why come from the install machine (cf_install.c, "ROW1|ROW2" literals)
for blk in re.findall(r"R\[kRows\] = \{(.*?)\};", s, re.S):
    lits += re.findall(r'"([^"]*)"', blk)
lits += [f.replace("%u", "88") for f in re.findall(r'snprintf\(\w+, sizeof \w+, "([^"]*%u[^"]*)"', s)]
for extra in sys.argv[3:]:
    for t in re.findall(r'"([A-Z][A-Z0-9 ,.\-]*\|[A-Z0-9 ,.\-]*)"', open(extra).read()):
        lits += t.split("|")
    lits += re.findall(r'"(GETTING  READY|RESUMING|TRYING  AGAIN  SOON|INSTALLED|NOT  INSTALLED)"', open(extra).read())
bad = 0
for t in sorted(set(lits)):
    e = end_x(t)
    over = e > 148 or len(t) > MAXCH
    print("%3d px %2d ch %s |%s|" % (e, len(t), "OVER" if over else "OK  ", t))
    bad += over
print("%d literals, %d over" % (len(set(lits)), bad))
sys.exit(1 if bad else 0)
