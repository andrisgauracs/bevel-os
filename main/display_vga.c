// The ESP32-S3 board's display: the VGA scan-out of main/vga.c behind the common display API.
//
// There is nothing to convert here: the logical framebuffer is what the scan-out reads, so disp_put()
// copies bytes into it and disp_present() has nothing to do. The pointer is a sprite drawn by the
// scan-out interrupt, and a snapshot turns RGB222 into RGB565 so the same screen-dump tool works on
// both boards. This board has no backlight and no true-colour layer.
#include <string.h>
#include "display.h"
#include "vga.h"
#include "gfx.h"        // the PIE copies, declared where HAS_PIE is set
#include "esp_attr.h"

// Each of the 64 palette entries as the 16-bit colour the dump tool expects.
static uint16_t s_rgb222_to_565[64];

esp_err_t disp_init(void)
{
    for (int v = 0; v < 64; v++)
        s_rgb222_to_565[v] = RGB565((v & 3) * 85, ((v >> 2) & 3) * 85, ((v >> 4) & 3) * 85);
    return vga_init();
}

uint8_t *disp_fb(void) { return vga_fb(); }

void IRAM_ATTR disp_put(int x, int y, int w, int h, const uint8_t *src, int stride)
{
    uint8_t *fb = vga_fb();
    if (!fb || x < 0 || y < 0 || x + w > SCREEN_W || y + h > SCREEN_H || w <= 0 || h <= 0) return;
    uint8_t *d = fb + (size_t)y * SCREEN_W + x;
    // The compositor hands over 16-pixel-wide strips, which is what the PIE kernels need.
    if (vga_get_copy_mode() == 1 && (x & 15) == 0 && (w & 15) == 0 && ((uintptr_t)src & 15) == 0)
        for (int r = 0; r < h; r++, d += SCREEN_W, src += stride) pie_copy16(d, src, w >> 4);
    else
        for (int r = 0; r < h; r++, d += SCREEN_W, src += stride) memcpy(d, src, w);
}

void disp_present(int x, int y, int w, int h)
{
    (void)x; (void)y; (void)w; (void)h;   // the scan-out reads the framebuffer itself: nothing to do
}

void disp_snapshot(uint16_t *dst, bool hide_cursor) { disp_snapshot_rows(dst, 0, SCREEN_H, hide_cursor); }

void disp_snapshot_rows(uint16_t *dst, int y0, int rows, bool hide_cursor)
{
    static DRAM_ATTR uint8_t s_row[SCREEN_W];
    uint8_t *fb = vga_fb();
    if (!dst || !fb || y0 < 0 || rows <= 0 || y0 + rows > SCREEN_H) return;
    // The pointer lives in the scan-out, not in the framebuffer, so taking it out of the picture also
    // hides it on screen for a moment - the same thing the other board's dump does.
    bool shown;
    vga_cursor_get(NULL, NULL, &shown);
    if (hide_cursor && shown) vga_cursor_show(false);
    for (int y = y0; y < y0 + rows; y++) {
        memcpy(s_row, fb + (size_t)y * SCREEN_W, SCREEN_W);
        vga_cursor_overlay_row(s_row, y);
        uint16_t *d = dst + (size_t)(y - y0) * SCREEN_W;
        for (int x = 0; x < SCREEN_W; x++) d[x] = s_rgb222_to_565[s_row[x]];
    }
    if (hide_cursor && shown) vga_cursor_show(true);
}

void disp_cursor_shape(const uint8_t *px, int w, int h, int hot_x, int hot_y)
{
    vga_cursor_shape(px, w, h, hot_x, hot_y);
}
void disp_cursor_move(int x, int y) { vga_cursor_move(x, y); }
void disp_cursor_show(bool on) { vga_cursor_show(on); }
void disp_cursor_get(int *x, int *y, bool *shown) { vga_cursor_get(x, y, shown); }

// No chroma-key layer on this panel: an app that asks for one is told there is none.
int disp_overlay_alloc(int w, int h) { (void)w; (void)h; return -1; }
uint16_t *disp_overlay_px(int id, int *w, int *h)
{
    (void)id;
    if (w) *w = 0;
    if (h) *h = 0;
    return NULL;
}
void disp_overlay_place(int id, int x, int y) { (void)id; (void)x; (void)y; }
void disp_overlay_update(int id) { (void)id; }
void disp_overlay_free(int id) { (void)id; }

int disp_wait_rows(int y0, int y1, int est_us) { return vga_wait_rows(y0, y1, est_us); }
int disp_fetch_row(void) { return vga_fetch_row(); }

void disp_get_scanout(disp_scanout_t *o, bool reset)
{
    vga_stats_t v;
    vga_get_stats(&v, reset);
    o->frames = v.frames; o->fills = v.fills; o->isr_avg_us10 = v.isr_avg_us10;
    o->isr_max_us = v.isr_max_us; o->slack_min_us = v.slack_min_us;
    o->late = v.late; o->isr_core = v.isr_core;
}

void disp_set_copy_mode(int mode) { vga_set_copy_mode(mode); }
int disp_get_copy_mode(void) { return vga_get_copy_mode(); }
