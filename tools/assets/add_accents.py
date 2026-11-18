#!/usr/bin/env python3
"""Write the accented glyphs the keyboard layouts need into the two font sources.

The fonts are ASCII-art files in tools/assets/src/ and start as plain ASCII 32..126.
This script reads the base letters from them and writes everything above 126 below them,
in the same format, so a letter tuned by hand is never redrawn twice. Running it again
writes the same file.

A mark is centred on the ink of its letter and always keeps one empty row from it:
  - lowercase has room above its x-height; i and j-like dots are removed first;
  - a capital reaches almost to the top of the cell, so it gives up one of its repeated
    rows and moves down by one, which makes room for the mark;
  - a cedilla hangs from the bottom of the letter, touching it.
Acute rises to the right, grave rises to the left. A one-column letter (the UI font's i
and I) is widened to three columns so that the mark fits.

Letters with no base (sharp s, ae) and the marks a dead key prints on its own are drawn
by hand here; the Latin-1 punctuation is in hand_glyphs.py.

Usage: python3 tools/assets/add_accents.py, then tools/assets/gen_assets.py
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hand_glyphs  # the hand-drawn punctuation designs, one per code point in each font
SRC = os.path.join(HERE, "src")
MARK = "; --- accented glyphs (written by add_accents.py, do not edit by hand) ---"

# code -> (base letter, mark)
ACCENTS = {
    0xE0: ("a", "grave"), 0xE1: ("a", "acute"), 0xE2: ("a", "circumflex"), 0xE4: ("a", "diaeresis"),
    0xE8: ("e", "grave"), 0xE9: ("e", "acute"), 0xEA: ("e", "circumflex"), 0xEB: ("e", "diaeresis"),
    0xEC: ("i", "grave"), 0xED: ("i", "acute"), 0xEE: ("i", "circumflex"), 0xEF: ("i", "diaeresis"),
    0xF2: ("o", "grave"), 0xF3: ("o", "acute"), 0xF4: ("o", "circumflex"), 0xF6: ("o", "diaeresis"),
    0xF9: ("u", "grave"), 0xFA: ("u", "acute"), 0xFB: ("u", "circumflex"), 0xFC: ("u", "diaeresis"),
    0xF1: ("n", "tilde"), 0xE7: ("c", "cedilla"),
    0xC0: ("A", "grave"), 0xC1: ("A", "acute"), 0xC2: ("A", "circumflex"), 0xC3: ("A", "tilde"),
    0xC4: ("A", "diaeresis"),
    0xC8: ("E", "grave"), 0xC9: ("E", "acute"), 0xCA: ("E", "circumflex"), 0xCB: ("E", "diaeresis"),
    0xCC: ("I", "grave"), 0xCD: ("I", "acute"), 0xCE: ("I", "circumflex"), 0xCF: ("I", "diaeresis"),
    0xD2: ("O", "grave"), 0xD3: ("O", "acute"), 0xD4: ("O", "circumflex"), 0xD6: ("O", "diaeresis"),
    0xD9: ("U", "grave"), 0xDA: ("U", "acute"), 0xDB: ("U", "circumflex"), 0xDC: ("U", "diaeresis"),
    0xD1: ("N", "tilde"), 0xC7: ("C", "cedilla"),
}

# the rows of each font: cell height, top of the capitals, top of the x-height, baseline row
METRICS = {"mono": (16, 2, 5, 11), "ui": (12, 2, 4, 9)}


def art(height, first, rows, width=None):
    """A glyph of the given height from rows placed at row 'first'."""
    w = width or len(rows[0])
    return ["." * w] * first + rows + ["." * w] * (height - first - len(rows))


HAND = {
    "mono": {
        0xDF: art(16, 2, ["..###...", ".#...#..", ".#...#..", ".#..#...", ".#.##...",
                          ".#...#..", ".#....#.", ".#....#.", ".#...#..", ".#.##..."]),
        0xC6: art(16, 2, ["..######", ".##.#...", ".#..#...", "#...#...", "#...####",
                          "#####...", "#...#...", "#...#...", "#...#...", "#...####"]),
        0xE6: art(16, 5, [".##.##..", "...#..#.", "...#..#.", ".######.", "#..#....",
                          "#..#..#.", ".##.##.."]),
        0xB4: art(16, 2, ["....#...", "...#...."]),
        0xA8: art(16, 3, ["..#..#.."]),
    },
    "ui": {
        0xDF: art(12, 2, [".##..", "#..#.", "#..#.", "#.#..", "#..#.", "#...#", "#...#", "#.##."]),
        0xC6: art(12, 2, ["...#####", "..##....", ".#.#....", ".#.####.", "#..#....", "####....",
                          "#..#....", "#..#####"]),
        0xE6: art(12, 4, [".##.##.", "...#..#", ".######", "#..#...", "#..#..#", ".##.##."]),
        0xB4: art(12, 0, ["...#", "..#."]),
        0xA8: art(12, 0, ["#..#"]),
    },
}
# the hand-drawn punctuation designs join them
for font in HAND:
    HAND[font].update(hand_glyphs.EXTRA[font])

# the two rows of a tilde, by the width of the letter under it
TILDE = {5: (".##.#", "#..#."), 6: (".##..#", "#..##."), 7: (".###..#", "#...##.")}


def ink_span(rows):
    cols = [x for r in rows for x, c in enumerate(r) if c == "#"]
    return min(cols), max(cols)


def put(rows, y, x):
    rows[y] = rows[y][:x] + "#" + rows[y][x + 1:]


def make_room_above(rows, top, base):
    """Drop one repeated row of a capital and move what was above it down by one."""
    for y in range(top + 1, base):
        if rows[y] == rows[y + 1]:
            return ["." * len(rows[0])] + rows[:y] + rows[y + 1:]
    raise ValueError("no repeated row to remove")


def accented(base_rows, letter, mark, font):
    _, cap_top, x_top, base = METRICS[font]
    rows = list(base_rows)
    if letter.islower():
        rows = ["." * len(r) if y < x_top else r for y, r in enumerate(rows)]  # no dot on the i
    if ink_span(rows)[1] == ink_span(rows)[0]:                                  # a single stem
        rows = ["." + r + "." for r in rows]
    if mark != "cedilla" and letter.isupper():
        rows = make_room_above(rows, cap_top, base)
        bottom = cap_top - 1        # the row above is the mark's gap to the moved-down letter
    else:
        bottom = x_top - 2
    lo, hi = ink_span(rows)
    span = hi - lo + 1
    left, right = (lo + hi) // 2, (lo + hi + 1) // 2    # the same column when the span is odd
    even = left != right
    if mark == "acute":
        high, low = (right, left) if even else (left + 1, left)
        put(rows, bottom - 1, high); put(rows, bottom, low)
    elif mark == "grave":
        high, low = (left, right) if even else (left - 1, left)
        put(rows, bottom - 1, high); put(rows, bottom, low)
    elif mark == "circumflex":
        put(rows, bottom - 1, left); put(rows, bottom - 1, right)
        put(rows, bottom, left - 1); put(rows, bottom, right + 1)
    elif mark == "diaeresis":
        reach = 2 if not even and span > 5 else 1
        put(rows, bottom, left - reach); put(rows, bottom, right + reach)
    elif mark == "tilde":
        for y, pattern in zip((bottom - 1, bottom), TILDE[span]):
            for x, c in enumerate(pattern):
                if c == "#":
                    put(rows, y, lo + x)
    elif mark == "cedilla":
        stem = math.ceil((lo + hi) / 2)
        put(rows, base + 1, stem)
        if font == "mono":
            put(rows, base + 2, stem + 1); put(rows, base + 3, stem); put(rows, base + 3, stem - 1)
        else:
            put(rows, base + 2, stem); put(rows, base + 2, stem - 1)
    return rows


def parse(path):
    """Return the base lines (without the generated block) and {code: rows}."""
    with open(path, "r", encoding="ascii") as f:
        lines = f.read().split("\n")
    cut = None
    for i, l in enumerate(lines):
        if l.strip() == MARK:
            cut = i
            break
    base = lines[:cut] if cut is not None else lines
    glyphs = {}
    cur = None
    for line in base:
        s = line.rstrip()
        if not s or s.startswith(";"):
            continue
        if s.startswith("@"):
            tok = s.split()[1]
            code = 32 if tok == "space" else (int(tok, 16) if tok.lower().startswith("0x") else ord(tok))
            cur = glyphs[code] = []
        else:
            cur.append(s)
    return base, glyphs


def main(argv):
    for name, font in (("font_mono.txt", "mono"), ("font_ui.txt", "ui")):
        path = os.path.join(SRC, name)
        base, glyphs = parse(path)
        height = METRICS[font][0]
        for code, rows in HAND[font].items():
            assert len(rows) == height, "%s 0x%02X: hand glyph has %d rows" % (name, code, len(rows))
            glyphs[code] = rows
        for code, (letter, mark) in ACCENTS.items():
            glyphs[code] = accented(glyphs[ord(letter)], letter, mark, font)
        out = list(base)
        if out and out[-1] != "":
            out.append("")
        out.append(MARK)
        for code in sorted(glyphs):
            if code <= 126:
                continue
            out.append("@ 0x%02X" % code)
            out.extend(glyphs[code])
        with open(path, "w", encoding="ascii", newline="\n") as f:
            f.write("\n".join(out) + "\n")
        print("%s: %d glyphs" % (name, len(glyphs)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
