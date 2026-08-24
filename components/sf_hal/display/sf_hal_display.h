#ifndef SF_HAL_DISPLAY_H
#define SF_HAL_DISPLAY_H

#include "esp_err.h"
#include "sf_hal.h"
#include "lvgl.h"

esp_err_t sf_hal_display_init(const sf_display_config_t *cfg);
esp_err_t sf_hal_display_deinit(void);
lv_display_t *sf_hal_display_get_lvgl_disp(void);

/** Set backlight brightness (0-100) */
esp_err_t sf_hal_display_set_brightness(uint8_t percent);

#endif
