#!/usr/bin/env python3
"""Append the accented glyphs the keyboard layouts need to the two font sources.

The fonts are ASCII-art files in tools/assets/src/ and start as plain ASCII 32..126.
This script reads the base glyphs from them and writes the accent versions below them,
in the same format, so a letter that was tuned by hand is never redrawn by hand twice.
Running the script again writes the same file.

Marks sit above the letter (2 rows for acute, grave, circumflex; 1 row for diaeresis
and tilde) or below it (cedilla). Sharp s and the two marks a dead key prints on its
own are drawn by hand: they have no base letter.

Usage: python3 tools/assets/add_accents.py
"""
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
    0xC0: ("A", "grave"), 0xC1: ("A", "acute"), 0xC2: ("A", "circumflex"), 0xC4: ("A", "diaeresis"),
    0xC8: ("E", "grave"), 0xC9: ("E", "acute"), 0xCA: ("E", "circumflex"), 0xCB: ("E", "diaeresis"),
    0xCC: ("I", "grave"), 0xCD: ("I", "acute"), 0xCE: ("I", "circumflex"), 0xCF: ("I", "diaeresis"),
    0xD2: ("O", "grave"), 0xD3: ("O", "acute"), 0xD4: ("O", "circumflex"), 0xD6: ("O", "diaeresis"),
    0xD9: ("U", "grave"), 0xDA: ("U", "acute"), 0xDB: ("U", "circumflex"), 0xDC: ("U", "diaeresis"),
    0xD1: ("N", "tilde"), 0xC7: ("C", "cedilla"),
}

# mono glyphs are 8 x 16, caps rows 2..11, x-height rows 5..11, descenders rows 12..15.
HAND = {
    "mono": {
        0xDF: ["........", "........", ".####...", "#....#..", "#....#..", "#....#..",
               ".####...", "#....#..", "#....#..", "#....#..", "#...##..", ".###.#..",
               "........", "........", "........", "........"],
        0xB4: ["........", "........", "....#...", "...#....", "........", "........",
               "........", "........", "........", "........", "........", "........",
               "........", "........", "........", "........"],
        0xA8: ["........", "........", "........", "..#..#..", "........", "........",
               "........", "........", "........", "........", "........", "........",
               "........", "........", "........", "........"],
    },
    # ui glyphs are 12 rows, caps rows 2..9, x-height rows 4..9, descenders rows 10..11
    "ui": {
        0xDF: ["......", ".#..#.", "#.#..#", "#.#..#", ".###..", "#.#..#", "#.#..#", "#.#..#",
               "#...##", ".##.##", "......", "......"],
        0xB4: ["...#", "..#.", "....", "....", "....", "....", "....", "....",
               "....", "....", "....", "...."],
        0xA8: ["#..#", "....", "....", "....", "....", "....", "....", "....",
               "....", "....", "....", "...."],
    },
}
# the hand-drawn punctuation designs join the hand-drawn accents
for font in ("mono", "ui"):
    HAND[font].update(hand_glyphs.EXTRA[font])

def mark_lines(mark, w, is_caps, is_mono):
    """The mark rows, placed where they belong for a letter of width w.

    Returns (top_rows, bottom_rows): rows to put above the letter and rows below it.
    """
    cx = w // 2
    if mark == "cedilla":
        d = w if is_mono else w
        top = []
        r1 = ["."] * d; r1[cx] = "#"
        r2 = ["."] * d; r2[max(0, cx - 1)] = "#"
        return top, ["".join(r1), "".join(r2)]
    # rows above: 2 for acute / grave / circumflex, 1 for diaeresis / tilde
    n = 2 if mark in ("acute", "grave", "circumflex") else 1
    # free space above the letter: mono caps 2 rows, mono lowercase 3, ui caps 2, ui lowercase 3
    free = 2 if is_caps else 3
    if is_mono:
        free = 2 if is_caps else 3
    gap = free - n
    lines = []
    for i in range(n):
        cols = []
        if mark == "acute":
            cols = [cx + 1 - (n - 1 - i), cx] if i == n - 1 else [cx + 1 - (n - 1 - i)]
        elif mark == "grave":
            cols = [cx + (n - 1 - i), cx] if i == n - 1 else [cx + (n - 1 - i)]
        elif mark == "circumflex":
            cols = [cx] if i == 0 else [cx - 1, cx + 1]
        elif mark == "diaeresis":
            cols = [cx - 1, cx + 1]
        elif mark == "tilde":
            cols = [cx - 2, cx - 1, cx + 1, cx + 2]
        width = w
        for c in cols:
            width = max(width, c + 1)
        row = ["."] * width
        for c in cols:
            if c >= 0:
                row[c] = "#"
        lines.append("".join(row))
    top = [""] * gap + lines
    return top, []

def combine(base_rows, w, is_caps, is_mono, mark):
    top, bottom = mark_lines(mark, w, is_caps, is_mono)
    width = w
    for r in top + bottom:
        width = max(width, len(r))
    body = [r + "." * (width - w) for r in base_rows]
    return [r + "." * (width - len(r)) for r in top] + body + [r + "." * (width - len(r)) for r in bottom]

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
    for name, is_mono in (("font_mono.txt", True), ("font_ui.txt", False)):
        path = os.path.join(SRC, name)
        base, glyphs = parse(path)
        rows_count = 16 if is_mono else 12
        for code, rows in HAND["mono" if is_mono else "ui"].items():
            assert len(rows) == rows_count, "%s: hand glyph has %d rows" % (name, len(rows))
            glyphs[code] = rows
        for code, (letter, mark) in ACCENTS.items():
            base_rows = glyphs[ord(letter)]
            w = len(base_rows[0])
            rows = combine(base_rows, w, letter.isupper(), is_mono, mark)
            if len(rows) > rows_count:
                # the mark pushed the letter down: take the rows that keep the letter at its
                # own height (the base glyph sits at the bottom of its cell)
                rows = rows[len(rows) - rows_count:]
            assert len(rows) == rows_count, "%s 0x%02X: %d rows" % (name, code, len(rows))
            glyphs[code] = rows
        out = list(base)
        if out and out[-1] != "":
            out.append("")
        out.append(MARK)
        out.append("; accented glyphs: the mark sits above the letter (2 rows for acute, grave,")
        out.append("; circumflex, 1 row for diaeresis and tilde) or below it (cedilla).")
        for code in sorted(glyphs):
            if code <= 126:
                continue
            out.append("@ 0x%02X" % code)
            out.extend(glyphs[code])
        with open(path, "w", encoding="ascii") as f:
            f.write("\n".join(out) + "\n")
        print("%s: %d glyphs" % (name, len(glyphs)))
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
