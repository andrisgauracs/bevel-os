#!/usr/bin/env python3
"""Bevel OS asset generator.

Reads the human-editable ASCII-art sources in tools/assets/src/ and writes
main/assets/assets_gen.c (the tables declared in main/assets/assets.h).

Source formats (lines starting with ';' are comments, blank lines are ignored):

  font_mono.txt / font_ui.txt
      @ A            glyph header: a literal character, the word 'space', or 0xNN
      ..##..         one line per pixel row, '#' = ink, '.' = background
    font_mono: every glyph is exactly 8 columns x 16 rows.
    font_ui:   every glyph is 12 rows; the row length is the glyph width (1..15)
               and the advance is width + 1.

  icons16/*.txt, icons32/*.txt, cursors/*.txt
      size W H
      hot X Y        (optional, cursors)
      color C r g b  legend: character C = colour r,g,b (each 0..3)
      ---
      ....KKKK....   H rows of W characters, '.' = transparent

Usage:  python3 tools/assets/gen_assets.py [--check]
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "src")
OUT_C = os.path.normpath(os.path.join(HERE, "..", "..", "main", "assets", "assets_gen.c"))

FIRST, LAST = 32, 126
NGLYPH = LAST - FIRST + 1
MONO_W, MONO_H = 8, 16
UI_H, UI_BASELINE, UI_MAX_W = 12, 9, 15
TRANSPARENT = 0xFF

# Names required by assets.h. Extra files in the folders are emitted too.
ICON16 = ["folder", "file", "text", "image", "app", "game", "drive", "settings", "computer",
          "terminal", "help", "shutdown", "logo", "tasks", "up", "camera", "sound"]
ICON32 = ["folder", "file", "text", "image", "app", "game", "drive", "settings", "computer",
          "terminal", "help", "shutdown", "logo", "tasks", "info", "warning", "error", "question", "camera", "sound"]
CURSORS = ["arrow", "ibeam", "busy", "size_we", "size_ns", "size_nwse", "move"]
CURSOR_MAX_W, CURSOR_MAX_H = 16, 20


class AssetError(Exception):
    pass


def _read(path):
    """Yield (line_number, text) for meaningful lines."""
    try:
        with open(path, "r", encoding="ascii") as f:
            raw = f.read().split("\n")
    except OSError as e:
        raise AssetError("%s: %s" % (path, e))
    for n, line in enumerate(raw, 1):
        line = line.rstrip()
        if not line or line.startswith(";"):
            continue
        yield n, line


def _glyph_code(tok, where):
    if tok == "space":
        return 32
    if len(tok) == 1:
        return ord(tok)
    if tok.lower().startswith("0x"):
        return int(tok, 16)
    raise AssetError("%s: bad glyph header '%s'" % (where, tok))


def parse_font(path, height, fixed_width=None, max_width=None):
    """Return {code: [row strings]} after validating the whole file."""
    glyphs = {}
    cur = None
    for n, line in _read(path):
        where = "%s:%d" % (os.path.relpath(path, HERE), n)
        if line.startswith("@"):
            parts = line.split()
            if len(parts) != 2:
                raise AssetError("%s: glyph header must be '@ <char>'" % where)
            code = _glyph_code(parts[1], where)
            if not FIRST <= code <= LAST:
                raise AssetError("%s: glyph code %d outside 32..126" % (where, code))
            if code in glyphs:
                raise AssetError("%s: duplicate glyph %r" % (where, chr(code)))
            cur = glyphs[code] = []
            continue
        if cur is None:
            raise AssetError("%s: pixel row before any '@' header" % where)
        if set(line) - set("#."):
            raise AssetError("%s: only '#' and '.' allowed in glyph rows" % where)
        cur.append(line)
    name = os.path.basename(path)
    missing = [chr(c) for c in range(FIRST, LAST + 1) if c not in glyphs]
    if missing:
        raise AssetError("%s: missing glyphs: %s" % (name, " ".join(repr(m) for m in missing)))
    for code in range(FIRST, LAST + 1):
        rows = glyphs[code]
        g = "%s glyph %r" % (name, chr(code))
        if len(rows) != height:
            raise AssetError("%s: has %d rows, expected %d" % (g, len(rows), height))
        w = len(rows[0])
        if any(len(r) != w for r in rows):
            raise AssetError("%s: rows have different lengths" % g)
        if fixed_width is not None and w != fixed_width:
            raise AssetError("%s: is %d wide, expected %d" % (g, w, fixed_width))
        if max_width is not None and not 1 <= w <= max_width:
            raise AssetError("%s: width %d outside 1..%d" % (g, w, max_width))
    return glyphs


def parse_bitmap(path):
    """Return dict(w, h, hot_x, hot_y, px=[bytes]) for one icon/cursor file."""
    name = os.path.relpath(path, HERE)
    size = None
    hot = (0, 0)
    legend = {".": TRANSPARENT}
    rows = []
    in_pixels = False
    for n, line in _read(path):
        where = "%s:%d" % (name, n)
        if in_pixels:
            rows.append((where, line))
            continue
        parts = line.split()
        try:
            if parts[0] == "size" and len(parts) == 3:
                size = (int(parts[1]), int(parts[2]))
            elif parts[0] == "hot" and len(parts) == 3:
                hot = (int(parts[1]), int(parts[2]))
            elif parts[0] == "color" and len(parts) == 5 and len(parts[1]) == 1:
                ch = parts[1]
                r, g, b = (int(v) for v in parts[2:5])
                if ch == "." or ch in legend:
                    raise AssetError("%s: legend character '%s' already defined" % (where, ch))
                if not all(0 <= v <= 3 for v in (r, g, b)):
                    raise AssetError("%s: colour channels must be 0..3" % where)
                legend[ch] = (b << 4) | (g << 2) | r
            elif line == "---":
                in_pixels = True
            else:
                raise AssetError("%s: cannot parse header line '%s'" % (where, line))
        except ValueError:
            raise AssetError("%s: bad number in '%s'" % (where, line))
    if size is None:
        raise AssetError("%s: missing 'size W H'" % name)
    w, h = size
    if not (1 <= w <= 255 and 1 <= h <= 255):
        raise AssetError("%s: bad size %dx%d" % (name, w, h))
    if len(rows) != h:
        raise AssetError("%s: has %d pixel rows, expected %d" % (name, len(rows), h))
    px = []
    used = set()
    for where, line in rows:
        if len(line) != w:
            raise AssetError("%s: row is %d wide, expected %d" % (where, len(line), w))
        for ch in line:
            if ch not in legend:
                raise AssetError("%s: character '%s' is not in the legend" % (where, ch))
            used.add(ch)
            px.append(legend[ch])
    unused = sorted(set(legend) - used - {"."})
    if unused:
        sys.stderr.write("note: %s: unused legend entries: %s\n" % (name, " ".join(unused)))
    if not (-128 <= hot[0] < w and -128 <= hot[1] < h and hot[0] >= 0 and hot[1] >= 0):
        raise AssetError("%s: hot spot %s outside the bitmap" % (name, hot))
    return {"w": w, "h": h, "hot_x": hot[0], "hot_y": hot[1], "px": px}


def _load_dir(sub, prefix, required, w=None, h=None, max_w=None, max_h=None):
    d = os.path.join(SRC, sub)
    try:
        names = sorted(f[:-4] for f in os.listdir(d) if f.endswith(".txt"))
    except OSError as e:
        raise AssetError(str(e))
    missing = [r for r in required if r not in names]
    if missing:
        raise AssetError("src/%s: missing required files: %s" % (sub, ", ".join(m + ".txt" for m in missing)))
    ordered = list(required) + [n for n in names if n not in required]
    out = []
    for n in ordered:
        if not n.replace("_", "").isalnum():
            raise AssetError("src/%s/%s.txt: file name must be a C identifier" % (sub, n))
        bm = parse_bitmap(os.path.join(d, n + ".txt"))
        if w is not None and (bm["w"], bm["h"]) != (w, h):
            raise AssetError("src/%s/%s.txt: is %dx%d, expected %dx%d" % (sub, n, bm["w"], bm["h"], w, h))
        if max_w is not None and (bm["w"] > max_w or bm["h"] > max_h):
            raise AssetError("src/%s/%s.txt: %dx%d exceeds %dx%d" % (sub, n, bm["w"], bm["h"], max_w, max_h))
        out.append((prefix + n, bm))
    return out


def load_all():
    """Parse and validate every source. Returns a dict used by the generator and preview."""
    mono_src = parse_font(os.path.join(SRC, "font_mono.txt"), MONO_H, fixed_width=MONO_W)
    ui_src = parse_font(os.path.join(SRC, "font_ui.txt"), UI_H, max_width=UI_MAX_W)
    mono = []
    for c in range(FIRST, LAST + 1):
        mono.append([sum(0x80 >> x for x, ch in enumerate(r) if ch == "#") for r in mono_src[c]])
    ui_adv, ui_rows = [], []
    for c in range(FIRST, LAST + 1):
        rows = ui_src[c]
        ui_adv.append(len(rows[0]) + 1)
        ui_rows.append([sum(0x8000 >> x for x, ch in enumerate(r) if ch == "#") for r in rows])
    bitmaps = []
    bitmaps += _load_dir("icons16", "icon16_", ICON16, w=16, h=16)
    bitmaps += _load_dir("icons32", "icon32_", ICON32, w=32, h=32)
    bitmaps += _load_dir("cursors", "cursor_", CURSORS, max_w=CURSOR_MAX_W, max_h=CURSOR_MAX_H)
    return {"mono": mono, "ui_adv": ui_adv, "ui_rows": ui_rows, "bitmaps": bitmaps}


def _cchar(c):
    ch = chr(c)
    return {"\\": "backslash", "/": "slash", "*": "asterisk"}.get(ch, ch)


def emit_c(a):
    o = []
    o.append("// GENERATED FILE - do not edit. Source: tools/assets/src/, generator: tools/assets/gen_assets.py")
    o.append('#include "assets.h"')
    o.append("")
    o.append("const uint8_t font_mono[%d][%d] = {" % (NGLYPH, MONO_H))
    for i, rows in enumerate(a["mono"]):
        o.append("    {%s}, // %s" % (",".join("0x%02X" % v for v in rows), _cchar(FIRST + i)))
    o.append("};")
    o.append("")
    o.append("const uint8_t font_ui_adv[%d] = {" % NGLYPH)
    for i in range(0, NGLYPH, 16):
        o.append("    %s," % ",".join("%d" % v for v in a["ui_adv"][i:i + 16]))
    o.append("};")
    o.append("")
    o.append("const uint16_t font_ui_rows[%d][FONT_UI_H] = {" % NGLYPH)
    for i, rows in enumerate(a["ui_rows"]):
        o.append("    {%s}, // %s" % (",".join("0x%04X" % v for v in rows), _cchar(FIRST + i)))
    o.append("};")
    for name, bm in a["bitmaps"]:
        w, h, px = bm["w"], bm["h"], bm["px"]
        o.append("")
        o.append("static const uint8_t %s_px[%d] = {" % (name, w * h))
        for y in range(h):
            o.append("    %s," % ",".join("0x%02X" % v for v in px[y * w:(y + 1) * w]))
        o.append("};")
        o.append("const bitmap_t %s = {%d, %d, %d, %d, %s_px};" % (name, w, h, bm["hot_x"], bm["hot_y"], name))
    o.append("")
    return "\n".join(o)


def main(argv):
    try:
        a = load_all()
    except AssetError as e:
        sys.stderr.write("asset error: %s\n" % e)
        return 1
    if "--check" in argv:
        print("ok: 2 fonts, %d bitmaps" % len(a["bitmaps"]))
        return 0
    os.makedirs(os.path.dirname(OUT_C), exist_ok=True)
    with open(OUT_C, "w", encoding="ascii") as f:
        f.write(emit_c(a))
    print("wrote %s (2 fonts, %d bitmaps)" % (os.path.relpath(OUT_C), len(a["bitmaps"])))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
