// The drawing primitives, written once for a pixel type and compiled by gfx.c for each one it needs.
// Defined by the includer: PX (the pixel type), FN(name), HOT, FROM8(p) (a palette byte as a PX),
// PX_FILL(d, c, n), PX_HLINE(d, c, n) and PX_COPY(d, s, n), all counting pixels.
#define ROWPTR(g, wx, wy) ((PX *)(g)->px + ((wy) - (g)->sy) * (g)->stride + ((wx) - (g)->sx))

void HOT FN(gfx_fill)(gfx_t *g, int x, int y, int w, int h, PX c)
{
    if (!clip_rect(g, &x, &y, &w, &h)) return;
    PX *d = ROWPTR(g, x, y);
    for (; h > 0; h--, d += g->stride) PX_FILL(d, c, w);
}

void HOT FN(gfx_hline)(gfx_t *g, int x, int y, int w, PX c)
{
    int h = 1;
    if (!clip_rect(g, &x, &y, &w, &h)) return;
    PX_HLINE(ROWPTR(g, x, y), c, w);
}

void HOT FN(gfx_vline)(gfx_t *g, int x, int y, int h, PX c)
{
    int w = 1;
    if (!clip_rect(g, &x, &y, &w, &h)) return;
    PX *d = ROWPTR(g, x, y);
    for (; h > 0; h--, d += g->stride) *d = c;
}

void FN(gfx_frame)(gfx_t *g, int x, int y, int w, int h, PX c)
{
    if (w <= 0 || h <= 0) return;
    FN(gfx_hline)(g, x, y, w, c);
    FN(gfx_hline)(g, x, y + h - 1, w, c);
    FN(gfx_vline)(g, x, y + 1, h - 2, c);
    FN(gfx_vline)(g, x + w - 1, y + 1, h - 2, c);
}

void FN(gfx_bevel)(gfx_t *g, int x, int y, int w, int h, PX tl, PX br)
{
    if (w <= 0 || h <= 0) return;
    FN(gfx_hline)(g, x, y, w - 1, tl);
    FN(gfx_vline)(g, x, y + 1, h - 2, tl);
    FN(gfx_hline)(g, x, y + h - 1, w, br);
    FN(gfx_vline)(g, x + w - 1, y, h - 1, br);
}

void HOT FN(gfx_pixel)(gfx_t *g, int x, int y, PX c)
{
    x += g->ox; y += g->oy;
    if (x < g->cx0 || x >= g->cx1 || y < g->cy0 || y >= g->cy1) return;
    *ROWPTR(g, x, y) = c;
}

void FN(gfx_line)(gfx_t *g, int x0, int y0, int x1, int y1, PX c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        FN(gfx_pixel)(g, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void HOT FN(gfx_checker)(gfx_t *g, int x, int y, int w, int h, PX c0, PX c1)
{
    if (!clip_rect(g, &x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        PX *d = ROWPTR(g, x, y + j);
        int ph = (x + y + j) & 1;
        for (int i = 0; i < w; i++) d[i] = ((i + ph) & 1) ? c1 : c0;
    }
}

void FN(gfx_dotted)(gfx_t *g, int x, int y, int w, int h, PX c)
{
    for (int i = 0; i < w; i++) {
        if (((x + i + y) & 1) == 0) FN(gfx_pixel)(g, x + i, y, c);
        if (((x + i + y + h - 1) & 1) == 0) FN(gfx_pixel)(g, x + i, y + h - 1, c);
    }
    for (int j = 1; j < h - 1; j++) {
        if (((x + y + j) & 1) == 0) FN(gfx_pixel)(g, x, y + j, c);
        if (((x + w - 1 + y + j) & 1) == 0) FN(gfx_pixel)(g, x + w - 1, y + j, c);
    }
}

void HOT FN(gfx_bitmap)(gfx_t *g, int x, int y, const bitmap_t *b)
{
    int w = b->w, h = b->h;
    int lx = x + g->ox, ly = y + g->oy;
    if (!clip_rect(g, &x, &y, &w, &h)) return;
    const uint8_t *s = b->px + (y - ly) * b->w + (x - lx);
    PX *d = ROWPTR(g, x, y);
    for (; h > 0; h--, s += b->w, d += g->stride) {
        for (int i = 0; i < w; i++) {
            uint8_t p = s[i];
            if (p != 0xFF) d[i] = FROM8(p);
        }
    }
}

void FN(gfx_bitmap_tint)(gfx_t *g, int x, int y, const bitmap_t *b, PX c)
{
    int w = b->w, h = b->h;
    int lx = x + g->ox, ly = y + g->oy;
    if (!clip_rect(g, &x, &y, &w, &h)) return;
    const uint8_t *s = b->px + (y - ly) * b->w + (x - lx);
    PX *d = ROWPTR(g, x, y);
    for (int j = 0; j < h; j++, s += b->w, d += g->stride) {
        for (int i = 0; i < w; i++) {
            uint8_t p = s[i];
            if (p != 0xFF) d[i] = ((x + i + y + j) & 1) ? c : FROM8(p);
        }
    }
}

void HOT FN(gfx_blit)(gfx_t *g, int x, int y, int w, int h, const PX *src, int src_stride)
{
    int lx = x + g->ox, ly = y + g->oy;
    if (!clip_rect(g, &x, &y, &w, &h)) return;
    const PX *s = src + (y - ly) * src_stride + (x - lx);
    PX *d = ROWPTR(g, x, y);
    for (; h > 0; h--, s += src_stride, d += g->stride) PX_COPY(d, s, w);
}

// Draw one row of up to 16 pixels; bit 15 of 'bits' is the pixel at world x.
static inline void HOT FN(put_bits)(const gfx_t *g, PX *row, int x, uint32_t bits, PX c)
{
    // mask off pixels outside the clip
    if (x < g->cx0) {
        int cut = g->cx0 - x;
        if (cut >= 16) return;
        bits &= 0xFFFFu >> cut;
    }
    if (x + 16 > g->cx1) {
        int keep = g->cx1 - x;
        if (keep <= 0) return;
        bits &= ~(0xFFFFu >> keep);
    }
    PX *d = row + (x - g->sx);
    while (bits) {
        int i = __builtin_clz(bits) - 16;
        d[i] = c;
        bits &= ~(0x8000u >> i);
    }
}

void HOT FN(gfx_glyph16)(gfx_t *g, int x, int y, const uint16_t *rows, int w, int h, PX c)
{
    x += g->ox; y += g->oy;
    if (x >= g->cx1 || x + 16 <= g->cx0) return;
    int j0 = y < g->cy0 ? g->cy0 - y : 0;
    int j1 = y + h > g->cy1 ? g->cy1 - y : h;
    (void)w;
    for (int j = j0; j < j1; j++) {
        uint32_t bits = rows[j];
        if (bits) FN(put_bits)(g, (PX *)g->px + (y + j - g->sy) * g->stride, x, bits, c);
    }
}

// draw one glyph at (wx, wy) in the canvas, and return how far the pen moves
static int FN(glyph_draw)(gfx_t *g, int wx, int wy, const char *s, int len, int i, int font,
                      PX fg, bool bold, bool underline, int *span)
{
    int gi = gfx_glyph(s, i, len, span);
    int adv = glyph_adv(gi, font, bold);
    int h = gfx_font_h(font);
    if (wy >= g->cy1 || wy + h <= g->cy0) return adv;
    int j0 = wy < g->cy0 ? g->cy0 - wy : 0;
    int j1 = wy + h > g->cy1 ? g->cy1 - wy : h;
    PX *row0 = (PX *)g->px + (wy - g->sy) * g->stride;
    if (font == FONT_MONO) {
        if (wx + 8 > g->cx0) {
            const uint8_t *gl = font_mono[gi];
            for (int j = j0; j < j1; j++) {
                uint32_t bits = (uint32_t)gl[j] << 8;
                if (bits) FN(put_bits)(g, row0 + j * g->stride, wx, bits, fg);
            }
        }
    } else if (wx < g->cx1 && wx + 16 > g->cx0) {
        const uint16_t *gl = font_ui_rows[gi];
        for (int j = j0; j < j1; j++) {
            uint32_t bits = gl[j];
            if (bold) bits |= bits >> 1;
            if (bits) FN(put_bits)(g, row0 + j * g->stride, wx, bits, fg);
        }
    }
    if (underline) FN(gfx_hline)(g, wx - g->ox, wy - g->oy + (font == FONT_MONO ? 14 : FONT_UI_BASELINE + 2), adv - 1, fg);
    return adv;
}

int HOT FN(gfx_text)(gfx_t *g, int x, int y, const char *s, int len, int font, PX fg)
{
    if (len < 0) len = strlen(s);
    int wx = x + g->ox, wy = y + g->oy, start = wx;
    if (wy >= g->cy1 || wy + gfx_font_h(font) <= g->cy0) return gfx_text_w(s, len, font);
    bool bold = font == FONT_UI_BOLD;
    for (int i = 0; i < len;) {
        int n;
        wx += FN(glyph_draw)(g, wx, wy, s, len, i, font, fg, bold, false, &n);
        i += n;
    }
    return wx - start;
}

int FN(gfx_label)(gfx_t *g, int x, int y, const char *s, int font, PX fg, bool underline)
{
    int len = strlen(s), x0 = x;
    for (int i = 0; i < len;) {
        bool mn = false;
        if (s[i] == '&') { i++; if (i >= len) break; mn = s[i] != '&'; }
        int n;
        x += FN(glyph_draw)(g, x + g->ox, y + g->oy, s, len, i, font, fg, font == FONT_UI_BOLD,
                        mn && underline, &n);
        i += n;
    }
    return x - x0;
}

#undef ROWPTR
