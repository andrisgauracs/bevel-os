// Display driver for the Guition JC1060P470-V2: 1024x600 JD9165 panel on MIPI-DSI (2 lanes).
//
// The DSI controller scans its own RGB565 framebuffer (in PSRAM) out by DMA, so there is no
// per-line interrupt to feed as on the VGA build. The OS draws RGB565 too; disp_put() keeps a copy in
// the logical framebuffer, writes the rows to the panel's framebuffer and flushes the data cache for
// them. The mouse pointer and the true-colour overlays are merged in on the way (see display.h).
//
// Panel init sequence and timings: the board vendor's demo, as used in the esp32_usb_cd_player and
// p4_edge_enhancer projects (docs/JC1060P470-V2.md).
#include <string.h>
#include "display.h"
#include "board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/ledc.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_jd9165.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_attr.h"
#include "esp_log.h"

static const char *TAG = "display";

#define BL_LEDC_TIMER   LEDC_TIMER_1
#define BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define BL_DUTY_BITS    LEDC_TIMER_10_BIT

// JD9165 init sequence for this panel (vendor)
static const jd9165_lcd_init_cmd_t s_lcd_cmd[] = {
    {0x30, (uint8_t[]){0x00}, 1, 0},
    {0xF7, (uint8_t[]){0x49,0x61,0x02,0x00}, 4, 0},
    {0x30, (uint8_t[]){0x01}, 1, 0},
    {0x04, (uint8_t[]){0x0C}, 1, 0},
    {0x05, (uint8_t[]){0x08}, 1, 0},
    {0x0B, (uint8_t[]){0x11}, 1, 0},
    {0x20, (uint8_t[]){0x04}, 1, 0},
    {0x1F, (uint8_t[]){0x05}, 1, 0},
    {0x23, (uint8_t[]){0x38}, 1, 0},
    {0x28, (uint8_t[]){0x18}, 1, 0},
    {0x29, (uint8_t[]){0x29}, 1, 0},
    {0x2A, (uint8_t[]){0x01}, 1, 0},
    {0x2B, (uint8_t[]){0x29}, 1, 0},
    {0x2C, (uint8_t[]){0x01}, 1, 0},
    {0x30, (uint8_t[]){0x02}, 1, 0},
    {0x00, (uint8_t[]){0x05}, 1, 0},
    {0x01, (uint8_t[]){0x22}, 1, 0},
    {0x02, (uint8_t[]){0x08}, 1, 0},
    {0x03, (uint8_t[]){0x12}, 1, 0},
    {0x04, (uint8_t[]){0x16}, 1, 0},
    {0x05, (uint8_t[]){0x64}, 1, 0},
    {0x06, (uint8_t[]){0x00}, 1, 0},
    {0x07, (uint8_t[]){0x00}, 1, 0},
    {0x08, (uint8_t[]){0x78}, 1, 0},
    {0x09, (uint8_t[]){0x00}, 1, 0},
    {0x0A, (uint8_t[]){0x04}, 1, 0},
    {0x0B, (uint8_t[]){0x16,0x17,0x0B,0x0D,0x0D,0x0D,0x11,0x10,0x07,0x07,0x09}, 11, 0},
    {0x0C, (uint8_t[]){0x09,0x1E,0x1E,0x1C,0x1C,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D}, 11, 0},
    {0x0D, (uint8_t[]){0x0A,0x05,0x0B,0x0D,0x0D,0x0D,0x11,0x10,0x06,0x06,0x08}, 11, 0},
    {0x0E, (uint8_t[]){0x08,0x1F,0x1F,0x1D,0x1D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D}, 11, 0},
    {0x0F, (uint8_t[]){0x0A,0x05,0x0D,0x0B,0x0D,0x0D,0x11,0x10,0x1D,0x1D,0x1F}, 11, 0},
    {0x10, (uint8_t[]){0x1F,0x08,0x08,0x06,0x06,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D}, 11, 0},
    {0x11, (uint8_t[]){0x16,0x17,0x0D,0x0B,0x0D,0x0D,0x11,0x10,0x1C,0x1C,0x1E}, 11, 0},
    {0x12, (uint8_t[]){0x1E,0x09,0x09,0x07,0x07,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D}, 11, 0},
    {0x13, (uint8_t[]){0x00,0x00,0x00,0x00}, 4, 0},
    {0x14, (uint8_t[]){0x00,0x00,0x41,0x41}, 4, 0},
    {0x15, (uint8_t[]){0x00,0x00,0x00,0x00}, 4, 0},
    {0x17, (uint8_t[]){0x00}, 1, 0},
    {0x18, (uint8_t[]){0x85}, 1, 0},
    {0x19, (uint8_t[]){0x06,0x09}, 2, 0},
    {0x1A, (uint8_t[]){0x05,0x08}, 2, 0},
    {0x1B, (uint8_t[]){0x0A,0x04}, 2, 0},
    {0x26, (uint8_t[]){0x00}, 1, 0},
    {0x27, (uint8_t[]){0x00}, 1, 0},
    {0x30, (uint8_t[]){0x06}, 1, 0},
    {0x12, (uint8_t[]){0x3F,0x25,0x27,0x35,0x1D,0x1B,0x1B,0x1A,0x18,0x0A,0x2A,0x21,0x19,0x30}, 14, 0},
    {0x13, (uint8_t[]){0x3F,0x26,0x27,0x35,0x1E,0x1C,0x1C,0x1A,0x18,0x0B,0x2A,0x21,0x19,0x30}, 14, 0},
    {0x30, (uint8_t[]){0x0A}, 1, 0},
    {0x02, (uint8_t[]){0x4F}, 1, 0},
    {0x0B, (uint8_t[]){0x40}, 1, 0},
    {0x30, (uint8_t[]){0x0D}, 1, 0},
    {0x0D, (uint8_t[]){0x04}, 1, 0},
    {0x10, (uint8_t[]){0x05}, 1, 0},
    {0x11, (uint8_t[]){0x0C}, 1, 0},
    {0x12, (uint8_t[]){0x05}, 1, 0},
    {0x13, (uint8_t[]){0x0C}, 1, 0},
    {0x30, (uint8_t[]){0x00}, 1, 0},

    {0x3A, (uint8_t[]){0x55}, 1, 0},    // 16 bit, RGB565
    {0x11, (uint8_t[]){0x00}, 1, 120},  // sleep out, 120 ms
    {0x29, (uint8_t[]){0x00}, 1, 20},   // display on, 20 ms
};

static pixel_t *s_fb;           // logical framebuffer: the picture without pointer and overlays
static uint16_t *s_pfb;         // the panel driver's framebuffer, what is really shown
uint16_t disp_rgb222_lut[256];  // palette byte -> pixel; 0x40 and up are the overlay keys
static SemaphoreHandle_t s_lock;
static int s_brightness = -1;

// --- pointer sprite ---
#define CUR_MAX_W 16
#define CUR_MAX_H 20
static uint8_t s_cur_px[CUR_MAX_W * CUR_MAX_H];
static int s_cur_x, s_cur_y;            // top-left of the sprite on screen
static int s_cur_w, s_cur_h, s_cur_hx, s_cur_hy;
static bool s_cur_on;

// --- overlays ---
typedef struct { uint16_t *px; int w, h, x, y; } overlay_t;
static overlay_t s_ov[DISP_OVERLAYS];
static int s_ov_count;

// ---------------------------------------------------------------- output

static inline bool is_key(pixel_t v) { return (v & ~(pixel_t)(DISP_OVERLAYS - 1)) == DISP_OVERLAY_KEY_BASE; }

// Rows of the picture into the panel framebuffer, with overlays and pointer. Caller holds s_lock.
static void IRAM_ATTR convert(int x, int y, int w, int h, const pixel_t *src, int stride)
{
    const uint16_t *lut = disp_rgb222_lut;
    for (int r = 0; r < h; r++, src += stride) {
        int yy = y + r;
        uint16_t *d = s_pfb + yy * SCREEN_W + x;
        if (!s_ov_count) {
            memcpy(d, src, (size_t)w * 2);
        } else {
            // a run of one overlay's key colour is copied from that overlay's row in one go (black
            // where the key reaches past the surface); a pixel at a time cost a live camera picture
            // 34 ms a frame
            for (int i = 0; i < w;) {
                pixel_t v = src[i];
                int j = i + 1;
                if (!is_key(v)) {
                    while (j < w && !is_key(src[j])) j++;
                    memcpy(d + i, src + i, (size_t)(j - i) * 2);
                    i = j;
                    continue;
                }
                while (j < w && src[j] == v) j++;
                const overlay_t *o = &s_ov[v & (DISP_OVERLAYS - 1)];
                int oy = yy - o->y;
                int lo = o->x - x, hi = lo + o->w;          // the surface's columns, in this row's terms
                int a = i > lo ? i : lo, b = j < hi ? j : hi;
                if (!o->px || oy < 0 || oy >= o->h || a >= b) {
                    memset(d + i, 0, (j - i) * 2);
                } else {
                    if (a > i) memset(d + i, 0, (a - i) * 2);
                    memcpy(d + a, o->px + oy * o->w + (a - lo), (b - a) * 2);
                    if (j > b) memset(d + b, 0, (j - b) * 2);
                }
                i = j;
            }
        }
        if (s_cur_on && yy >= s_cur_y && yy < s_cur_y + s_cur_h) {
            const uint8_t *c = s_cur_px + (yy - s_cur_y) * CUR_MAX_W;
            int xa = s_cur_x > x ? s_cur_x : x;
            int xb = s_cur_x + s_cur_w < x + w ? s_cur_x + s_cur_w : x + w;
            for (int xx = xa; xx < xb; xx++) {
                uint8_t p = c[xx - s_cur_x];
                if (p != 0xFF) d[xx - x] = lut[p];
            }
        }
    }
    // the DSI DMA reads the PSRAM itself, not our cached view of it
    uint16_t *first = s_pfb + y * SCREEN_W + x;
    esp_cache_msync(first, ((h - 1) * SCREEN_W + w) * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static bool clip(int *x, int *y, int *w, int *h)
{
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > SCREEN_W) *w = SCREEN_W - *x;
    if (*y + *h > SCREEN_H) *h = SCREEN_H - *y;
    return *w > 0 && *h > 0;
}

void IRAM_ATTR disp_put(int x, int y, int w, int h, const pixel_t *src, int stride)
{
    if (!s_pfb || x < 0 || y < 0 || x + w > SCREEN_W || y + h > SCREEN_H || w <= 0 || h <= 0) return;
    pixel_t *d = s_fb + y * SCREEN_W + x;
    const pixel_t *s = src;
    for (int r = 0; r < h; r++, d += SCREEN_W, s += stride) memcpy(d, s, (size_t)w * 2);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    convert(x, y, w, h, src, stride);
    xSemaphoreGive(s_lock);
}

static void present_locked(int x, int y, int w, int h)
{
    if (clip(&x, &y, &w, &h)) convert(x, y, w, h, s_fb + y * SCREEN_W + x, SCREEN_W);
}

void disp_present(int x, int y, int w, int h)
{
    if (!s_pfb) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    present_locked(x, y, w, h);
    xSemaphoreGive(s_lock);
}

pixel_t *disp_fb(void) { return s_fb; }

// What the panel shows, copied out for a screen dump. The pointer is part of the panel framebuffer
// (it is drawn during the conversion), so leaving it out means having it repainted first.
void disp_snapshot_rows(uint16_t *dst, int y0, int rows, bool hide_cursor)
{
    if (!s_pfb || !dst || y0 < 0 || rows <= 0 || y0 + rows > SCREEN_H) return;
    bool shown;
    disp_cursor_get(NULL, NULL, &shown);
    if (hide_cursor && shown) disp_cursor_show(false);
    memcpy(dst, s_pfb + (size_t)y0 * SCREEN_W, (size_t)SCREEN_W * rows * sizeof(uint16_t));
    if (hide_cursor && shown) disp_cursor_show(true);
}

void disp_snapshot(uint16_t *dst, bool hide_cursor) { disp_snapshot_rows(dst, 0, SCREEN_H, hide_cursor); }

// ---------------------------------------------------------------- pointer

void disp_cursor_shape(const uint8_t *px, int w, int h, int hot_x, int hot_y)
{
    if (w > CUR_MAX_W) w = CUR_MAX_W;
    if (h > CUR_MAX_H) h = CUR_MAX_H;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    int ox = s_cur_x, oy = s_cur_y;
    int px_x = s_cur_x + s_cur_hx, px_y = s_cur_y + s_cur_hy; // the pointer position stays put
    memset(s_cur_px, 0xFF, sizeof(s_cur_px));
    for (int y = 0; y < h; y++) memcpy(s_cur_px + y * CUR_MAX_W, px + y * w, w);
    s_cur_w = w; s_cur_h = h; s_cur_hx = hot_x; s_cur_hy = hot_y;
    s_cur_x = px_x - hot_x; s_cur_y = px_y - hot_y;
    if (s_pfb && s_cur_on) {
        present_locked(ox, oy, CUR_MAX_W, CUR_MAX_H);
        present_locked(s_cur_x, s_cur_y, CUR_MAX_W, CUR_MAX_H);
    }
    if (s_lock) xSemaphoreGive(s_lock);
}

void disp_cursor_move(int x, int y)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    int ox = s_cur_x, oy = s_cur_y;
    s_cur_x = x - s_cur_hx;
    s_cur_y = y - s_cur_hy;
    if (s_pfb && s_cur_on && (ox != s_cur_x || oy != s_cur_y)) {
        present_locked(ox, oy, s_cur_w, s_cur_h);           // puts the picture back, and the pointer where they overlap
        present_locked(s_cur_x, s_cur_y, s_cur_w, s_cur_h);
    }
    if (s_lock) xSemaphoreGive(s_lock);
}

void disp_cursor_show(bool on)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    if (on != s_cur_on) {
        s_cur_on = on;
        if (s_pfb) present_locked(s_cur_x, s_cur_y, s_cur_w, s_cur_h);
    }
    if (s_lock) xSemaphoreGive(s_lock);
}

void disp_cursor_get(int *x, int *y, bool *shown)
{
    if (x) *x = s_cur_x + s_cur_hx;
    if (y) *y = s_cur_y + s_cur_hy;
    if (shown) *shown = s_cur_on;
}

// ---------------------------------------------------------------- overlays

int disp_overlay_alloc(int w, int h)
{
    if (w <= 0 || h <= 0) return -1;
    uint16_t *px = heap_caps_calloc(w * h, 2, MALLOC_CAP_SPIRAM);
    if (!px) return -1;
    int id = -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < DISP_OVERLAYS; i++) {
        if (!s_ov[i].px) { s_ov[i] = (overlay_t){px, w, h, SCREEN_W, SCREEN_H}; s_ov_count++; id = i; break; }
    }
    xSemaphoreGive(s_lock);
    if (id < 0) free(px);
    return id;
}

uint16_t *disp_overlay_px(int id, int *w, int *h)
{
    if (id < 0 || id >= DISP_OVERLAYS || !s_ov[id].px) return NULL;
    if (w) *w = s_ov[id].w;
    if (h) *h = s_ov[id].h;
    return s_ov[id].px;
}

void disp_overlay_place(int id, int x, int y)
{
    if (id < 0 || id >= DISP_OVERLAYS) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ov[id].x = x; s_ov[id].y = y;
    xSemaphoreGive(s_lock);
}

void disp_overlay_update(int id)
{
    if (id < 0 || id >= DISP_OVERLAYS || !s_pfb) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_ov[id].px) present_locked(s_ov[id].x, s_ov[id].y, s_ov[id].w, s_ov[id].h);
    xSemaphoreGive(s_lock);
}

void disp_overlay_free(int id)
{
    if (id < 0 || id >= DISP_OVERLAYS || !s_ov[id].px) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint16_t *px = s_ov[id].px;
    s_ov[id].px = NULL;
    s_ov_count--;
    xSemaphoreGive(s_lock);
    free(px);
}

// ---------------------------------------------------------------- backlight

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BL_DUTY_BITS,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");
    const ledc_channel_config_t ch = {
        .gpio_num = BOARD_PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_LEDC_CHANNEL,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
    };
    return ledc_channel_config(&ch);
}

void disp_set_brightness(int pct)
{
    pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    if (pct == s_brightness) return;
    // quadratic: the eye's response is far from linear, so the low levels need small steps
    const uint32_t max = (1u << BL_DUTY_BITS) - 1;
    uint32_t duty = pct ? (max * pct * pct + 9999) / 10000 : 0;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL);
    s_brightness = pct;
}

int disp_get_brightness(void) { return s_brightness < 0 ? 0 : s_brightness; }

// ---------------------------------------------------------------- init

esp_err_t disp_init(void)
{
    static const uint8_t lv5[4] = {0, 10, 21, 31}, lv6[4] = {0, 21, 42, 63};
    for (int i = 0; i < 256; i++)
        disp_rgb222_lut[i] = i < 0x40 ? lv5[i & 3] << 11 | lv6[(i >> 2) & 3] << 5 | lv5[(i >> 4) & 3]
                                      : DISP_OVERLAY_KEY(i & (DISP_OVERLAYS - 1));
    s_lock = xSemaphoreCreateMutex();
    s_fb = heap_caps_aligned_calloc(64, SCREEN_W * SCREEN_H, sizeof(pixel_t), MALLOC_CAP_SPIRAM);
    if (!s_fb || !s_lock) return ESP_ERR_NO_MEM;

    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");

    // MIPI D-PHY supply
    esp_ldo_channel_handle_t ldo = NULL;
    const esp_ldo_channel_config_t ldo_cfg = {.chan_id = BOARD_MIPI_LDO_CHAN, .voltage_mv = BOARD_MIPI_LDO_MV};
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &ldo), TAG, "MIPI LDO");

    esp_lcd_dsi_bus_handle_t dsi = NULL;
    const esp_lcd_dsi_bus_config_t bus_cfg = JD9165_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &dsi), TAG, "DSI bus");
    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_dbi_io_config_t dbi_cfg = {.virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8};
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi, &dbi_cfg, &io), TAG, "DBI io");

    const esp_lcd_dpi_panel_config_t dpi_cfg = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 52,
        .virtual_channel = 0,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .video_timing = {
            .h_size = SCREEN_W,
            .v_size = SCREEN_H,
            .hsync_back_porch = 136,
            .hsync_pulse_width = 24,
            .hsync_front_porch = 160,
            .vsync_back_porch = 21,
            .vsync_pulse_width = 2,
            .vsync_front_porch = 12,
        },
    };
    const jd9165_vendor_config_t vendor_cfg = {
        .init_cmds = s_lcd_cmd,
        .init_cmds_size = sizeof(s_lcd_cmd) / sizeof(s_lcd_cmd[0]),
        .mipi_config = {.dsi_bus = dsi, .dpi_config = &dpi_cfg},
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_cfg,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9165(io, &panel_cfg, &panel), TAG, "JD9165");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "init");
    // the JD9165 driver returns the DPI panel itself
    void *fb = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb), TAG, "frame buffer");
    s_pfb = fb;
    disp_present(0, 0, SCREEN_W, SCREEN_H);
    disp_set_brightness(80);
    ESP_LOGI(TAG, "%dx%d panel up", SCREEN_W, SCREEN_H);
    return ESP_OK;
}
