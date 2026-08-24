#ifndef SF_ST7789_H
#define SF_ST7789_H

#include "esp_err.h"
#include "sf_hal.h"
#include "lvgl.h"

esp_err_t st7789_init(const sf_display_config_t *cfg);
esp_err_t st7789_deinit(void);
lv_display_t *st7789_get_lvgl_display(void);

/** Set backlight brightness (0-100) */
esp_err_t st7789_set_brightness(uint8_t percent);

#endif
