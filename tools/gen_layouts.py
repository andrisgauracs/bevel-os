#!/usr/bin/env python3
"""Write the keyboard layout tables from the key maps in tools/layouts/.

The source files are the key maps of the X keyboard config (xkeyboard-config), the
reference the Linux keyboards follow, copied from its symbol files into small
sections (xkb_latin.txt, xkb_layouts.txt, xkb_de_fr.txt). Each section lists what a
physical key prints, in four columns: plain, Shift, AltGr, AltGr+Shift.

The generator maps the X keyboard key names onto the USB HID usage codes the board
sees, keeps only the characters the Bevel OS fonts can draw, and writes
main/layout_gen.c (the tables declared in main/layout.h).

The key next to the left Shift (the ISO keyboards' extra key) carries the hash on
German and United Kingdom keyboards, so it gets the hash as its AltGr character.

Usage: python3 tools/gen_layouts.py [--check]
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "layouts")
OUT_C = os.path.normpath(os.path.join(HERE, "..", "main", "layout_gen.c"))

# X keyboard key name -> USB HID usage. The names name physical key positions,
# counted from the US keyboard: AD01 is the key US prints Q on, AC10 the key it
# prints ; on, and so on. LSGT is the extra key ISO keyboards have next to the
# left Shift (usage 0x31); BKSL is the extra key at the right of Enter, which
# keyboards report as usage 0x64.
POS = {}
POS["TLDE"] = 0x35
for i in range(10):
    POS["AE%02d" % (i + 1)] = 0x1E + i
POS["AE11"] = 0x2D
POS["AE12"] = 0x2E
# top letter row, left to right on a US keyboard
ROW1 = ["AD01", "AD02", "AD03", "AD04", "AD05", "AD06", "AD07", "AD08", "AD09", "AD10", "AD11", "AD12"]
ROW2 = ["AC01", "AC02", "AC03", "AC04", "AC05", "AC06", "AC07", "AC08", "AC09", "AC10", "AC11"]
ROW3 = ["AB01", "AB02", "AB03", "AB04", "AB05", "AB06", "AB07", "AB08", "AB09", "AB10"]
for i, n in enumerate(ROW1):
    POS[n] = [0x14, 0x1A, 0x08, 0x15, 0x17, 0x1C, 0x18, 0x0C, 0x12, 0x13, 0x2F, 0x30][i]
for i, n in enumerate(ROW2):
    POS[n] = [0x04, 0x16, 0x07, 0x09, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F, 0x33, 0x34][i]
for i, n in enumerate(ROW3):
    POS[n] = [0x1D, 0x1B, 0x06, 0x19, 0x05, 0x11, 0x10, 0x36, 0x37, 0x38][i]
POS["LSGT"] = 0x31
POS["BKSL"] = 0x64
POS["SPCE"] = 0x2C

# symbols the fonts have glyphs for, and the X keyboard names for them
SYMBOL = {}
for c in range(32, 127):
    ch = chr(c)
    SYMBOL[ch] = c
NAMES = {
    "grave": 0x60, "asciitilde": 0x7E, "asciicircum": 0x5E, "exclam": 0x21, "at": 0x40,
    "numbersign": 0x23, "dollar": 0x24, "percent": 0x25, "ampersand": 0x26, "asterisk": 0x2A,
    "parenleft": 0x28, "parenright": 0x29, "minus": 0x2D, "underscore": 0x5F, "equal": 0x3D,
    "plus": 0x2B, "bracketleft": 0x5B, "bracketright": 0x5D, "braceleft": 0x7B, "braceright": 0x7D,
    "backslash": 0x5C, "bar": 0x7C, "semicolon": 0x3B, "colon": 0x3A, "apostrophe": 0x27,
    "quotedbl": 0x22, "comma": 0x2C, "period": 0x2E, "slash": 0x2F, "question": 0x3F,
    "less": 0x3C, "greater": 0x3E, "space": 0x20,
    "agrave": 0xE0, "aacute": 0xE1, "acircumflex": 0xE2, "adiaeresis": 0xE4,
    "egrave": 0xE8, "eacute": 0xE9, "ecircumflex": 0xEA, "ediaeresis": 0xEB,
    "igrave": 0xEC, "iacute": 0xED, "icircumflex": 0xEE, "idiaeresis": 0xEF,
    "ograve": 0xF2, "oacute": 0xF3, "ocircumflex": 0xF4, "odiaeresis": 0xF6,
    "ugrave": 0xF9, "uacute": 0xFA, "ucircumflex": 0xFB, "udiaeresis": 0xFC,
    "ntilde": 0xF1, "ccedilla": 0xE7, "ssharp": 0xDF,
    "Agrave": 0xC0, "Aacute": 0xC1, "Acircumflex": 0xC2, "Adiaeresis": 0xC4,
    "Egrave": 0xC8, "Eacute": 0xC9, "Ecircumflex": 0xCA, "Ediaeresis": 0xCB,
    "Igrave": 0xCC, "Iacute": 0xCD, "Icircumflex": 0xCE, "Idiaeresis": 0xCF,
    "Ograve": 0xD2, "Oacute": 0xD3, "Ocircumflex": 0xD4, "Odiaeresis": 0xD6,
    "Ugrave": 0xD9, "Uacute": 0xDA, "Ucircumflex": 0xDB, "Udiaeresis": 0xDC,
    "Ntilde": 0xD1, "Ccedilla": 0xC7,
    # the punctuation the European keyboards print, and the X keyboard names for it
    "sterling": 0xA3, "section": 0xA7, "degree": 0xB0, "twosuperior": 0xB2, "threesuperior": 0xB3,
    "onesuperior": 0xB9, "mu": 0xB5, "exclamdown": 0xA1, "questiondown": 0xBF, "notsign": 0xAC,
    "onequarter": 0xBC, "onehalf": 0xBD, "threequarters": 0xBE, "cent": 0xA2, "plusminus": 0xB1,
    "brokenbar": 0xA6, "currency": 0xA4, "yen": 0xA5, "ae": 0xE6, "AE": 0xC3,
    "periodcentered": 0xB7, "guillemotleft": 0xAB, "guillemotright": 0xBB,
    "copyright": 0xA9, "registered": 0xAE, "division": 0xF7, "multiply": 0xD7,
}
DEAD = {"dead_circumflex": "circumflex", "dead_grave": "grave", "dead_acute": "acute",
        "dead_diaeresis": "diaeresis", "dead_tilde": "tilde"}
# the character a dead key prints on its own (press the dead key, then space)
DEAD_CHAR = {"circumflex": 0x5E, "grave": 0x60, "acute": 0xB4, "diaeresis": 0xA8, "tilde": 0x7E}
DEAD_CODE = {"": 0, "circumflex": 1, "grave": 2, "acute": 3, "diaeresis": 4, "tilde": 5}

ORDER = ["us", "gb", "de", "fr", "es", "ch_de", "ch_fr"]
DISPLAY = {"us": "US", "gb": "United Kingdom", "de": "German", "fr": "French",
           "es": "Spanish", "ch_de": "Swiss German", "ch_fr": "Swiss French"}
# which sections build each layout, in order: later sections win
BUILD = {
    "us": ["latin_basic", "us_basic"],
    "gb": ["latin_basic", "gb_basic"],
    "de": ["latin_basic", "latin_type4", "de_basic"],
    "fr": ["latin_basic", "fr_basic"],
    "es": ["latin_basic", "latin_type4", "es_basic"],
    "ch_de": ["latin_basic", "ch_basic"],
    "ch_fr": ["latin_basic", "ch_basic", "ch_fr"],
}

MIN_U, MAX_U = 4, 100   # usage 0x64 = 100 is the extra key right of Enter on ISO keyboards
GLYPHS = set()  # filled from the font source files in main()
SECTIONS = {}


class LayoutError(Exception):
    pass


def load_sections():
    for name in ("xkb_latin.txt", "xkb_layouts.txt", "xkb_de_fr.txt"):
        path = os.path.join(SRC, name)
        section = None
        try:
            with open(path, "r", encoding="utf-8") as f:
                text = f.read()
        except OSError as e:
            raise LayoutError(str(e))
        for line in text.split("\n"):
            s = line.strip()
            m = re.match(r'^xkb_symbols\s+"(\w+)"\s*\{$', s)
            if m:
                section = m.group(1)
                SECTIONS[section] = {}
                continue
            if s.startswith("};"):
                section = None
                continue
            if not s:
                continue
            m = re.match(r'^key\s+<(\w+)>\s*\{\s*\[(.*)\]\s*\};$', s)
            if not m:
                raise LayoutError("%s: cannot parse '%s'" % (path, s))
            if section is None:
                raise LayoutError("%s: key outside a section: %s" % (path, s))
            pos, syms = m.group(1), [x.strip() for x in m.group(2).split(",")]
            if pos not in POS:
                raise LayoutError("%s: unknown key name %s" % (path, pos))
            SECTIONS[section][pos] = syms[:4] + [""] * (4 - len(syms))


# the code points the Bevel OS fonts can draw: read from the font source files, so the
# layouts never ask for a glyph that does not exist
def font_glyphs():
    codes = set()
    for name in ("font_mono.txt", "font_ui.txt"):
        path = os.path.join(HERE, "assets", "src", name)
        try:
            with open(path, "r", encoding="utf-8") as f:
                for line in f:
                    if line.startswith("@"):
                        tok = line.split()[1]
                        codes.add(32 if tok == "space" else int(tok, 16) if tok.lower().startswith("0x") else ord(tok))
        except OSError as e:
            raise LayoutError(str(e))
    if not set(range(32, 127)) <= codes:
        raise LayoutError("the fonts are missing ASCII glyphs")
    return codes


def symbol(sym):
    """(code, dead) for one X keyboard symbol name, (0, "") when the fonts cannot draw it."""
    if sym in DEAD:
        name = DEAD[sym]
        return DEAD_CHAR[name], name
    if sym in NAMES:
        code = NAMES[sym]
        return (code, "") if code in GLYPHS else (0, "")
    if len(sym) == 1 and ord(sym) in GLYPHS:
        return ord(sym), ""
    return 0, ""


def build_layout(name):
    cp = [[0] * 4 for _ in range(MAX_U + 1)]
    dead = [[0] * 4 for _ in range(MAX_U + 1)]
    for section in BUILD[name]:
        if section not in SECTIONS:
            raise LayoutError("%s: no section %s" % (name, section))
        for pos, syms in SECTIONS[section].items():
            u = POS[pos]
            for i, sym in enumerate(syms):
                code, d = symbol(sym)
                # a later section replaces the key, so the state is set and not added on
                cp[u][i] = code
                dead[u][i] = DEAD_CODE[d]
    # the space bar prints a space on every keyboard
    cp[0x2C] = [0x20, 0x20, 0x20, 0x20]
    for u in range(MIN_U, MAX_U + 1):
        for i in range(4):
            if cp[u][i] and cp[u][i] not in GLYPHS:
                raise LayoutError("%s: usage 0x%02X state %d has no glyph (0x%02X)" % (name, u, i, cp[u][i]))
    return cp, dead


def emit(layouts):
    o = ["// GENERATED FILE - do not edit. Source: tools/layouts/, generator: tools/gen_layouts.py",
         '#include "layout.h"', ""]
    for i, name in enumerate(ORDER):
        cp, dead = layouts[i]
        o.append("// %s (%s)" % (DISPLAY[name], name))
        o.append("static const uint16_t %s_cp[%d][4] = {" % (name, MAX_U + 1))
        for u in range(0, MAX_U + 1):   # the row index is the HID usage, so low usages pad with zeros
            shown = "".join(chr(c) if c else "." for c in cp[u]).replace("\\", "B")  # keep the comment safe
            comment = "" if u < MIN_U else " // 0x%02X %s" % (u, shown)
            o.append("    {%d,%d,%d,%d},%s" % (cp[u][0], cp[u][1], cp[u][2], cp[u][3], comment))
        o.append("};")
        o.append("static const uint16_t %s_dead[%d][4] = {" % (name, MAX_U + 1))
        for u in range(0, MAX_U + 1):
            o.append("    {%d,%d,%d,%d}," % tuple(dead[u]))
        o.append("};")
    o.append("")
    o.append("const uint16_t (*const layout_cp[layout_count])[4] = { %s };" %
            ", ".join("%s_cp" % n for n in ORDER))
    o.append("const uint16_t (*const layout_dead[layout_count])[4] = { %s };" %
            ", ".join("%s_dead" % n for n in ORDER))
    o.append("const char *const layout_name[layout_count] = { %s };" %
            ", ".join('"%s"' % DISPLAY[n] for n in ORDER))
    o.append("")
    return "\n".join(o)


def main(argv):
    global GLYPHS
    GLYPHS = font_glyphs()
    load_sections()
    layouts = [build_layout(n) for n in ORDER]
    if "--check" in argv:
        for name, (cp, dead) in zip(ORDER, layouts):
            marks = sum(1 for row in cp for c in row if c)
            n_dead = sum(1 for row in dead for d in row if d)
            print("%-8s %d characters, %d dead keys" % (name, marks, n_dead))
        return 0
    with open(OUT_C, "w", encoding="utf-8") as f:
        f.write(emit(layouts))
    print("wrote %s (%d layouts)" % (os.path.relpath(OUT_C), len(layouts)))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except LayoutError as e:
        sys.stderr.write("layout error: %s\n" % e)
        sys.exit(1)
