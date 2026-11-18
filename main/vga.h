// VGA scan-out on the ESP32-S3: 640x480@60, RGB222, one byte per pixel, framebuffer in PSRAM.
// main/display_vga.c puts the common display API (main/display.h) on top of this.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "display.h"

#define VGA_W SCREEN_W
#define VGA_H SCREEN_H

// Starts the panel. The scan-out interrupt runs on the core that calls this.
esp_err_t vga_init(void);
uint8_t *vga_fb(void);

// Mouse pointer: a sprite overlaid while filling the bounce buffers, so it never touches
// the framebuffer. px = w*h bytes, 0xFF = transparent. Max 16x20.
void vga_cursor_shape(const uint8_t *px, int w, int h, int hot_x, int hot_y);
void vga_cursor_move(int x, int y);
void vga_cursor_show(bool on);
void vga_cursor_get(int *x, int *y, bool *shown);
// Draw the pointer into one row of an RGB222 copy of the frame (for snapshots).
void vga_cursor_overlay_row(uint8_t *row, int y);

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
