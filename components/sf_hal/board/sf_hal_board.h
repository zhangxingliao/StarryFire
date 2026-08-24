#ifndef SF_HAL_BOARD_H
#define SF_HAL_BOARD_H

#include "esp_err.h"
#include "sf_hal.h"

esp_err_t sf_hal_board_init(void);
const sf_board_pins_t *sf_hal_board_get_pins(void);

#endif
