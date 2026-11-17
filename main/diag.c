// Self-test: measure the sync signals the chip is actually driving on its pins.
#include "driver/pulse_cnt.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "diag";

static pcnt_unit_handle_t make_counter(int gpio)
{
    pcnt_unit_config_t ucfg = { .low_limit = -1, .high_limit = 32767 };
    pcnt_unit_handle_t unit;
    ESP_ERROR_CHECK(pcnt_new_unit(&ucfg, &unit));
    pcnt_chan_config_t ccfg = { .edge_gpio_num = gpio, .level_gpio_num = -1 };
    pcnt_channel_handle_t ch;
    ESP_ERROR_CHECK(pcnt_new_channel(unit, &ccfg, &ch));
    // count falling edges only
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(ch, PCNT_CHANNEL_EDGE_ACTION_HOLD, PCNT_CHANNEL_EDGE_ACTION_INCREASE));
    ESP_ERROR_CHECK(pcnt_unit_enable(unit));
    return unit;
}

void diag_sync_check(int hs_gpio, int vs_gpio)
{
    static pcnt_unit_handle_t hs, vs;
    if (!hs) {
        hs = make_counter(hs_gpio);
        vs = make_counter(vs_gpio);
    }

    // Frequency: count falling edges over 500 ms (hsync would overflow 32767 in 1 s).
    pcnt_unit_clear_count(hs); pcnt_unit_clear_count(vs);
    pcnt_unit_start(hs); pcnt_unit_start(vs);
    vTaskDelay(pdMS_TO_TICKS(500));
    pcnt_unit_stop(hs); pcnt_unit_stop(vs);
    int h = 0, v = 0;
    pcnt_unit_get_count(hs, &h); pcnt_unit_get_count(vs, &v);

    // Polarity / duty: sample pin levels.
    int hlow = 0, vlow = 0;
    const int N = 200000;
    for (int i = 0; i < N; i++) {
        hlow += !gpio_get_level(hs_gpio);
        vlow += !gpio_get_level(vs_gpio);
    }
    ESP_LOGI(TAG, "HSYNC %d Hz , low %.1f%% of time ",
             h * 2, 100.0 * hlow / N);
    ESP_LOGI(TAG, "VSYNC %d Hz , low %.2f%% of time ",
             v * 2, 100.0 * vlow / N);
}
