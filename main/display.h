// Display: the panel of whichever board this firmware is built for. The OS draws into a logical
// framebuffer of pixel_t; what happens from there is per board:
//
//   ESP32-P4 (main/display_dsi.c): the 7" 1024x600 MIPI-DSI panel (JD9165). Pixels are RGB565. Every
//     update is copied into the panel driver's own framebuffer, which the DSI controller scans out by
//     DMA. The mouse pointer and the true-colour overlays (camera, Doom) are added during that copy
//     and never touch the logical framebuffer.
//   ESP32-S3 (main/display_vga.c, main/vga.c): pixels are RGB222 bytes, the 64 colours its resistor
//     DAC can show, and the framebuffer is scanned out straight to it at 640x480@60. The pointer is a
//     sprite the scan-out interrupt draws, and the compositor waits for the beam before rewriting
//     rows it is about to read ("beam racing").
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
#define DISP_TRUE_COLOR    1
#else
#define DISP_HAS_BACKLIGHT 0
#define DISP_HAS_OVERLAYS  0
#define DISP_HAS_SCANOUT   1
#define DISP_TRUE_COLOR    0
#endif

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))  // each 0..255

// The palette byte: bits [1:0]=R, [3:2]=G, [5:4]=B, each 0..3. Icons, fonts, the pointer, saved
// settings and the canvases of loadable programs are in this form on every board.
#define RGB222_8(r, g, b) ((uint8_t)((((b) & 3) << 4) | (((g) & 3) << 2) | ((r) & 3)))

// RGB222(r, g, b) is a palette colour as a pixel of this screen; gfx_from8() does the same at run time.
#if DISP_TRUE_COLOR
typedef uint16_t pixel_t;
#define RGB222_L5(v) ((v) == 0 ? 0 : (v) == 1 ? 10 : (v) == 2 ? 21 : 31)
#define RGB222(r, g, b) ((pixel_t)(RGB222_L5((r) & 3) << 11 | ((g) & 3) * 21 << 5 | RGB222_L5((b) & 3)))
extern uint16_t disp_rgb222_lut[256];
static inline pixel_t gfx_from8(uint8_t c) { return disp_rgb222_lut[c]; }
#else
typedef uint8_t pixel_t;
#define RGB222(r, g, b) RGB222_8(r, g, b)
static inline pixel_t gfx_from8(uint8_t c) { return c; }
#endif

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
pixel_t *disp_fb(void);                 // SCREEN_W x SCREEN_H pixels, to draw into before disp_present()

// Copy w x h pixels (row stride `stride`, in pixels) to the logical framebuffer at (x, y) and show them.
void disp_put(int x, int y, int w, int h, const pixel_t *src, int stride);
// Show a part of the logical framebuffer (after drawing into it directly: the boot splash, the shut-down screen).
void disp_present(int x, int y, int w, int h);

// What rows y0.. of the screen show, converted to RGB565 into dst (SCREEN_W pixels per row), pointer
// and overlays included. hide_cursor leaves the pointer out. Call from a worker task.
void disp_snapshot_rows(uint16_t *dst, int y0, int rows, bool hide_cursor);
// The same picture in this screen's own pixels, pointer included: half the memory on the VGA board.
// disp_to_565() converts one of them.
void disp_read_rows(pixel_t *dst, int y0, int rows);
#if DISP_TRUE_COLOR
static inline uint16_t disp_to_565(pixel_t p) { return p; }
#else
static inline uint16_t disp_to_565(pixel_t p) { return RGB565((p & 3) * 85, ((p >> 2) & 3) * 85, ((p >> 4) & 3) * 85); }
#endif

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
// (canvas16 in the program API) draws RGB222 instead. The keys are colours the palette cannot make:
// DISP_OVERLAY_KEY as a pixel of the screen, DISP_OVERLAY_KEY8 as a byte in a program's canvas.
#define DISP_OVERLAY_KEY8(id) ((uint8_t)(0x40 + (id)))
#if DISP_TRUE_COLOR
#define DISP_OVERLAY_KEY_BASE 0x0820
#define DISP_OVERLAY_KEY(id) ((pixel_t)(DISP_OVERLAY_KEY_BASE + (id)))
#else
#define DISP_OVERLAY_KEY(id) ((pixel_t)DISP_OVERLAY_KEY8(id))
#endif
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
