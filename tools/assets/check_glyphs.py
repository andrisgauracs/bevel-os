import sys
sys.path.insert(0, "tools/assets")
import gen_assets as g

a = g.load_all()
words = "Ànaïs - Seúl  Ångström: España? Ñoño l'après-midi über Zürich, café Crème brûlée"
missing = [(c, hex(ord(c))) for c in words if a["index"][ord(c)] == 0xFFFF]
print("missing:", missing)
print("order len:", len(a["order"]), "extra:", [hex(c) for c in a["order"][95:]])
# what the layouts ask for
import re
need = set()
for line in open("main/layout_gen.c", encoding="utf-8"):
    m = re.match(r"\s+\{(\d+),(\d+),(\d+),(\d+)\}, // 0x\w\w (.*)", line)
    if m:
        for v in m.groups()[:4]:
            v = int(v)
            if v > 126 and a["index"][v] == 0xFFFF:
                need.add(hex(v))
print("layout codes with no glyph:", sorted(need))
