#!/usr/bin/env python3
"""Render a specimen sheet of all Bevel OS assets to tools/assets/out/preview.png.

Also writes one PNG per section (preview_mono.png, preview_ui.png, preview_icons16.png,
preview_icons32.png, preview_cursors.png) which are easier to inspect up close.
Stdlib only.  Usage: python3 tools/assets/preview.py
"""
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_assets  # noqa: E402

OUT = os.path.join(HERE, "out")
CHARSET = "".join(chr(c) for c in range(32, 127))
WORDS = "Ànaïs - Seúl  España? Ñoño l'après-midi über Zürich, café Crème brûlée"
PANGRAM = "The quick brown fox jumps over the lazy dog."
PANGRAM2 = "SPHINX OF BLACK QUARTZ, JUDGE MY VOW! 0123456789"
SAMPLES = [PANGRAM, PANGRAM2,
           "Illegal 1lI| O0o 5S 2Z 8B rn m vv w `'\" ,.;: -_=+~ (){}[]<> /\\ #$%&@*^?!",
           "for (int i = 0; i < n; i++) { buf[i] ^= 0x5A; }  // 100% done?"]
UI_SAMPLES = SAMPLES + ["File  Edit  View  Help      OK   Cancel   Apply      Shut Down...   My Files (D:)",
                        "Tabular: 1111111111 | 0000000000 | 1234567890 | 12:45 PM  3.14  640x480"]


def rgb(r, g, b):
    return (r * 85, g * 85, b * 85)


WHITE, BLACK, GREY, TEAL, NAVY = rgb(3, 3, 3), rgb(0, 0, 0), rgb(2, 2, 2), rgb(0, 2, 2), rgb(0, 0, 1)


class Canvas:
    def __init__(self, w, h, bg=WHITE):
        self.w, self.h = w, h
        self.buf = bytearray(bytes(bg) * (w * h))

    def rect(self, x, y, w, h, col):
        x0, y0, x1, y1 = max(0, x), max(0, y), min(self.w, x + w), min(self.h, y + h)
        if x1 <= x0:
            return
        row = bytes(col) * (x1 - x0)
        for yy in range(y0, y1):
            self.buf[(yy * self.w + x0) * 3:(yy * self.w + x1) * 3] = row

    def paste(self, other, x, y):
        for yy in range(other.h):
            self.buf[((y + yy) * self.w + x) * 3:((y + yy) * self.w + x + other.w) * 3] = \
                other.buf[yy * other.w * 3:(yy + 1) * other.w * 3]

    def save(self, path):
        raw = b"".join(b"\0" + bytes(self.buf[y * self.w * 3:(y + 1) * self.w * 3]) for y in range(self.h))

        def chunk(tag, data):
            return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        with open(path, "wb") as f:
            f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", self.w, self.h, 8, 2, 0, 0, 0)) +
                    chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def draw_mono(cv, a, x, y, text, scale=1, fg=BLACK):
    for ch in text:
        rows = a["mono"][a["index"][ord(ch)]]
        for ry, bits in enumerate(rows):
            for rx in range(8):
                if bits & (0x80 >> rx):
                    cv.rect(x + rx * scale, y + ry * scale, scale, scale, fg)
        x += 8 * scale
    return x


def ui_width(a, text, bold=False):
    return sum(a["ui_adv"][a["index"][ord(c)]] + (1 if bold else 0) for c in text)


def draw_ui(cv, a, x, y, text, scale=1, fg=BLACK, bold=False):
    for ch in text:
        i = a["index"][ord(ch)]
        for ry, bits in enumerate(a["ui_rows"][i]):
            if bold:
                bits |= bits >> 1  # the renderer's 1px right smear
            for rx in range(16):
                if bits & (0x8000 >> rx):
                    cv.rect(x + rx * scale, y + ry * scale, scale, scale, fg)
        x += (a["ui_adv"][i] + (1 if bold else 0)) * scale
    return x


def blit(cv, bm, x, y, scale=1):
    for i, p in enumerate(bm["px"]):
        if p == 0xFF:
            continue
        col = rgb(p & 3, (p >> 2) & 3, (p >> 4) & 3)
        cv.rect(x + (i % bm["w"]) * scale, y + (i // bm["w"]) * scale, scale, scale, col)


def chunks(s, n):
    return [s[i:i + n] for i in range(0, len(s), n)]


def sec_mono(a):
    extra = "".join(chr(c) for c in a["order"][gen_assets.NGLYPH:])
    cv = Canvas(1056, 16 * 9 + 48 * 8 + 60)
    y = 8
    for line in chunks(CHARSET, 48) + chunks(extra, 48) + [WORDS] + SAMPLES:
        draw_mono(cv, a, 8, y, line)
        y += 16
    cv.rect(0, y + 4, cv.w, 16 * 2 + 8, NAVY)
    draw_mono(cv, a, 8, y + 8, PANGRAM + " " + PANGRAM2[:38], fg=WHITE)
    draw_mono(cv, a, 8, y + 24, "C:\\> dir /w *.txt   [README.TXT]  {a|b} ~user 3<4>=2 x_y `q` it's \"ok\" #1 @ 50%", fg=rgb(0, 3, 0))
    y += 52
    for line in chunks(CHARSET, 32) + chunks(extra, 32) + [PANGRAM[:43], PANGRAM2[:43], "0O 1lI| 5S 2Z 8B g9q rn m {[(<>)]} ;:,.'`\"!?"]:
        draw_mono(cv, a, 8, y, line, 3)
        y += 48
    return cv


def sec_ui(a):
    extra = "".join(chr(c) for c in a["order"][gen_assets.NGLYPH:])
    cv = Canvas(1056, 14 * 20 + 40 * 10 + 40)
    y = 8
    for bold in (False, True):
        for line in chunks(CHARSET, 48) + chunks(extra, 48) + [WORDS] + UI_SAMPLES:
            draw_ui(cv, a, 8, y, line, bold=bold)
            y += 14
    # on dark title bar + grey button
    cv.rect(0, y + 2, 400, 18, NAVY)
    draw_ui(cv, a, 8, y + 5, "Bevel OS - Untitled.txt [Text Editor]", fg=WHITE, bold=True)
    cv.rect(410, y + 2, 300, 18, GREY)
    draw_ui(cv, a, 418, y + 5, "Programs   Settings   Help   Shut Down...")
    y += 26
    for bold in (False, True):
        for line in chunks(CHARSET, 32) + chunks(extra, 32):
            draw_ui(cv, a, 8, y, line, 3, bold=bold)
            y += 40
    draw_ui(cv, a, 8, y, PANGRAM, 3)
    y += 40
    draw_ui(cv, a, 8, y, "Bevel OS 0123456789 Shut Down?", 3, bold=True)
    return cv


def sec_bitmaps(a, prefix, per_row, scale):
    items = [(n, b) for n, b in a["bitmaps"] if n.startswith(prefix)]
    cw = max(b["w"] for _, b in items)
    ch = max(b["h"] for _, b in items)
    cell_w, cell_h = cw * scale + 12, ch * scale + ch + 30
    nrows = (len(items) + per_row - 1) // per_row
    cv = Canvas(per_row * cell_w * 2 + 12, nrows * cell_h + 8)
    for half, bg in enumerate((GREY, TEAL)):
        cv.rect(half * (per_row * cell_w + 6), 0, per_row * cell_w + 6, cv.h, bg)
        for i, (name, bm) in enumerate(items):
            x = half * (per_row * cell_w + 6) + 8 + (i % per_row) * cell_w
            y = 8 + (i // per_row) * cell_h
            blit(cv, bm, x, y, scale)
            blit(cv, bm, x, y + ch * scale + 6, 1)
            draw_ui(cv, a, x + cw + 6, y + ch * scale + 6, name[len(prefix):], fg=BLACK if half == 0 else WHITE)
            if prefix == "cursor_":  # mark the hot spot
                cv.rect(x + bm["hot_x"] * scale + scale // 2 - 1, y + bm["hot_y"] * scale + scale // 2 - 1, 2, 2, rgb(3, 0, 0))
    return cv


def main():
    try:
        a = gen_assets.load_all()
    except gen_assets.AssetError as e:
        sys.stderr.write("asset error: %s\n" % e)
        return 1
    os.makedirs(OUT, exist_ok=True)
    secs = [("mono", sec_mono(a)), ("ui", sec_ui(a)),
            ("icons16", sec_bitmaps(a, "icon16_", 8, 4)),
            ("icons32", sec_bitmaps(a, "icon32_", 6, 4)),
            ("cursors", sec_bitmaps(a, "cursor_", 7, 4))]
    for name, cv in secs:
        cv.save(os.path.join(OUT, "preview_%s.png" % name))
    w = max(cv.w for _, cv in secs)
    full = Canvas(w, sum(cv.h + 8 for _, cv in secs), rgb(1, 1, 1))
    y = 0
    for _, cv in secs:
        full.paste(cv, 0, y)
        y += cv.h + 8
    full.save(os.path.join(OUT, "preview.png"))
    print("wrote %s (%dx%d) and per-section PNGs" % (os.path.join(OUT, "preview.png"), full.w, full.h))
    return 0


if __name__ == "__main__":
    sys.exit(main())
