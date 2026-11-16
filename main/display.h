// Display: the board's 7" 1024x600 MIPI-DSI panel (JD9165), with the PWM backlight.
//
// The OS still draws in its 64-colour RGB222 palette, one byte per pixel, into a logical
// framebuffer. Every update is converted to RGB565 on its way into the panel driver's own
// framebuffer, which the DSI controller scans out by DMA. Two things are added during that
// conversion and never touch the logical framebuffer: the mouse pointer, and true-colour
// overlays (camera preview, pictures), which show through wherever a "key" colour was drawn.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define SCREEN_W 1024
#define SCREEN_H 600

// RGB222 pixel: bits [1:0]=R, [3:2]=G, [5:4]=B, each 0..3
#define RGB222(r, g, b) ((uint8_t)((((b) & 3) << 4) | (((g) & 3) << 2) | ((r) & 3)))
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))  // each 0..255

esp_err_t disp_init(void);
uint8_t *disp_fb(void);                 // logical framebuffer, SCREEN_W x SCREEN_H bytes
const uint16_t *disp_panel_fb(void);    // what the panel shows (RGB565), for screen dumps

// Copy w x h pixels (RGB222, row stride `stride`) to the logical framebuffer at (x, y) and show them.
void disp_put(int x, int y, int w, int h, const uint8_t *src, int stride);
// Show a part of the logical framebuffer again (after drawing into it directly, or when an overlay changed).
void disp_present(int x, int y, int w, int h);

// Mouse pointer: drawn over the picture while it is converted, so moving it costs two small
// conversions and no repaint. px = w*h bytes, 0xFF = transparent. Max 16x20.
void disp_cursor_shape(const uint8_t *px, int w, int h, int hot_x, int hot_y);
void disp_cursor_move(int x, int y);
void disp_cursor_show(bool on);
void disp_cursor_get(int *x, int *y, bool *shown);

// True-colour overlays. A window paints DISP_OVERLAY_KEY(id) where the overlay should appear and
// places the overlay's surface on the screen; anything drawn over the key colour (another window,
// a menu) hides the overlay there, as with a chroma-keyed video card.
#define DISP_OVERLAYS 4
#define DISP_OVERLAY_KEY(id) ((uint8_t)(0x40 + (id)))
int disp_overlay_alloc(int w, int h);               // returns an id, or -1
uint16_t *disp_overlay_px(int id, int *w, int *h);  // RGB565 surface, row stride = w
void disp_overlay_place(int id, int x, int y);      // screen position of the surface's top-left pixel
void disp_overlay_update(int id);                   // its pixels changed: show them (any task)
void disp_overlay_free(int id);

// Backlight, 0..100 %
void disp_set_brightness(int percent);
int disp_get_brightness(void);
