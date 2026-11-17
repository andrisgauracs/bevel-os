// Display: the panel of whichever board this firmware is built for. The OS always draws in its
// 64-colour RGB222 palette, one byte per pixel, into a logical framebuffer; what happens to those
// bytes from there is per board:
//
//   ESP32-P4 (main/display_dsi.c): the 7" 1024x600 MIPI-DSI panel (JD9165). Every update is
//     converted to RGB565 on its way into the panel driver's own framebuffer, which the DSI
//     controller scans out by DMA. The mouse pointer and the true-colour overlays (camera,
//     pictures, Doom) are added during that conversion and never touch the logical framebuffer.
//   ESP32-S3 (main/display_vga.c, main/vga.c): the same framebuffer is scanned out straight to a
//     resistor DAC at 640x480@60. The pointer is a sprite the scan-out interrupt draws, and the
//     compositor waits for the beam before rewriting rows it is about to read ("beam racing").
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "board.h"

#if CONFIG_IDF_TARGET_ESP32P4
#define SCREEN_W 1024
#define SCREEN_H 600
#else
#define SCREEN_W 640
#define SCREEN_H 480
#endif

// What this display can do. The DSI panel has a PWM backlight and can key out true-colour layers;
// the VGA scan-out can be waited for and measured instead.
#if CONFIG_IDF_TARGET_ESP32P4
#define DISP_HAS_BACKLIGHT 1
#define DISP_HAS_OVERLAYS  1
#define DISP_HAS_SCANOUT   0
#else
#define DISP_HAS_BACKLIGHT 0
#define DISP_HAS_OVERLAYS  0
#define DISP_HAS_SCANOUT   1
#endif

// RGB222 pixel: bits [1:0]=R, [3:2]=G, [5:4]=B, each 0..3
#define RGB222(r, g, b) ((uint8_t)((((b) & 3) << 4) | (((g) & 3) << 2) | ((r) & 3)))
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))  // each 0..255

// Absolute pointers (a KVM's digitiser, a tablet) report where some capture device sees the picture.
// On the VGA board that is a capture expecting standard timing: the 640 active pixels sit centred in
// 678 of the 848-pixel line, 19 widths of blanking on each side. The DSI panel is scanned out
// directly, so the whole panel is what such a pointer covers.
#if CONFIG_IDF_TARGET_ESP32P4
#define POINTER_SPAN_X  SCREEN_W
#define POINTER_OFFS_X  0
#else
#define POINTER_SPAN_X  678
#define POINTER_OFFS_X  19
#endif

// Starts the panel. For the VGA board the scan-out interrupt lives on the core that calls this.
esp_err_t disp_init(void);
uint8_t *disp_fb(void);                 // logical framebuffer, SCREEN_W x SCREEN_H bytes

// Copy w x h pixels (RGB222, row stride `stride`) to the logical framebuffer at (x, y) and show them.
void disp_put(int x, int y, int w, int h, const uint8_t *src, int stride);
// Show a part of the logical framebuffer again (after drawing into it directly, or when an overlay changed).
void disp_present(int x, int y, int w, int h);

// What the screen shows, converted to RGB565 into dst (SCREEN_W x SCREEN_H pixels), pointer and
// overlays included. hide_cursor leaves the pointer out. dst is a PSRAM buffer; call from a worker task.
void disp_snapshot(uint16_t *dst, bool hide_cursor);

// Mouse pointer: drawn over the picture instead of into the framebuffer, so moving it costs nothing
// in repaints. px = w*h bytes, 0xFF = transparent. Max 16x20.
void disp_cursor_shape(const uint8_t *px, int w, int h, int hot_x, int hot_y);
void disp_cursor_move(int x, int y);
void disp_cursor_show(bool on);
void disp_cursor_get(int *x, int *y, bool *shown);

// True-colour overlays: a window paints DISP_OVERLAY_KEY(id) where the overlay should appear and
// places the overlay's surface on the screen; anything drawn over the key colour (another window, a
// menu) hides the overlay there, as with a chroma-keyed video card. Only the DSI panel can do this;
// on the VGA board allocating a layer says so (id -1, no pixels) and the app that asked for one
// (canvas16 in the program API) draws RGB222 instead.
#define DISP_OVERLAY_KEY(id) ((uint8_t)(0x40 + (id)))
#if DISP_HAS_OVERLAYS
#define DISP_OVERLAYS 4
int disp_overlay_alloc(int w, int h);               // returns an id, or -1
uint16_t *disp_overlay_px(int id, int *w, int *h);  // RGB565 surface, row stride = w
#else
#define DISP_OVERLAYS 0
int disp_overlay_alloc(int w, int h);               // always -1
uint16_t *disp_overlay_px(int id, int *w, int *h);  // always NULL
#endif
void disp_overlay_place(int id, int x, int y);      // screen position of the surface's top-left pixel
void disp_overlay_update(int id);                   // its pixels changed: show them (any task)
void disp_overlay_free(int id);

#if DISP_HAS_BACKLIGHT
// Backlight, 0..100 %
void disp_set_brightness(int percent);
int disp_get_brightness(void);
#endif

#if DISP_HAS_SCANOUT
// Racing the beam: block until rows [y0,y1) can be rewritten without the scan-out reading them
// half-way, assuming the write takes est_us. Returns the microseconds spent waiting. disp_fetch_row
// is the next framebuffer row the scan-out will read.
int disp_wait_rows(int y0, int y1, int est_us);
int disp_fetch_row(void);

typedef struct {
    uint32_t frames;        // frames scanned out
    uint32_t fills;         // bounce buffer fills
    uint32_t isr_avg_us10;  // average fill time, 0.1 us units
    uint32_t isr_max_us;    // worst fill time
    int32_t  slack_min_us;  // worst-case time left before the DMA needed the buffer (<0 = underrun)
    uint32_t late;          // fills that finished after the DMA started on the buffer (visible glitch)
    uint32_t isr_core;      // core the scan-out interrupt runs on
} disp_scanout_t;
void disp_get_scanout(disp_scanout_t *out, bool reset);

// How the framebuffer is filled: 0 = libc memcpy, 1 = PIE 128-bit copy.
void disp_set_copy_mode(int mode);
int disp_get_copy_mode(void);
#endif
