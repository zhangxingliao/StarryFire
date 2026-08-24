#ifndef SF_CST328_H
#define SF_CST328_H

#include "esp_err.h"
#include "lvgl.h"

typedef struct {
    int i2c_port;       /* I2C peripheral id (I2C_NUM_0=0 / I2C_NUM_1=1), board profile */
    int sda_gpio;
    int scl_gpio;
    int int_gpio;
    int rst_gpio;
    uint16_t width;
    uint16_t height;
} cst328_config_t;

esp_err_t cst328_init(const cst328_config_t *cfg);
esp_err_t cst328_deinit(void);
lv_indev_t *cst328_get_lvgl_indev(void);

#endif
