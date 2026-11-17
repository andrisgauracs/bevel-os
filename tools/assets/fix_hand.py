import sys
sys.path.insert(0, "tools/assets")
import hand_glyphs as h

head = '"""Hand-drawn glyph designs for the Bevel OS fonts: Latin-1 punctuation.\n\n' \
       "'#' is ink, '.' is background. mono = 16 rows x 8 cols (caps rows 2..11, baseline row 11,\n" \
       'descenders rows 12..15). ui = 12 rows of equal length (caps rows 2..9, baseline row 9,\n' \
       'descenders rows 10..11). Designed by hand, appended to the font sources by\n' \
       'add_accents.py.\n\"\"\"\n'

e = {k: {c: h.EXTRA[k][c] for c in sorted(h.EXTRA[k])} for k in ("mono", "ui")}
fixed = 0
for k in e:
    for c, rows in e[k].items():
        w = max(len(r) for r in rows)
        assert set("".join(rows)) <= {"#", "."}, (k, hex(c))
        if any(len(r) != w for r in rows):
            fixed += 1
            e[k][c] = [r + "." * (w - len(r)) for r in rows]
        assert len(e[k][c]) == (16 if k == "mono" else 12), (k, hex(c))
print("padded glyphs:", fixed)

def fmt(d):
    out = []
    for k, rows in d.items():
        out.append("    %s: [%s]," % ("0x%02X" % k, ", ".join('"%s"' % r for r in rows)))
    return "\n".join(out)

text = head + "EXTRA = {\n    \"mono\": {\n" + fmt(e["mono"]) + "\n    },\n    \"ui\": {\n" + fmt(e["ui"]) + "\n    },\n}\n"
open("tools/assets/hand_glyphs.py", "w").write(text)
print("mono %d ui %d" % (len(e["mono"]), len(e["ui"])))
