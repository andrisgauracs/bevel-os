// Touchscreen: the panel's GT911 on the shared I2C bus, polled (its interrupt and reset lines are
// not needed). A finger acts as the left mouse button: touch = press, drag = move, lift = release.
// The pointer is hidden while the screen is being touched and comes back when a mouse moves.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "board.h"
#include "display.h"
#include "input.h"
#include "os.h"

static const char *TAG = "touch";
static esp_lcd_touch_handle_t s_tp;

static void touch_task(void *arg)
{
    bool down = false;
    int lx = 0, ly = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(down ? 8 : 16));
        if (esp_lcd_touch_read_data(s_tp) != ESP_OK) continue;
        esp_lcd_touch_point_data_t pt[1];
        uint8_t n = 0;
        bool pressed = esp_lcd_touch_get_data(s_tp, pt, &n, 1) == ESP_OK && n > 0;
        if (pressed) {
            int x = pt[0].x, y = pt[0].y;
            if (!down || x != lx || y != ly) input_touch(x, y, true);
            lx = x; ly = y;
        } else if (down) {
            input_touch(lx, ly, false);
        }
        down = pressed;
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
