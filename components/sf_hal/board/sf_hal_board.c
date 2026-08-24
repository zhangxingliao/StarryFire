#include "sf_hal_board.h"

static const char *TAG = "sf_hal_board";

static const sf_board_pins_t board_pins = {
    .lcd_mosi   = CONFIG_SF_PIN_LCD_MOSI,
    .lcd_miso   = CONFIG_SF_PIN_LCD_MISO,
    .lcd_sclk   = CONFIG_SF_PIN_LCD_SCLK,
    .lcd_cs     = CONFIG_SF_PIN_LCD_CS,
    .lcd_dc     = CONFIG_SF_PIN_LCD_DC,
    .lcd_rst    = CONFIG_SF_PIN_LCD_RST,
    .lcd_bl     = CONFIG_SF_PIN_LCD_BL,
    .touch_sda  = CONFIG_SF_PIN_TOUCH_SDA,
    .touch_scl  = CONFIG_SF_PIN_TOUCH_SCL,
    .touch_int  = CONFIG_SF_PIN_TOUCH_INT,
    .touch_rst  = CONFIG_SF_PIN_TOUCH_RST,
    .btn_back   = CONFIG_SF_PIN_BTN_BACK,
    .btn_home   = CONFIG_SF_PIN_BTN_HOME,
    .i2c0_sda   = CONFIG_SF_PIN_I2C0_SDA,
    .i2c0_scl   = CONFIG_SF_PIN_I2C0_SCL,
    .i2s_bclk   = CONFIG_SF_PIN_I2S_BCLK,
    .i2s_lrck   = CONFIG_SF_PIN_I2S_LRCK,
    .i2s_din    = CONFIG_SF_PIN_I2S_DIN,
    .sd_clk     = CONFIG_SF_PIN_SD_CLK,
    .sd_cmd     = CONFIG_SF_PIN_SD_CMD,
    .sd_d0      = CONFIG_SF_PIN_SD_D0,
    .pwr_key_in = CONFIG_SF_PIN_PWR_KEY,
    .pwr_ctrl_out = CONFIG_SF_PIN_PWR_CTRL,
    .bat_adc    = CONFIG_SF_PIN_BAT_ADC,
};

esp_err_t sf_hal_board_init(void)
{
    ESP_LOGI(TAG, "Board pins initialized");
    return ESP_OK;
}

const sf_board_pins_t *sf_hal_board_get_pins(void)
{
    return &board_pins;
}
