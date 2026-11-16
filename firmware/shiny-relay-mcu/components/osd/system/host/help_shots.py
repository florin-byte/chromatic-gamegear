"""PNGs of every HOW TO PLAY ONLINE page (play_help.c), drawn by cwl/host/osd_phone.py.

usage: help_shots.py OSD_PHONE.py FONT.c BG.png OUT_DIR play_help.c
"""
import re
import subprocess
import sys

shot, font, bg, out, src = sys.argv[1:6]
body = open(src).read().split("g_PlayHelp[][kPlayHelpRows] = {", 1)[1].split("};", 1)[0]
pages = [re.findall(r'^\s+"(.*)",$', blk, re.M) for blk in body.split("    {\n")[1:]]
# as the device draws them: a row is cut at kTextLen - 1 characters (network.c)
import os
maxch = int(re.search(r"kTextLen = (\d+)", open(os.path.join(os.path.dirname(os.path.abspath(src)), "network.c")).read()).group(1)) - 1
pages = [[("!" + r[1:][:maxch]) if r.startswith("!") else r[:maxch] for r in rows] for rows in pages]
for i, rows in enumerate(pages):
    subprocess.check_call([sys.executable, "-I", shot, font, bg, "%s/help_%d.png" % (out, i + 1)] + rows)
print("%d pages" % len(pages))
