// 2D renderer: clipped primitives, bitmap fonts and blits onto an 8-bit (RGB222) surface.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "assets.h"

typedef struct { int16_t x, y, w, h; } rect_t;

static inline rect_t rect_make(int x, int y, int w, int h) { rect_t r = {x, y, w, h}; return r; }
static inline bool rect_empty(const rect_t *r) { return r->w <= 0 || r->h <= 0; }
static inline bool rect_has(const rect_t *r, int x, int y)
{
    return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}
bool rect_intersect(rect_t *out, const rect_t *a, const rect_t *b); // false if empty
void rect_union(rect_t *out, const rect_t *a, const rect_t *b);
static inline bool rect_overlap(const rect_t *a, const rect_t *b)
{
    return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

// A drawing context. "World" coordinates are screen coordinates; the surface covers the world
// rectangle starting at (sx, sy). Drawing calls take coordinates relative to the origin (ox, oy)
// and are clipped to [cx0,cx1) x [cy0,cy1) in world coordinates.
typedef struct gfx {
    uint8_t *px;
    int stride;
    int sx, sy;
    int cx0, cy0, cx1, cy1;
    int ox, oy;
} gfx_t;

typedef struct { int cx0, cy0, cx1, cy1, ox, oy; } gfx_state_t;

void gfx_init(gfx_t *g, uint8_t *px, int stride, int sx, int sy, int w, int h);
static inline void gfx_save(const gfx_t *g, gfx_state_t *s)
{
    s->cx0 = g->cx0; s->cy0 = g->cy0; s->cx1 = g->cx1; s->cy1 = g->cy1; s->ox = g->ox; s->oy = g->oy;
}
static inline void gfx_restore(gfx_t *g, const gfx_state_t *s)
{
    g->cx0 = s->cx0; g->cy0 = s->cy0; g->cx1 = s->cx1; g->cy1 = s->cy1; g->ox = s->ox; g->oy = s->oy;
}
// Intersect the clip with a rectangle given in local coordinates. Returns false if nothing is left.
bool gfx_clip(gfx_t *g, int x, int y, int w, int h);
static inline void gfx_translate(gfx_t *g, int dx, int dy) { g->ox += dx; g->oy += dy; }
// Is any part of the local rectangle inside the clip?
bool gfx_visible(const gfx_t *g, int x, int y, int w, int h);

void gfx_fill(gfx_t *g, int x, int y, int w, int h, uint8_t c);
void gfx_hline(gfx_t *g, int x, int y, int w, uint8_t c);
void gfx_vline(gfx_t *g, int x, int y, int h, uint8_t c);
void gfx_frame(gfx_t *g, int x, int y, int w, int h, uint8_t c);          // 1px outline
void gfx_pixel(gfx_t *g, int x, int y, uint8_t c);
void gfx_line(gfx_t *g, int x0, int y0, int x1, int y1, uint8_t c);
void gfx_checker(gfx_t *g, int x, int y, int w, int h, uint8_t c0, uint8_t c1); // 50% dither
void gfx_dotted(gfx_t *g, int x, int y, int w, int h, uint8_t c);         // dotted focus rectangle
void gfx_bevel(gfx_t *g, int x, int y, int w, int h, uint8_t tl, uint8_t br); // 1px two-colour edge
void gfx_bitmap(gfx_t *g, int x, int y, const bitmap_t *b);               // 0xFF = transparent
void gfx_bitmap_tint(gfx_t *g, int x, int y, const bitmap_t *b, uint8_t c); // selected-icon look
void gfx_blit(gfx_t *g, int x, int y, int w, int h, const uint8_t *src, int src_stride); // opaque
void gfx_glyph16(gfx_t *g, int x, int y, const uint16_t *rows, int w, int h, uint8_t c); // bit15 = left

enum { FONT_UI = 0, FONT_UI_BOLD = 1, FONT_MONO = 2 };
int gfx_font_h(int font);
int gfx_text_w(const char *s, int len, int font);                         // len < 0: NUL-terminated
int gfx_text(gfx_t *g, int x, int y, const char *s, int len, int font, uint8_t fg); // returns advance
// Text with a '&' mnemonic marker ("&File" underlines F). "&&" draws one ampersand.
int gfx_label_w(const char *s, int font);
int gfx_label(gfx_t *g, int x, int y, const char *s, int font, uint8_t fg, bool underline);
char gfx_label_mnemonic(const char *s);                                   // lower-case letter or 0

// Raw kernels
void k_copy(uint8_t *dst, const uint8_t *src, int n);
void k_fill(uint8_t *dst, uint8_t c, int n);
