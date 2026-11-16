// Display interface: one 640x480 RGB222 framebuffer (one byte per pixel, in PSRAM) and a mouse
// pointer sprite kept outside of it.
//
// Backends (CONFIG_BEVEL_DISPLAY_*):
//   vga.c       the VGA scan-out through the LCD peripheral (beam racing, hardware-style sprite)
//   headless.c  no local screen: the framebuffer only, for the VNC server, QEMU and the
//               desktop simulator (host/), which presents it in an SDL window
// Remote viewers (vnc.c) learn about changes through display_damage().
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "sdkconfig.h"

#define VGA_W 640
#define VGA_H 480
#define VGA_ABS_SPAN_X 678   // H total 848 * 0.8: width of a standard-timing capture in our pixels

// RGB222 pixel: bits [1:0]=R, [3:2]=G, [5:4]=B, each 0..3
#define RGB222(r, g, b) ((uint8_t)((((b) & 3) << 4) | (((g) & 3) << 2) | ((r) & 3)))

// Starts the panel. The scan-out interrupt runs on the core that calls this.
esp_err_t vga_init(void);
uint8_t *vga_fb(void);

// Mouse pointer: a sprite overlaid while filling the bounce buffers, so it never touches
// the framebuffer. px = w*h bytes, 0xFF = transparent. Max 16x20.
void vga_cursor_shape(const uint8_t *px, int w, int h, int hot_x, int hot_y);
void vga_cursor_move(int x, int y);
void vga_cursor_show(bool on);
void vga_cursor_get(int *x, int *y, bool *shown);
void vga_cursor_overlay(uint8_t *dst_fb_copy); // draw the cursor into a copy of the frame (for dumps)
// Pointer image for remote viewers: copies the sprite (CUR_MAX_W x CUR_MAX_H max, 0xFF =
// transparent) and returns a counter that changes whenever the shape changes.
#define VGA_CUR_MAX_W 16
#define VGA_CUR_MAX_H 20
uint32_t vga_cursor_image(uint8_t *px, int *w, int *h, int *hot_x, int *hot_y);

// Name of the local display backend, for System Information.
const char *vga_name(void);

// Pixels in [x, x+w) x [y, y+h) of the framebuffer changed. The compositor calls this after each
// flush; anything else that writes the framebuffer directly must call it too.
#if CONFIG_BEVEL_VNC
void vnc_damage(int x, int y, int w, int h);
static inline void display_damage(int x, int y, int w, int h) { vnc_damage(x, y, w, h); }
#else
static inline void display_damage(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
#endif

// Beam racing: block until rows [y0,y1) can be rewritten without the scan-out reading them
// half-way, assuming the write takes est_us. Returns the time waited in microseconds.
int vga_wait_rows(int y0, int y1, int est_us);
// Next framebuffer row the scan-out will fetch.
int vga_fetch_row(void);

typedef struct {
    uint32_t frames;        // frames scanned out
    uint32_t fills;         // bounce buffer fills
    uint32_t isr_avg_us10;  // average fill time, 0.1 us units
    uint32_t isr_max_us;    // worst fill time
    int32_t  slack_min_us;  // worst-case time left before the DMA needed the buffer (<0 = underrun)
    uint32_t late;          // fills that finished after the DMA started on the buffer (visible glitch)
    uint32_t isr_core;
} vga_stats_t;
void vga_get_stats(vga_stats_t *out, bool reset);

// 0 = libc memcpy, 1 = PIE 128-bit copy
void vga_set_copy_mode(int mode);
int vga_get_copy_mode(void);
