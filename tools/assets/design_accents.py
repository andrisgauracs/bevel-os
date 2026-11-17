"""Add the missing diacritics to the accented letter glyphs in the two font sources.

The generator reads "@ <code>" blocks of '#'/'.' rows. The accented letters were copied from
their base letters with no accent marks, so this script rebuilds each one from the base glyph
plus the mark drawn in the empty rows above the letter (or below, for the cedilla).

font_ui.txt blocks are 12 rows tall, the letters sit in rows 4..9, so marks go in rows 2..3.
font_mono.txt blocks are 16 rows tall, the letters sit in rows 5..11, so marks go in rows 3..4.
"""

CAPS = {
    '0xC0': ('A', 'grave'), '0xC1': ('A', 'acute'), '0xC2': ('A', 'circumflex'),
    '0xC3': ('A', 'tilde'),  '0xC4': ('A', 'diaeresis'),
    '0xC8': ('E', 'grave'),  '0xC9': ('E', 'acute'),  '0xCA': ('E', 'circumflex'),
    '0xCB': ('E', 'diaeresis'),
    '0xCC': ('I', 'grave'),  '0xCD': ('I', 'acute'),  '0xCE': ('I', 'circumflex'),
    '0xCF': ('I', 'diaeresis'),
    '0xD1': ('N', 'tilde'),
    '0xD2': ('O', 'grave'),  '0xD3': ('O', 'acute'),  '0xD4': ('O', 'circumflex'),
    '0xD6': ('O', 'diaeresis'),
    '0xD9': ('U', 'grave'),  '0xDA': ('U', 'acute'),  '0xDB': ('U', 'circumflex'),
    '0xDC': ('U', 'diaeresis'),
}
SMALL = {
    '0xE0': ('a', 'grave'), '0xE1': ('a', 'acute'), '0xE2': ('a', 'circumflex'),
    '0xE4': ('a', 'diaeresis'),
    '0xE8': ('e', 'grave'), '0xE9': ('e', 'acute'), '0xEA': ('e', 'circumflex'),
    '0xEB': ('e', 'diaeresis'),
    '0xEC': ('i', 'grave'), '0xED': ('i', 'acute'), '0xEE': ('i', 'circumflex'),
    '0xEF': ('i', 'diaeresis'),
    '0xF1': ('n', 'tilde'),
    '0xF2': ('o', 'grave'), '0xF3': ('o', 'acute'), '0xF4': ('o', 'circumflex'),
    '0xF6': ('o', 'diaeresis'),
    '0xF9': ('u', 'grave'), '0xFA': ('u', 'acute'), '0xFB': ('u', 'circumflex'),
    '0xFC': ('u', 'diaeresis'),
}
CEDILLA = {'0xE7': 'c', '0xC7': 'C'}


def blocks_of(path):
    """header line -> list of rows"""
    lines = open(path, encoding="utf-8").read().split("\n")
    heads = [i for i, l in enumerate(lines) if l.startswith("@ ")]
    out = {}
    for n, i in enumerate(heads):
        end = heads[n + 1] if n + 1 < len(heads) else len(lines)
        out[lines[i]] = [r for r in lines[i + 1:end] if r]
    return out


def diac(kind, w, top):
    """(row, columns) marks centred over a letter of width w whose first letter row is top"""
    mid = (w - 1) // 2
    if kind == "acute":
        return [(top - 1, [mid + 1]), (top - 2, [mid])]
    if kind == "grave":
        return [(top - 1, [mid]), (top - 2, [mid - 1])]
    if kind == "circumflex":
        return [(top - 1, [mid - 1, mid + 1]), (top - 2, [mid])]
    if kind == "diaeresis":
        return [(top - 2, [mid - 1, mid + 1])]
    if kind == "tilde":
        return [(top - 1, [mid - 1]), (top - 2, [mid - 2, mid]), (top - 3, [mid + 1])]


def draw(base, spec, w):
    rows = [list(r) + ["."] * (w - len(r)) for r in base]
    for r, cols in spec:
        for c in cols:
            if 0 <= r < len(rows) and 0 <= c < w:
                rows[r][c] = "#"
    return ["".join(r) for r in rows]


def cedilla(base, w):
    rows = [list(r) + ["."] * (w - len(r)) for r in base]
    rows[-2][w - 2] = "#"
    rows[-1][w - 3] = "#"
    return ["".join(r) for r in rows]


def patch(path, top):
    blocks = blocks_of(path)
    new = {}
    for code, (ltr, kind) in list(CAPS.items()) + list(SMALL.items()):
        base = blocks["@ " + ltr]
        w = max(len(r) for r in base)
        if w == 1:  # i and I: widen so the mark has room beside the stem
            base = ["." + r + "." for r in base]
            w = 3
        new["@ " + code] = draw(base, diac(kind, w, top), w)
    for code, ltr in CEDILLA.items():
        base = blocks["@ " + ltr]
        w = max(len(r) for r in base)
        new["@ " + code] = cedilla(base, w)

    out = []
    for head, rows in blocks.items():
        out.append(head)
        out.extend(new.get(head, rows))
    open(path, "w", encoding="utf-8").write("\n".join(out) + "\n")
    print("patched", path, len(blocks), "glyphs,", len(new), "redrawn")


patch("tools/assets/src/font_ui.txt", 4)
patch("tools/assets/src/font_mono.txt", 5)
