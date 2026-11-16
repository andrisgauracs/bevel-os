// VGA 640x480@60 output via the ESP32-S3 RGB LCD peripheral.
//
// The driver runs in "no framebuffer" mode: it owns two 10-line bounce buffers in internal RAM
// and calls us from the DMA end-of-buffer interrupt to refill them. We copy from our own PSRAM
// framebuffer, overlay the mouse pointer on the way (a hardware-style sprite: moving the pointer
// costs zero framebuffer writes), and record where the scan-out is so the compositor can avoid
// rewriting rows that are about to be fetched (tear-free without a second framebuffer).
#include <string.h>
#include "vga.h"
#include "cursor_priv.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp32s3/rom/cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "vga";

// Pin map (see docs/HANDOFF.md). Do not change without asking the human: it is wired.
#define PIN_R_LO 5
#define PIN_R_HI 4
#define PIN_G_LO 7
#define PIN_G_HI 6
#define PIN_B_LO 16
#define PIN_B_HI 15
#define PIN_HSYNC 17
#define PIN_VSYNC 18

// 640x480@60 on an integer-divided clock: 160 MHz / 6 = 26.67 MHz, H total 848 (31.45 kHz),
// V total 525 (59.9 Hz), negative sync. The exact 25.175 MHz needs the fractional divider,
// which the test monitor rejects.
#define PCLK_HZ   26666667
#define H_FP 36
#define H_SYNC 102
#define H_BP 70
#define V_FP 10
#define V_SYNC 2
#define V_BP 33
#define H_TOTAL (VGA_W + H_FP + H_SYNC + H_BP)
#define V_TOTAL (VGA_H + V_FP + V_SYNC + V_BP)

#define BB_LINES 10
#define BB_BYTES (VGA_W * BB_LINES)
#define CPU_HZ 240000000u
// CPU cycles per bounce buffer (10 lines)
#define BB_CYCLES ((uint32_t)((uint64_t)H_TOTAL * BB_LINES * CPU_HZ / PCLK_HZ))
#define LINE_CYCLES (BB_CYCLES / BB_LINES)

#if CONFIG_BEVEL_PIE
void pie_copy16_isr(void *dst, const void *src, int blocks);
#else
#define pie_copy16_isr(d, s, n) memcpy((d), (s), (size_t)(n) << 4)
#endif

static esp_lcd_panel_handle_t s_panel;
static uint8_t *s_fb;
#if CONFIG_BEVEL_PIE
static int s_copy_mode = 1;
#else
static int s_copy_mode = 0;
#endif

// The pointer sprite lives in cursor.c (shared with the headless backend); the ISR reads it.
#define s_cur_px  vga_cur_px
#define s_cur_x   vga_cur_x
#define s_cur_y   vga_cur_y
#define s_cur_w   vga_cur_w
#define s_cur_h   vga_cur_h
#define s_cur_on  vga_cur_on
#define CUR_MAX_W VGA_CUR_MAX_W
#define CUR_MAX_H VGA_CUR_MAX_H

// --- beam state (written by the ISR) ---
static DRAM_ATTR volatile int32_t s_fetch_row;      // first row of the next fetch (or the one in progress)
static DRAM_ATTR volatile uint32_t s_fetch_time;    // cycle count when that fetch is expected

// --- stats ---
static DRAM_ATTR volatile uint32_t s_frames, s_fills, s_isr_max, s_late, s_isr_core;
static DRAM_ATTR volatile uint64_t s_isr_sum;
static DRAM_ATTR volatile int32_t s_slack_min = INT32_MAX;
static DRAM_ATTR uint32_t s_anchor;                 // estimated cycle count of the EOF that asked for rows 20..29

static inline void IRAM_ATTR overlay_cursor(uint8_t *buf, int row0)
{
    int cy = s_cur_y, ch = s_cur_h;
    if (cy >= row0 + BB_LINES || cy + ch <= row0) return;
    int cx = s_cur_x, cw = s_cur_w;
    int ya = cy < row0 ? row0 : cy;
    int yb = cy + ch > row0 + BB_LINES ? row0 + BB_LINES : cy + ch;
    int xa = cx < 0 ? 0 : cx;
    int xb = cx + cw > VGA_W ? VGA_W : cx + cw;
    for (int y = ya; y < yb; y++) {
        const uint8_t *s = s_cur_px + (y - cy) * CUR_MAX_W + (xa - cx);
        uint8_t *d = buf + (y - row0) * VGA_W + xa;
        for (int x = xa; x < xb; x++, s++, d++) {
            uint8_t p = *s;
            if (p != 0xFF) *d = p;
        }
    }
}

static bool IRAM_ATTR on_bounce_empty(esp_lcd_panel_handle_t panel, void *buf, int pos_px, int len, void *ctx)
{
    uint32_t t0 = esp_cpu_get_cycle_count();
    int row = pos_px / VGA_W;
    s_fetch_row = row;
    s_fetch_time = t0;

    const uint8_t *src = s_fb + pos_px;
    if (s_copy_mode == 1 && ((uintptr_t)buf & 15) == 0) {
        pie_copy16_isr(buf, src, len >> 4);
    } else {
        memcpy(buf, src, len);
    }
    int next = pos_px + len;
    if (next >= VGA_W * VGA_H) next = 0;
    // Start pulling the next 10 lines from PSRAM into the data cache now, so the next fill
    // is mostly a cache-to-RAM copy.
    Cache_Start_DCache_Preload((uint32_t)(s_fb + next), len, 0);

    if (s_cur_on) overlay_cursor(buf, row);

    uint32_t t1 = esp_cpu_get_cycle_count();
    uint32_t dt = t1 - t0;
    s_isr_sum += dt;
    if (dt > s_isr_max) s_isr_max = dt;
    s_fills++;

    // Timeline within a frame: rows 0-9 are fetched when the beam is at line 470 of the previous
    // frame and rows 10-19 at line 480; from then on rows R..R+9 are fetched when the beam is at
    // R-10, and must be ready one buffer period later.
    int idx = row / BB_LINES;
    if (idx >= 2) {
        uint32_t nominal = (uint32_t)(idx - 2) * BB_CYCLES;
        uint32_t a = t0 - nominal;
        if (idx == 2 || (int32_t)(a - s_anchor) < 0) s_anchor = a;
        int32_t slack = (int32_t)(s_anchor + nominal + BB_CYCLES - t1);
        if (slack < s_slack_min) s_slack_min = slack;
        if (slack < 0) s_late++;
    }
    if (next == 0) s_frames++;

    // When is the next fetch?
    uint32_t gap = BB_CYCLES;
    if (row == BB_LINES) gap = (uint32_t)(V_TOTAL - VGA_H + BB_LINES) * LINE_CYCLES; // across vertical blanking
    s_fetch_time = t0 + gap;
    s_fetch_row = next / VGA_W;
    return false;
}

esp_err_t vga_init(void)
{
    s_fb = heap_caps_aligned_calloc(64, 1, VGA_W * VGA_H, MALLOC_CAP_SPIRAM);
    if (!s_fb) return ESP_ERR_NO_MEM;
    s_isr_core = xPortGetCoreID();

    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_PLL160M,
        .timings = {
            .pclk_hz = PCLK_HZ,
            .h_res = VGA_W,
            .v_res = VGA_H,
            .hsync_pulse_width = H_SYNC,
            .hsync_back_porch = H_BP,
            .hsync_front_porch = H_FP,
            .vsync_pulse_width = V_SYNC,
            .vsync_back_porch = V_BP,
            .vsync_front_porch = V_FP,
            .flags = { .hsync_idle_low = 0, .vsync_idle_low = 0 }, // negative sync
        },
        .data_width = 8,
        .bits_per_pixel = 8,
        .bounce_buffer_size_px = BB_BYTES,
        .dma_burst_size = 64,
        .hsync_gpio_num = PIN_HSYNC,
        .vsync_gpio_num = PIN_VSYNC,
        .de_gpio_num = -1,
        .pclk_gpio_num = -1,
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            PIN_R_LO, PIN_R_HI, PIN_G_LO, PIN_G_HI, PIN_B_LO, PIN_B_HI, -1, -1,
            -1, -1, -1, -1, -1, -1, -1, -1,
        },
        .flags = { .no_fb = 1 },
    };
    esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_rgb_panel failed: %s", esp_err_to_name(err));
        return err;
    }
    esp_lcd_rgb_panel_event_callbacks_t cbs = { .on_bounce_empty = on_bounce_empty };
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(s_panel, &cbs, NULL));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    return ESP_OK;
}

uint8_t *vga_fb(void) { return s_fb; }
const char *vga_name(void) { return "640 x 480, 64 colours, 60 Hz VGA"; }

int vga_fetch_row(void) { return s_fetch_row; }

// Would any fetch that happens before now+est touch rows [y0,y1)?
static inline bool IRAM_ATTR rows_busy(int y0, int y1, uint32_t est_cycles)
{
    int32_t row; uint32_t t;
    do { row = s_fetch_row; t = s_fetch_time; } while (row != s_fetch_row);
    uint32_t now = esp_cpu_get_cycle_count();
    int32_t until = (int32_t)(now + est_cycles + LINE_CYCLES - t); // cycles past the next fetch when we finish
    if (until < 0) return false;                                    // done before the next fetch
    int n = (until / (int32_t)BB_CYCLES + 1) * BB_LINES;            // rows fetched in the meantime
    if (n >= VGA_H) return false;                                   // cannot be avoided, don't wait
    int a = row, b = row + n;                                       // fetched rows [a,b), may wrap
    if (b <= VGA_H) return y0 < b && y1 > a;
    return y1 > a || y0 < b - VGA_H;
}

int IRAM_ATTR vga_wait_rows(int y0, int y1, int est_us)
{
    uint32_t est = (uint32_t)est_us * (CPU_HZ / 1000000);
    if (!rows_busy(y0, y1, est)) return 0;
    uint32_t t0 = esp_cpu_get_cycle_count();
    while (rows_busy(y0, y1, est)) {
        if (esp_cpu_get_cycle_count() - t0 > CPU_HZ / 50) break; // 20 ms safety net
    }
    return (esp_cpu_get_cycle_count() - t0) / (CPU_HZ / 1000000);
}

void vga_get_stats(vga_stats_t *o, bool reset)
{
    portDISABLE_INTERRUPTS();
    uint32_t fills = s_fills;
    uint64_t sum = s_isr_sum;
    o->frames = s_frames;
    o->fills = fills;
    o->isr_max_us = s_isr_max / (CPU_HZ / 1000000);
    o->slack_min_us = s_slack_min == INT32_MAX ? 0 : s_slack_min / (int32_t)(CPU_HZ / 1000000);
    o->late = s_late;
    o->isr_core = s_isr_core;
    if (reset) {
        s_frames = 0; s_fills = 0; s_isr_sum = 0; s_isr_max = 0; s_late = 0; s_slack_min = INT32_MAX;
    }
    portENABLE_INTERRUPTS();
    o->isr_avg_us10 = fills ? (uint32_t)(sum * 10 / fills / (CPU_HZ / 1000000)) : 0;
}

void vga_set_copy_mode(int mode) { s_copy_mode = mode; }
int vga_get_copy_mode(void) { return s_copy_mode; }
