// 2D renderer. Hot paths live in IRAM. The drawing primitives themselves are in gfx_draw.inc.
#include <string.h>
#include "gfx.h"
#include "esp_attr.h"

bool rect_intersect(rect_t *out, const rect_t *a, const rect_t *b)
{
    int x0 = a->x > b->x ? a->x : b->x;
    int y0 = a->y > b->y ? a->y : b->y;
    int x1 = a->x + a->w < b->x + b->w ? a->x + a->w : b->x + b->w;
    int y1 = a->y + a->h < b->y + b->h ? a->y + a->h : b->y + b->h;
    if (x1 <= x0 || y1 <= y0) { out->x = out->y = out->w = out->h = 0; return false; }
    out->x = x0; out->y = y0; out->w = x1 - x0; out->h = y1 - y0;
    return true;
}

void rect_union(rect_t *out, const rect_t *a, const rect_t *b)
{
    if (rect_empty(a)) { *out = *b; return; }
    if (rect_empty(b)) { *out = *a; return; }
    int x0 = a->x < b->x ? a->x : b->x;
    int y0 = a->y < b->y ? a->y : b->y;
    int x1 = a->x + a->w > b->x + b->w ? a->x + a->w : b->x + b->w;
    int y1 = a->y + a->h > b->y + b->h ? a->y + a->h : b->y + b->h;
    out->x = x0; out->y = y0; out->w = x1 - x0; out->h = y1 - y0;
}

// ---------------------------------------------------------------- kernels

#if HAS_PIE
// 128-bit PIE moves: the VGA scan-out reads the framebuffer straight out of PSRAM, so filling it
// fast enough is what decides whether the picture underruns (docs/PERF.md).
void IRAM_ATTR k_copy(uint8_t *dst, const uint8_t *src, int n)
{
    if (n >= 64 && (((uintptr_t)dst ^ (uintptr_t)src) & 15) == 0) {
        int head = (16 - ((uintptr_t)dst & 15)) & 15;
        if (head) { memcpy(dst, src, head); dst += head; src += head; n -= head; }
        int blocks = n >> 4;
        pie_copy16(dst, src, blocks);
        dst += blocks << 4; src += blocks << 4; n &= 15;
    }
    if (n) memcpy(dst, src, n);
}

void IRAM_ATTR k_fill(uint8_t *dst, uint8_t c, int n)
{
    if (n >= 64) {
        int head = (16 - ((uintptr_t)dst & 15)) & 15;
        if (head) { memset(dst, c, head); dst += head; n -= head; }
        int blocks = n >> 4;
        pie_fill16(dst, c, blocks);
        dst += blocks << 4; n &= 15;
    }
    // short tail / short runs
    while (n >= 4) { uint32_t v = c * 0x01010101u; memcpy(dst, &v, 4); dst += 4; n -= 4; }
    while (n-- > 0) *dst++ = c;
}
#else
void IRAM_ATTR k_copy(uint8_t *dst, const uint8_t *src, int n) { memcpy(dst, src, n); }
void IRAM_ATTR k_fill(uint8_t *dst, uint8_t c, int n) { memset(dst, c, n); }
#endif

// ---------------------------------------------------------------- context

void gfx_init(gfx_t *g, void *px, int stride, int sx, int sy, int w, int h)
{
    g->px = px; g->stride = stride; g->sx = sx; g->sy = sy;
    g->cx0 = sx; g->cy0 = sy; g->cx1 = sx + w; g->cy1 = sy + h;
    g->ox = 0; g->oy = 0;
}

bool gfx_clip(gfx_t *g, int x, int y, int w, int h)
{
    x += g->ox; y += g->oy;
    if (x > g->cx0) g->cx0 = x;
    if (y > g->cy0) g->cy0 = y;
    if (x + w < g->cx1) g->cx1 = x + w;
    if (y + h < g->cy1) g->cy1 = y + h;
    return g->cx1 > g->cx0 && g->cy1 > g->cy0;
}

bool gfx_visible(const gfx_t *g, int x, int y, int w, int h)
{
    x += g->ox; y += g->oy;
    return x < g->cx1 && y < g->cy1 && x + w > g->cx0 && y + h > g->cy0;
}

// Clip a local rectangle to world coordinates. Returns false if empty.
static inline bool clip_rect(const gfx_t *g, int *x, int *y, int *w, int *h)
{
    int x0 = *x + g->ox, y0 = *y + g->oy;
    int x1 = x0 + *w, y1 = y0 + *h;
    if (x0 < g->cx0) x0 = g->cx0;
    if (y0 < g->cy0) y0 = g->cy0;
    if (x1 > g->cx1) x1 = g->cx1;
    if (y1 > g->cy1) y1 = g->cy1;
    if (x1 <= x0 || y1 <= y0) return false;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return true;
}


// ---------------------------------------------------------------- text

int gfx_font_h(int font) { return font == FONT_MONO ? 16 : FONT_UI_H; }

// Text is UTF-8, and the fonts cover ASCII plus the accented letters the keyboard layouts
// type. One glyph is one byte (ASCII) or two bytes (the accents), so text walks by glyphs:
// gfx_glyph gives the row of the font tables to draw and how many bytes the glyph is, and
// gfx_glyph_back how many bytes the glyph before a caret spans, so Backspace deletes a
// whole accented letter and not half of it.
int gfx_glyph(const char *s, int i, int len, int *bytes)
{
    unsigned char c = (unsigned char)s[i];
    int row;
    if (c < 0x80) {
        *bytes = 1;
        row = font_glyph_index[c];
    } else if ((c & 0xE0) == 0xC0 && i + 1 < len) {
        unsigned cp = ((c & 0x1F) << 6) | ((unsigned char)s[i + 1] & 0x3F);
        *bytes = 2;
        row = cp <= 255 ? font_glyph_index[cp] : 0xFFFF;
    } else {
        *bytes = 1;
        row = 0xFFFF;
    }
    return row == 0xFFFF ? font_glyph_index['?'] : row;
}

int gfx_glyphs(const char *s, int len)
{
    if (len < 0) len = strlen(s);
    int n = 0;
    for (int i = 0; i < len;) {
        int b;
        gfx_glyph(s, i, len, &b);
        i += b;
        n++;
    }
    return n;
}

int gfx_glyph_back(const char *s, int pos)
{
    if (pos >= 2 && (s[pos - 2] & 0xC0) == 0xC0) return 2;
    return 1;
}

static inline int glyph_adv(int row, int font, bool bold)
{
    return (font == FONT_MONO ? 8 : font_ui_adv[row]) + (bold ? 1 : 0);
}

int IRAM_ATTR gfx_text_w(const char *s, int len, int font)
{
    if (len < 0) len = strlen(s);
    bool bold = font == FONT_UI_BOLD;
    int w = 0;
    for (int i = 0; i < len;) {
        int n;
        w += glyph_adv(gfx_glyph(s, i, len, &n), font, bold);
        i += n;
    }
    return w;
}

int gfx_label_w(const char *s, int font)
{
    int len = strlen(s), w = 0;
    for (int i = 0; i < len;) {
        if (s[i] == '&') { i++; if (i >= len) break; }
        int n;
        w += glyph_adv(gfx_glyph(s, i, len, &n), font, font == FONT_UI_BOLD);
        i += n;
    }
    return w;
}

char gfx_label_mnemonic(const char *s)
{
    for (; *s; s++) {
        if (*s == '&') {
            s++;
            if (!*s) break;
            if (*s != '&') return (*s >= 'A' && *s <= 'Z') ? *s + 32 : *s;
        }
    }
    return 0;
}

// ---------------------------------------------------------------- primitives

#if DISP_TRUE_COLOR
static inline void IRAM_ATTR fill16(uint16_t *d, uint16_t c, int n)
{
    if ((c >> 8) == (c & 0xFF)) { memset(d, c & 0xFF, n * 2); return; }     // black, white
    if (n >= 8) {
        if ((uintptr_t)d & 2) { *d++ = c; n--; }
        uint32_t v = c * 0x10001u, *p = (uint32_t *)d;
        for (; n >= 16; n -= 16, p += 8) { p[0] = v; p[1] = v; p[2] = v; p[3] = v; p[4] = v; p[5] = v; p[6] = v; p[7] = v; }
        for (; n >= 2; n -= 2) *p++ = v;
        d = (uint16_t *)p;
    }
    while (n-- > 0) *d++ = c;
}

// the screen's own pixels
#define PX uint16_t
#define FN(name) name
#define HOT IRAM_ATTR
#define FROM8(p) disp_rgb222_lut[p]
#define PX_FILL(d, c, n) fill16((d), (c), (n))
#define PX_HLINE(d, c, n) fill16((d), (c), (n))
#define PX_COPY(d, s, n) memcpy((d), (s), (size_t)(n) * 2)
#include "gfx_draw.inc"
#undef PX
#undef FN
#undef HOT
#undef FROM8
#undef PX_FILL
#undef PX_HLINE
#undef PX_COPY

// A surface of palette bytes (a program's canvas) shown on the screen
void IRAM_ATTR gfx_blit_rgb222(gfx_t *g, int x, int y, int w, int h, const uint8_t *src, int src_stride)
{
    int lx = x + g->ox, ly = y + g->oy;
    if (!clip_rect(g, &x, &y, &w, &h)) return;
    const uint8_t *s = src + (y - ly) * src_stride + (x - lx);
    uint16_t *d = (uint16_t *)g->px + (y - g->sy) * g->stride + (x - g->sx);
    const uint16_t *lut = disp_rgb222_lut;
    for (; h > 0; h--, s += src_stride, d += g->stride)
        for (int i = 0; i < w; i++) d[i] = lut[s[i]];
}
#define FN(name) name##8
#define HOT
#else
#define FN(name) name
#define HOT IRAM_ATTR
#endif

// palette bytes: every surface of the S3, and the canvases of loadable programs on the P4
#define PX uint8_t
#define FROM8(p) (p)
#define PX_FILL(d, c, n) do { if ((n) >= 64) k_fill((d), (c), (n)); else memset((d), (c), (n)); } while (0)
#define PX_HLINE(d, c, n) memset((d), (c), (n))
#define PX_COPY(d, s, n) k_copy((d), (s), (n))
#include "gfx_draw.inc"
