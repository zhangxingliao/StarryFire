#include "sf_hal_input.h"
#include "sf_hal_board.h"
#if CONFIG_SF_HAL_INPUT_TOUCH_CST328
#include "cst328.h"
#endif

static const char *TAG = "sf_hal_input";

esp_err_t sf_hal_input_init(void)
{
#if CONFIG_SF_HAL_INPUT_TOUCH_CST328
    const sf_board_pins_t *pins = sf_hal_board_get_pins();

    cst328_config_t touch_cfg = {
        .i2c_port = CONFIG_SF_HAL_TOUCH_I2C_PORT,
        .sda_gpio = pins->touch_sda,
        .scl_gpio = pins->touch_scl,
        .int_gpio = pins->touch_int,
        .rst_gpio = pins->touch_rst,
        .width = CONFIG_SF_HAL_DISPLAY_WIDTH,
        .height = CONFIG_SF_HAL_DISPLAY_HEIGHT,
    };

    ESP_ERROR_CHECK(cst328_init(&touch_cfg));
    ESP_LOGI(TAG, "CST328 touch initialized");
#endif
    return ESP_OK;
}

esp_err_t sf_hal_input_deinit(void)
{
#if CONFIG_SF_HAL_INPUT_TOUCH_CST328
    ESP_ERROR_CHECK(cst328_deinit());
#endif
    return ESP_OK;
}
