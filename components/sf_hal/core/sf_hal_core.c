#include "sf_hal.h"
#include "sf_hal_display.h"
#include "sf_hal_board.h"
#include "sf_hal_input.h"
#include "esp_lvgl_port.h"

esp_err_t sf_hal_init(void)
{
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    /* Raise the LVGL task stack above the 7KB default. The default 7168 is tuned
       for simple demos; this project's full UI (phone shell + 3-page tabview +
       Tasks lv_table + Regions chart/bars + animations) overflows it during the
       generic draw dispatch (lv_draw_dispatch_layer) — see taskLVGL stack overflow
       crashes. 16KB gives comfortable headroom. [system-level config, user-approved] */
    lvgl_cfg.task_stack = 16384;
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    sf_display_config_t disp_cfg = {
        .width = CONFIG_SF_HAL_DISPLAY_WIDTH,
        .height = CONFIG_SF_HAL_DISPLAY_HEIGHT,
        .cmd_bits = CONFIG_SF_HAL_DISPLAY_CMD_BITS,
        .param_bits = CONFIG_SF_HAL_DISPLAY_PARAM_BITS,
        .pixel_clock_hz = CONFIG_SF_HAL_DISPLAY_PIXEL_CLOCK_HZ,
        .bl_on_level = CONFIG_SF_HAL_DISPLAY_BL_ON_LEVEL,
        .spi_host = CONFIG_SF_HAL_LCD_SPI_HOST,
        .bl_ledc_timer = CONFIG_SF_HAL_LCD_BL_LEDC_TIMER,
        .bl_ledc_channel = CONFIG_SF_HAL_LCD_BL_LEDC_CHANNEL,
    };

    ESP_ERROR_CHECK(sf_hal_board_init());
    ESP_ERROR_CHECK(sf_hal_display_init(&disp_cfg));
    ESP_ERROR_CHECK(sf_hal_input_init());

    return ESP_OK;
}

esp_err_t sf_hal_deinit(void)
{
    ESP_ERROR_CHECK(sf_hal_display_deinit());
    return ESP_OK;
}
