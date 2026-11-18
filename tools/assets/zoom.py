import os
import sys
sys.path.insert(0, "tools/assets")
sys.path.insert(0, ".")
import gen_assets as g
from preview import Canvas, draw_mono, draw_ui, BLACK, WHITE

codes = [int(x, 16) for x in sys.argv[1].split(",")]
scale = int(sys.argv[2]) if len(sys.argv) > 2 else 6
font = sys.argv[3] if len(sys.argv) > 3 else "mono"
a = g.load_all()
row = "".join(chr(c) for c in codes)
w = len(codes) * (8 if font == "mono" else 16) * scale + 20
cv = Canvas(w, (16 if font == "mono" else 12) * scale + 20, WHITE)
x = 10
for c in codes:
    text = chr(c)
    if font == "mono":
        draw_mono(cv, a, x, 10, text, scale)
    else:
        draw_ui(cv, a, x, 10, text, scale)
    x += (8 if font == "mono" else a["ui_adv"][a["index"][c]] + 1) * scale
out = os.path.join("tools", "assets", "out", "zoom.png")
cv.save(out)
print("wrote %s (%d glyphs, scale %d, %s font)" % (out, len(codes), scale, font))
