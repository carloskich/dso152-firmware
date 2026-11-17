# Builds the PC page from tools/page_template.html, putting in the
# instrument's own 5x7 font (src/font5x7.h and the extra glyphs in src/lcd.c),
# so the page draws the same screen pixel for pixel.
import json
import os
import re
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
font_h = open(os.path.join(root, "src", "font5x7.h"), encoding="utf-8").read()
glyphs = [[int(v, 16) for v in m.split(",")] for m in re.findall(r"\{(0x[0-9A-Fa-f]{2}(?:,\s*0x[0-9A-Fa-f]{2}){4})\}", font_h)]
assert len(glyphs) == 95, len(glyphs)
lcd_c = open(os.path.join(root, "src", "lcd.c"), encoding="utf-8").read()
extra = {}
for code, cols in re.findall(r"\{(0x[0-9A-Fa-f]+),\s*\{([^}]*)\}\}", lcd_c):
    extra[int(code, 16)] = [int(v, 16) for v in cols.split(",")]
page = open(os.path.join(root, "tools", "page_template.html"), encoding="utf-8").read()
page = page.replace("/*FONT*/", json.dumps(glyphs, separators=(",", ":")))
page = page.replace("/*EXTRA*/", json.dumps(extra, separators=(",", ":")))
for out in sys.argv[1:]:
    with open(out, "w", encoding="utf-8") as f:
        f.write(page)
    print("wrote", out, len(page), "bytes,", len(extra), "extra glyphs")
