import re

t = open("main/layout_gen.c", encoding="utf-8").read()
tables = {}
cur = None
for line in t.split("\n"):
    m = re.match(r"static const uint16_t (\w+)_cp", line)
    if m:
        cur = m.group(1)
        tables[cur] = {}
        continue
    if line.startswith("static"):
        cur = None
        continue
    if cur:
        m = re.match(r"\s+\{\d+,\d+,\d+,\d+\}, // 0x(\w\w) (.*)", line)
        if m:
            tables[cur][int(m.group(1), 16)] = m.group(2)

pos = [(0x1E, "1"), (0x1F, "2"), (0x20, "3"), (0x21, "4"), (0x22, "5"), (0x23, "6"), (0x24, "7"),
       (0x25, "8"), (0x26, "9"), (0x27, "0"), (0x2D, "AE11"), (0x2E, "AE12"), (0x35, "TLDE")]
r1 = [(0x14, "Q"), (0x1A, "W"), (0x08, "E"), (0x15, "R"), (0x17, "T"), (0x1C, "Y"), (0x18, "U"),
      (0x0C, "I"), (0x12, "O"), (0x13, "P"), (0x2F, "AD11"), (0x30, "AD12")]
r2 = [(0x04, "A"), (0x16, "S"), (0x07, "D"), (0x09, "F"), (0x0A, "G"), (0x0B, "H"), (0x0D, "J"),
      (0x0E, "K"), (0x0F, "L"), (0x33, "AC10"), (0x34, "AC11")]
r3 = [(0x1D, "Z"), (0x1B, "X"), (0x06, "C"), (0x19, "V"), (0x05, "B"), (0x11, "N"), (0x10, "M"),
      (0x36, "AB08"), (0x37, "AB09"), (0x38, "AB10")]

with open("layout_review.txt", "w", encoding="utf-8") as out:
    for name in ("us", "gb", "de", "fr", "es", "ch_de", "ch_fr"):
        tab = tables[name]
        out.write("== %s\n" % name)
        for lst, lab in ((pos, "top"), (r1, "r1"), (r2, "r2"), (r3, "r3")):
            out.write("  %s: %s\n" % (lab, "  ".join("%s=%s" % (l, tab.get(u, "?")) for u, l in lst)))
        out.write("  key next to left shift: %s\n" % tab.get(0x64, "?"))
print(open("layout_review.txt", encoding="utf-8").read())
