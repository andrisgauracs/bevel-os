import sys
sys.path.insert(0, "tools/assets")
import hand_glyphs as h

# the ae ligature redrawn by hand: the E bars fused with the bowl of the A/a
new = {
    "mono": {
        0xC3: ["........", "........", "#####...", "#....#..", "#...#...", "#..#....",
               "######..", "#..#....", "#...#...", "#....#..", "########", "........",
               "........", "........", "........", "........"],
        0xE6: ["........", "........", "........", "........", ".####...", "#....#..",
               "#.#.###.", "#.#..#..", "#.#..#..", "#..##.#.", ".####.##", "........",
               "........", "........", "........", "........"],
    },
    "ui": {
        0xC3: ["............", "............", "#####.......", "#....#......", "#...#.......",
               "#..#........", "######......", "#..#........", "#...#.......", "########....",
               "............", "............"],
        0xE6: [".............", ".............", ".............", ".............", ".####.......",
               "#....#......", "#.#.###.....", "#.#..#......", "#..##.#.....", ".####.###...",
               ".............", "............."],
    },
}
for font, d in new.items():
    for c, rows in d.items():
        w = max(len(r) for r in rows)
        h.EXTRA[font][c] = [r + "." * (w - len(r)) for r in rows]

out = {}
for font in ("mono", "ui"):
    out[font] = {c: h.EXTRA[font][c] for c in sorted(h.EXTRA[font])}
head = '"""Hand-drawn glyph designs for the Bevel OS fonts: Latin-1 punctuation.\n\n' \
       "'#' is ink, '.' is background. mono = 16 rows x 8 cols (caps rows 2..11, baseline row 11,\n" \
       'descenders rows 12..15). ui = 12 rows of equal length (caps rows 2..9, baseline row 9,\n' \
       'descenders rows 10..11). Designed by hand, appended to the font sources by\n' \
       'add_accents.py.\n\"\"\"\n'

def fmt(d):
    return "\n".join("    %s: [%s]," % ("0x%02X" % c, ", ".join('"%s"' % r for r in rows))
                     for c, rows in d.items())

text = head + "EXTRA = {\n    \"mono\": {\n" + fmt(out["mono"]) + "\n    },\n    \"ui\": {\n" + fmt(out["ui"]) + "\n    },\n}\n"
open("tools/assets/hand_glyphs.py", "w").write(text)
print("mono %d ui %d" % (len(out["mono"]), len(out["ui"])))
