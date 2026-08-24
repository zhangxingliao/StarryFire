#include "sf_hal_display.h"
#include "sf_hal_board.h"

#if CONFIG_SF_HAL_DISPLAY_ST7789
#include "backends/st7789/st7789.h"
#endif

static lv_display_t *lvgl_disp = NULL;

esp_err_t sf_hal_display_init(const sf_display_config_t *cfg)
{
#if CONFIG_SF_HAL_DISPLAY_ST7789

    esp_err_t ret = st7789_init(cfg);
    if (ret == ESP_OK) lvgl_disp = st7789_get_lvgl_display();
    return ret;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t sf_hal_display_deinit(void)
{
#if CONFIG_SF_HAL_DISPLAY_ST7789
    return st7789_deinit();
#else
    return ESP_OK;
#endif
}

lv_display_t *sf_hal_display_get_lvgl_disp(void)
{
    return lvgl_disp ? lvgl_disp : lv_display_get_default();
}

esp_err_t sf_hal_display_set_brightness(uint8_t percent)
{
#if CONFIG_SF_HAL_DISPLAY_ST7789
    return st7789_set_brightness(percent);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
