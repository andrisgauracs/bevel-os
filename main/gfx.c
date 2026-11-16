// 2D renderer. Hot paths live in IRAM and write whole words / 128-bit PIE vectors.
#include <string.h>
#include "gfx.h"
#include "esp_attr.h"
#include "sdkconfig.h"

#if !CONFIG_BEVEL_PIE
// Portable versions of the PIE kernels in pie.S (emulators, the desktop simulator).
void pie_copy16(void *dst, const void *src, int blocks) { memcpy(dst, src, (size_t)blocks << 4); }
void pie_fill16(void *dst, int c, int blocks) { memset(dst, c, (size_t)blocks << 4); }
#endif

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

// ---------------------------------------------------------------- context

void gfx_init(gfx_t *g, uint8_t *px, int stride, int sx, int sy, int w, int h)
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

#define ROWPTR(g, wx, wy) ((g)->px + ((wy) - (g)->sy) * (g)->stride + ((wx) - (g)->sx))

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

// ---------------------------------------------------------------- primitives

void IRAM_ATTR gfx_fill(gfx_t *g, int x, int y, int w, int h, uint8_t c)
{
    if (!clip_rect(g, &x, &y, &w, &h)) return;
    uint8_t *d = ROWPTR(g, x, y);
    if (w >= 64) {
        for (; h > 0; h--, d += g->stride) k_fill(d, c, w);
    } else {
        for (; h > 0; h--, d += g->stride) memset(d, c, w);
    }
}

void IRAM_ATTR gfx_hline(gfx_t *g, int x, int y, int w, uint8_t c)
{
    int h = 1;
    if (!clip_rect(g, &x, &y, &w, &h)) return;
    memset(ROWPTR(g, x, y), c, w);
}

void IRAM_ATTR gfx_vline(gfx_t *g, int x, int y, int h, uint8_t c)
{
    int w = 1;
    if (!clip_rect(g, &x, &y, &w, &h)) return;
    uint8_t *d = ROWPTR(g, x, y);
    for (; h > 0; h--, d += g->stride) *d = c;
}

void gfx_frame(gfx_t *g, int x, int y, int w, int h, uint8_t c)
{
    if (w <= 0 || h <= 0) return;
    gfx_hline(g, x, y, w, c);
    gfx_hline(g, x, y + h - 1, w, c);
    gfx_vline(g, x, y + 1, h - 2, c);
    gfx_vline(g, x + w - 1, y + 1, h - 2, c);
}

void gfx_bevel(gfx_t *g, int x, int y, int w, int h, uint8_t tl, uint8_t br)
{
    if (w <= 0 || h <= 0) return;
    gfx_hline(g, x, y, w - 1, tl);
    gfx_vline(g, x, y + 1, h - 2, tl);
    gfx_hline(g, x, y + h - 1, w, br);
    gfx_vline(g, x + w - 1, y, h - 1, br);
}

void IRAM_ATTR gfx_pixel(gfx_t *g, int x, int y, uint8_t c)
{
    x += g->ox; y += g->oy;
    if (x < g->cx0 || x >= g->cx1 || y < g->cy0 || y >= g->cy1) return;
    *ROWPTR(g, x, y) = c;
}

void gfx_line(gfx_t *g, int x0, int y0, int x1, int y1, uint8_t c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx_pixel(g, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void IRAM_ATTR gfx_checker(gfx_t *g, int x, int y, int w, int h, uint8_t c0, uint8_t c1)
{
    if (!clip_rect(g, &x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        uint8_t *d = ROWPTR(g, x, y + j);
        int ph = (x + y + j) & 1;
        for (int i = 0; i < w; i++) d[i] = ((i + ph) & 1) ? c1 : c0;
    }
}

void gfx_dotted(gfx_t *g, int x, int y, int w, int h, uint8_t c)
{
    for (int i = 0; i < w; i++) {
        if (((x + i + y) & 1) == 0) gfx_pixel(g, x + i, y, c);
        if (((x + i + y + h - 1) & 1) == 0) gfx_pixel(g, x + i, y + h - 1, c);
    }
    for (int j = 1; j < h - 1; j++) {
        if (((x + y + j) & 1) == 0) gfx_pixel(g, x, y + j, c);
        if (((x + w - 1 + y + j) & 1) == 0) gfx_pixel(g, x + w - 1, y + j, c);
    }
}

void IRAM_ATTR gfx_bitmap(gfx_t *g, int x, int y, const bitmap_t *b)
{
    int w = b->w, h = b->h;
    int lx = x + g->ox, ly = y + g->oy;
    if (!clip_rect(g, &x, &y, &w, &h)) return;
    const uint8_t *s = b->px + (y - ly) * b->w + (x - lx);
    uint8_t *d = ROWPTR(g, x, y);
    for (; h > 0; h--, s += b->w, d += g->stride) {
        for (int i = 0; i < w; i++) {
            uint8_t p = s[i];
            if (p != 0xFF) d[i] = p;
        }
    }
}

void gfx_bitmap_tint(gfx_t *g, int x, int y, const bitmap_t *b, uint8_t c)
{
    int w = b->w, h = b->h;
    int lx = x + g->ox, ly = y + g->oy;
    if (!clip_rect(g, &x, &y, &w, &h)) return;
    const uint8_t *s = b->px + (y - ly) * b->w + (x - lx);
    uint8_t *d = ROWPTR(g, x, y);
    for (int j = 0; j < h; j++, s += b->w, d += g->stride) {
        for (int i = 0; i < w; i++) {
            uint8_t p = s[i];
            if (p != 0xFF) d[i] = ((x + i + y + j) & 1) ? c : p;
        }
    }
}

void IRAM_ATTR gfx_blit(gfx_t *g, int x, int y, int w, int h, const uint8_t *src, int src_stride)
{
    int lx = x + g->ox, ly = y + g->oy;
    if (!clip_rect(g, &x, &y, &w, &h)) return;
    const uint8_t *s = src + (y - ly) * src_stride + (x - lx);
    uint8_t *d = ROWPTR(g, x, y);
    for (; h > 0; h--, s += src_stride, d += g->stride) k_copy(d, s, w);
}

// Draw one row of up to 16 pixels; bit 15 of 'bits' is the pixel at world x.
static inline void IRAM_ATTR put_bits(const gfx_t *g, uint8_t *row, int x, uint32_t bits, uint8_t c)
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
    uint8_t *d = row + (x - g->sx);
    while (bits) {
        int i = __builtin_clz(bits) - 16;
        d[i] = c;
        bits &= ~(0x8000u >> i);
    }
}

void IRAM_ATTR gfx_glyph16(gfx_t *g, int x, int y, const uint16_t *rows, int w, int h, uint8_t c)
{
    x += g->ox; y += g->oy;
    if (x >= g->cx1 || x + 16 <= g->cx0) return;
    int j0 = y < g->cy0 ? g->cy0 - y : 0;
    int j1 = y + h > g->cy1 ? g->cy1 - y : h;
    (void)w;
    for (int j = j0; j < j1; j++) {
        uint32_t bits = rows[j];
        if (bits) put_bits(g, g->px + (y + j - g->sy) * g->stride, x, bits, c);
    }
}

// ---------------------------------------------------------------- text

int gfx_font_h(int font) { return font == FONT_MONO ? 16 : FONT_UI_H; }

static inline int glyph_index(char ch)
{
    unsigned c = (unsigned char)ch;
    return (c < 32 || c > 126) ? '?' - 32 : (int)c - 32;
}

int IRAM_ATTR gfx_text_w(const char *s, int len, int font)
{
    if (len < 0) len = strlen(s);
    if (font == FONT_MONO) return len * 8;
    int w = 0;
    for (int i = 0; i < len; i++) w += font_ui_adv[glyph_index(s[i])];
    if (font == FONT_UI_BOLD) w += len;
    return w;
}

int IRAM_ATTR gfx_text(gfx_t *g, int x, int y, const char *s, int len, int font, uint8_t fg)
{
    if (len < 0) len = strlen(s);
    int wx = x + g->ox, wy = y + g->oy;
    int h = gfx_font_h(font);
    int start = wx;
    if (wy >= g->cy1 || wy + h <= g->cy0) return gfx_text_w(s, len, font);
    int j0 = wy < g->cy0 ? g->cy0 - wy : 0;
    int j1 = wy + h > g->cy1 ? g->cy1 - wy : h;
    uint8_t *row0 = g->px + (wy - g->sy) * g->stride;
    if (font == FONT_MONO) {
        for (int i = 0; i < len; i++, wx += 8) {
            if (wx >= g->cx1) { wx += (len - i) * 8; break; }
            if (wx + 8 <= g->cx0) continue;
            const uint8_t *gl = font_mono[glyph_index(s[i])];
            for (int j = j0; j < j1; j++) {
                uint32_t bits = (uint32_t)gl[j] << 8;
                if (bits) put_bits(g, row0 + j * g->stride, wx, bits, fg);
            }
        }
    } else {
        bool bold = font == FONT_UI_BOLD;
        for (int i = 0; i < len; i++) {
            int gi = glyph_index(s[i]);
            int adv = font_ui_adv[gi] + (bold ? 1 : 0);
            if (wx < g->cx1 && wx + 16 > g->cx0) {
                const uint16_t *gl = font_ui_rows[gi];
                for (int j = j0; j < j1; j++) {
                    uint32_t bits = gl[j];
                    if (bold) bits |= bits >> 1;
                    if (bits) put_bits(g, row0 + j * g->stride, wx, bits, fg);
                }
            }
            wx += adv;
        }
    }
    return wx - start;
}

int gfx_label_w(const char *s, int font)
{
    int w = 0;
    for (; *s; s++) {
        if (*s == '&') { s++; if (!*s) break; }
        w += gfx_text_w(s, 1, font);
    }
    return w;
}

int gfx_label(gfx_t *g, int x, int y, const char *s, int font, uint8_t fg, bool underline)
{
    int x0 = x;
    for (; *s; s++) {
        bool mn = false;
        if (*s == '&') { s++; if (!*s) break; mn = *s != '&'; }
        int adv = gfx_text(g, x, y, s, 1, font, fg);
        if (mn && underline) gfx_hline(g, x, y + (font == FONT_MONO ? 14 : FONT_UI_BASELINE + 2), adv - 1, fg);
        x += adv;
    }
    return x - x0;
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
