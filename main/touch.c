// Touchscreen: the panel's GT911 on the shared I2C bus, polled (its interrupt and reset lines are
// not needed). The pointer is hidden while the screen is being touched and comes back when a mouse
// moves.
//
// One finger is the mouse: a tap is a left click, moving scrolls what is under it or else drags with
// the left button (the window manager decides, see handle_touch), and a finger held still is a right
// click. Which of these it is only shows once the finger has moved, lifted or waited, so nothing is
// pressed before that: a long touch must not click what is under it. Two fingers are a pinch, sent to
// whatever is under them (input_pinch), and never click anything.
#include <math.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "board.h"
#include "display.h"
#include "input.h"
#include "os.h"

#define TOUCH_SLOP      12      // pixels a finger may wander and still be holding still
#define LONG_TOUCH_MS   500
#define PINCH_MIN_DIST  32      // fingers closer than this make the ratio jump
#define PINCH_PERIOD_MS 30      // at most this often, so a slow window cannot fill the event queue
#define PINCH_END_MS    80      // the panel can lose a finger for a frame or two

static const char *TAG = "touch";
static esp_lcd_touch_handle_t s_tp;

typedef struct { int x, y; } finger_t;

static enum { T_IDLE, T_PENDING, T_DRAG, T_PINCH, T_WAIT_LIFT } s_state;
static int s_x0, s_y0, s_lx, s_ly;          // where the finger landed, and where it was last
static uint32_t s_t0;
static float s_pinch_d0, s_pinch_d;
static int s_pinch_x, s_pinch_y;
static uint32_t s_pinch_sent, s_pinch_seen;

// Test input from the serial console (the "touch" command): replaces the panel while it lasts.
static volatile int s_inject_n = -1;
static finger_t s_inject[2];

void touch_inject(int n, const int *xy)
{
    for (int i = 0; i < n; i++) { s_inject[i].x = xy[i * 2]; s_inject[i].y = xy[i * 2 + 1]; }
    s_inject_n = n;
}

static float finger_dist(const finger_t *f)
{
    float dx = f[0].x - f[1].x, dy = f[0].y - f[1].y;
    float d = sqrtf(dx * dx + dy * dy);
    return d < PINCH_MIN_DIST ? PINCH_MIN_DIST : d;
}

static void pinch_begin(const finger_t *f, uint32_t now)
{
    s_state = T_PINCH;
    s_pinch_d0 = s_pinch_d = finger_dist(f);
    s_pinch_x = (f[0].x + f[1].x) / 2;
    s_pinch_y = (f[0].y + f[1].y) / 2;
    s_pinch_sent = s_pinch_seen = now;
    input_pinch(s_pinch_x, s_pinch_y, 256, PINCH_BEGIN);
}

static void pinch_update(const finger_t *f, uint32_t now)
{
    s_pinch_seen = now;
    if (now - s_pinch_sent < PINCH_PERIOD_MS) return;
    float d = finger_dist(f);
    int x = (f[0].x + f[1].x) / 2, y = (f[0].y + f[1].y) / 2;
    if (fabsf(d - s_pinch_d) < 2 && abs(x - s_pinch_x) < 2 && abs(y - s_pinch_y) < 2) return;
    s_pinch_d = d; s_pinch_x = x; s_pinch_y = y;
    s_pinch_sent = now;
    float scale = d * 256 / s_pinch_d0;
    input_pinch(x, y, scale > 32767 ? 32767 : (int)scale, PINCH_MOVE);
}

// One reading of the panel: n fingers (at most two are looked at).
static void touch_frame(int n, const finger_t *f, uint32_t now)
{
    switch (s_state) {
    case T_IDLE:
        if (!n) break;
        s_x0 = s_lx = f[0].x; s_y0 = s_ly = f[0].y; s_t0 = now;
        s_state = T_PENDING;
        if (n >= 2) pinch_begin(f, now);
        break;
    case T_PENDING:
        if (n >= 2) { pinch_begin(f, now); break; }
        if (!n) {                               // a tap
            input_touch_gesture(TOUCH_TAP, s_x0, s_y0);
            s_state = T_IDLE;
        } else if (abs(f[0].x - s_x0) > TOUCH_SLOP || abs(f[0].y - s_y0) > TOUCH_SLOP) {
            input_touch_gesture(TOUCH_DRAG_BEGIN, s_x0, s_y0);  // from where the finger landed
            input_touch_gesture(TOUCH_DRAG_MOVE, f[0].x, f[0].y);
            s_lx = f[0].x; s_ly = f[0].y;
            s_state = T_DRAG;
        } else if (now - s_t0 >= LONG_TOUCH_MS) {
            input_touch_gesture(TOUCH_LONG, s_x0, s_y0);
            s_state = T_WAIT_LIFT;
        }
        break;
    case T_DRAG:
        if (!n) { input_touch_gesture(TOUCH_DRAG_END, s_lx, s_ly); s_state = T_IDLE; break; }
        if (f[0].x != s_lx || f[0].y != s_ly) input_touch_gesture(TOUCH_DRAG_MOVE, f[0].x, f[0].y);
        s_lx = f[0].x; s_ly = f[0].y;
        break;
    case T_PINCH:
        if (n >= 2) { pinch_update(f, now); break; }
        if (now - s_pinch_seen < PINCH_END_MS) break;
        input_pinch(s_pinch_x, s_pinch_y, (int)(s_pinch_d * 256 / s_pinch_d0), PINCH_END);
        s_state = n ? T_WAIT_LIFT : T_IDLE;     // the finger left behind must not become a click
        break;
    case T_WAIT_LIFT:
        if (!n) s_state = T_IDLE;
        break;
    }
}

static void touch_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(s_state != T_IDLE ? 8 : 16));
        finger_t f[2] = {0};
        int n = 0;
        if (s_inject_n >= 0) {
            n = s_inject_n;
            f[0] = s_inject[0]; f[1] = s_inject[1];
            if (!n) s_inject_n = -1;            // all fingers up: back to the panel
        } else {
            if (esp_lcd_touch_read_data(s_tp) != ESP_OK) continue;
            esp_lcd_touch_point_data_t pt[2];
            uint8_t got = 0;
            if (esp_lcd_touch_get_data(s_tp, pt, &got, 2) == ESP_OK) n = got;
            for (int i = 0; i < n; i++) { f[i].x = pt[i].x; f[i].y = pt[i].y; }
        }
        touch_frame(n, f, os_uptime_ms());
    }
}

void touch_start(void)
{
    i2c_master_bus_handle_t bus = board_i2c();
    if (!bus) return;
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.scl_speed_hz = 400000;
    if (esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io) != ESP_OK) { ESP_LOGE(TAG, "no I2C device"); return; }
    const esp_lcd_touch_config_t cfg = {
        .x_max = SCREEN_W,
        .y_max = SCREEN_H,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
    };
    if (esp_lcd_touch_new_i2c_gt911(io, &cfg, &s_tp) != ESP_OK) { ESP_LOGE(TAG, "GT911 not found: no touch input"); return; }
    xTaskCreatePinnedToCore(touch_task, "touch", 4096, NULL, 8, NULL, SYS_CORE);
    ESP_LOGI(TAG, "GT911 touch panel ready");
}
