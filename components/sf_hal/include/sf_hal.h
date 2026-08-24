#ifndef SF_HAL_H
#define SF_HAL_H

#include <stdbool.h>
#include "esp_log.h"
#include "esp_err.h"

typedef struct {
    int lcd_mosi;
    int lcd_miso;
    int lcd_sclk;
    int lcd_cs;
    int lcd_dc;
    int lcd_rst;
    int lcd_bl;
    int touch_sda;
    int touch_scl;
    int touch_int;
    int touch_rst;
    int btn_back;
    int btn_home;
    int i2c0_sda;
    int i2c0_scl;
    int i2s_bclk;
    int i2s_lrck;
    int i2s_din;
    int sd_clk;
    int sd_cmd;
    int sd_d0;
    int pwr_key_in;
    int pwr_ctrl_out;
    int bat_adc;
} sf_board_pins_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t cmd_bits;
    uint8_t param_bits;
    uint32_t pixel_clock_hz;
    int bl_on_level;
    int spi_host;        /* LCD SPI peripheral id (SPI2_HOST=1 / SPI3_HOST=2), board profile */
    int bl_ledc_timer;   /* backlight LEDC timer id, board profile */
    int bl_ledc_channel; /* backlight LEDC channel, board profile */
} sf_display_config_t;

esp_err_t sf_hal_init(void);
esp_err_t sf_hal_deinit(void);

const sf_board_pins_t *sf_hal_board_get_pins(void);
esp_err_t sf_hal_display_init(const sf_display_config_t *cfg);
esp_err_t sf_hal_display_set_brightness(uint8_t percent);

#endif
