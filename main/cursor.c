// Mouse pointer sprite: shape, position and visibility. The VGA backend overlays it while filling
// its bounce buffers; the headless backend only keeps it for remote viewers and dumps.
#include <string.h>
#include "cursor_priv.h"
#include "esp_attr.h"

DRAM_ATTR uint8_t vga_cur_px[VGA_CUR_MAX_W * VGA_CUR_MAX_H];
DRAM_ATTR volatile int16_t vga_cur_x, vga_cur_y;
DRAM_ATTR volatile int8_t vga_cur_w, vga_cur_h, vga_cur_hx, vga_cur_hy;
DRAM_ATTR volatile bool vga_cur_on;
static volatile uint32_t s_cur_serial = 1;   // bumped on every shape or visibility change

void vga_cursor_shape(const uint8_t *px, int w, int h, int hot_x, int hot_y)
{
    if (w > VGA_CUR_MAX_W) w = VGA_CUR_MAX_W;
    if (h > VGA_CUR_MAX_H) h = VGA_CUR_MAX_H;
    int px_x = vga_cur_x + vga_cur_hx, px_y = vga_cur_y + vga_cur_hy; // pointer position stays put
    bool was = vga_cur_on;
    vga_cur_on = false;
    memset(vga_cur_px, 0xFF, sizeof(vga_cur_px));
    for (int y = 0; y < h; y++) memcpy(vga_cur_px + y * VGA_CUR_MAX_W, px + y * w, w);
    vga_cur_w = w; vga_cur_h = h; vga_cur_hx = hot_x; vga_cur_hy = hot_y;
    vga_cur_x = px_x - hot_x; vga_cur_y = px_y - hot_y;
    vga_cur_on = was;
    s_cur_serial++;
}

void vga_cursor_move(int x, int y)
{
    vga_cur_x = x - vga_cur_hx;
    vga_cur_y = y - vga_cur_hy;
}

void vga_cursor_show(bool on)
{
    if (on != vga_cur_on) s_cur_serial++;
    vga_cur_on = on;
}

void vga_cursor_get(int *x, int *y, bool *shown)
{
    if (x) *x = vga_cur_x + vga_cur_hx;
    if (y) *y = vga_cur_y + vga_cur_hy;
    if (shown) *shown = vga_cur_on;
}

void vga_cursor_overlay(uint8_t *dst)
{
    if (!vga_cur_on) return;
    for (int y = 0; y < vga_cur_h; y++) {
        for (int x = 0; x < vga_cur_w; x++) {
            int sx = vga_cur_x + x, sy = vga_cur_y + y;
            uint8_t p = vga_cur_px[y * VGA_CUR_MAX_W + x];
            if (p != 0xFF && sx >= 0 && sx < VGA_W && sy >= 0 && sy < VGA_H) dst[sy * VGA_W + sx] = p;
        }
    }
}

uint32_t vga_cursor_image(uint8_t *px, int *w, int *h, int *hot_x, int *hot_y)
{
    uint32_t serial;
    do {
        serial = s_cur_serial;
        bool on = vga_cur_on;
        *w = on ? vga_cur_w : 0;
        *h = on ? vga_cur_h : 0;
        *hot_x = vga_cur_hx;
        *hot_y = vga_cur_hy;
        for (int y = 0; y < *h; y++) memcpy(px + y * *w, vga_cur_px + y * VGA_CUR_MAX_W, *w);
    } while (serial != s_cur_serial);
    return serial;
}
