// Board glue shared by several drivers: the one I2C bus (touch, clock chip, camera).
#include "board.h"
#include "esp_log.h"

#if BOARD_HAS_I2C

static i2c_master_bus_handle_t s_i2c;

i2c_master_bus_handle_t board_i2c(void)
{
    if (!s_i2c) {
        const i2c_master_bus_config_t cfg = {
            .i2c_port = -1, // any free port
            .sda_io_num = BOARD_PIN_I2C_SDA,
            .scl_io_num = BOARD_PIN_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        if (i2c_new_master_bus(&cfg, &s_i2c) != ESP_OK) {
            ESP_LOGE("board", "I2C bus could not be created");
            s_i2c = NULL;
        }
    }
    return s_i2c;
}

#endif  // BOARD_HAS_I2C
