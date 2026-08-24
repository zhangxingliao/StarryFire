#include "st7789.h"
#include "sf_hal_board.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "esp_lvgl_port_disp.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

static const char *TAG = "sf_st7789";

static esp_lcd_panel_io_handle_t io_handle = NULL;
static esp_lcd_panel_handle_t panel_handle = NULL;
static lv_display_t *lvgl_disp = NULL;

static spi_host_device_t s_spi_host = SPI3_HOST;     /* from board profile, set at init */
static ledc_channel_t s_bl_channel = LEDC_CHANNEL_0; /* from board profile, set at init */

static esp_err_t st7789_backlight_init(int gpio_num, int duty_pct,
                                       ledc_timer_t timer_id, ledc_channel_t channel)
{
    if (gpio_num < 0) return ESP_OK;

    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = timer_id,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    ledc_channel_config_t ledc_ch = {
        .gpio_num = gpio_num,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = timer_id,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_ch));

    s_bl_channel = channel;

    uint32_t duty = ((1 << 13) - 1) * duty_pct / 100;
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, channel));

    return ESP_OK;
}

esp_err_t st7789_set_brightness(uint8_t percent)
{
    if (percent > 100) percent = 100;
    uint32_t duty = ((1 << 13) - 1) * percent / 100;
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, s_bl_channel, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, s_bl_channel));
    return ESP_OK;
}

static esp_err_t st7789_send_init_sequence(esp_lcd_panel_io_handle_t io)
{
    const uint8_t gamma_positive[] = {
        0xD0, 0x0D, 0x14, 0x0D, 0x0D, 0x09, 0x38,
        0x44, 0x4E, 0x3A, 0x17, 0x18, 0x2F, 0x30
    };
    const uint8_t gamma_negative[] = {
        0xD0, 0x09, 0x0F, 0x08, 0x07, 0x14, 0x37,
        0x44, 0x4D, 0x38, 0x15, 0x16, 0x2C, 0x2E
    };

    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0x36, (uint8_t[]){0x00}, 1));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0x3A, (uint8_t[]){0x55}, 1));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xB0, (uint8_t[]){0x00, 0xE8}, 2));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xB2, (uint8_t[]){0x0C, 0x0C, 0x00, 0x33, 0x33}, 5));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xB7, (uint8_t[]){0x75}, 1));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xBB, (uint8_t[]){0x1A}, 1));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xC0, (uint8_t[]){0x0C}, 1));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xC2, (uint8_t[]){0x01, 0xFF}, 2));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xC3, (uint8_t[]){0x13}, 1));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xC4, (uint8_t[]){0x20}, 1));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xC6, (uint8_t[]){0x0F}, 1));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xD0, (uint8_t[]){0xA4}, 1));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xE0, gamma_positive, 14));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xE1, gamma_negative, 14));

    return ESP_OK;
}

esp_err_t st7789_init(const sf_display_config_t *cfg)
{
    const sf_board_pins_t *pins = sf_hal_board_get_pins();

    ESP_LOGI(TAG, "Initialize SPI bus");
    spi_bus_config_t buscfg = {
        .miso_io_num = pins->lcd_miso,
        .mosi_io_num = pins->lcd_mosi,
        .sclk_io_num = pins->lcd_sclk,
        .quadhd_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .max_transfer_sz = cfg->width * cfg->height * sizeof(uint16_t),
    };
    s_spi_host = (spi_host_device_t)cfg->spi_host;
    ESP_ERROR_CHECK(spi_bus_initialize(s_spi_host, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_spi_config_t iocfg = {
        .cs_gpio_num = pins->lcd_cs,
        .dc_gpio_num = pins->lcd_dc,
        .pclk_hz = cfg->pixel_clock_hz,
        .lcd_cmd_bits = cfg->cmd_bits,
        .lcd_param_bits = cfg->param_bits,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)s_spi_host, &iocfg, &io_handle));

    ESP_LOGI(TAG, "Install ST7789 panel driver");
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = pins->lcd_rst,
        .color_space = LCD_RGB_ENDIAN_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &panel_handle));

    ESP_LOGI(TAG, "Panel init with custom sequence");
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(st7789_send_init_sequence(io_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_ERROR_CHECK(st7789_backlight_init(pins->lcd_bl, 100,
                                          (ledc_timer_t)cfg->bl_ledc_timer,
                                          (ledc_channel_t)cfg->bl_ledc_channel));

    lvgl_port_display_cfg_t lvgl_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .hres = cfg->width,
        .vres = cfg->height,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .buffer_size = (int) cfg->width * cfg->height * 0.3,
        .double_buffer = true,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .full_refresh = false,
            .swap_bytes = false,
        },
        .trans_size = 0,
    };
    lvgl_disp = lvgl_port_add_disp(&lvgl_cfg);
    if (lvgl_disp == NULL) {
        ESP_LOGE(TAG, "Failed to add LVGL display");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t st7789_deinit(void)
{
    if (lvgl_disp) {
        lvgl_port_remove_disp(lvgl_disp);
        lvgl_disp = NULL;
    }
    if (panel_handle) {
        esp_lcd_panel_disp_on_off(panel_handle, false);
        esp_lcd_panel_del(panel_handle);
        panel_handle = NULL;
    }
    if (io_handle) {
        esp_lcd_panel_io_del(io_handle);
        io_handle = NULL;
    }
    spi_bus_free(s_spi_host);
    return ESP_OK;
}

lv_display_t *st7789_get_lvgl_display(void)
{
    return lvgl_disp;
}
