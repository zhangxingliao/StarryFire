#include "cst328.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

#define CST328_I2C_ADDR        0x1A
#define CST328_I2C_FREQ_HZ     400000
#define CST328_I2C_TIMEOUT_MS  50

#define CST328_REG_MODE_NORMAL 0xD109
#define CST328_REG_TOUCH_DATA  0xD000
#define CST328_TOUCH_BUF_SIZE  27

#define CST328_TOUCH_STATUS_PRESSED 0x06

static const char *TAG = "sf_cst328";

static i2c_master_bus_handle_t bus_handle = NULL;
static i2c_master_dev_handle_t dev_handle = NULL;
static lv_indev_t *lvgl_indev = NULL;

static esp_err_t cst328_write_cmd(uint16_t reg)
{
    uint8_t buf[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    return i2c_master_transmit(dev_handle, buf, 2, CST328_I2C_TIMEOUT_MS);
}

static esp_err_t cst328_read_regs(uint16_t reg, uint8_t *buf, size_t len)
{
    uint8_t reg_buf[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    return i2c_master_transmit_receive(dev_handle, reg_buf, 2, buf, len, CST328_I2C_TIMEOUT_MS);
}

static void cst328_lvgl_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint8_t buf[CST328_TOUCH_BUF_SIZE];
    esp_err_t ret = cst328_read_regs(CST328_REG_TOUCH_DATA, buf, sizeof(buf));
    if (ret != ESP_OK) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    uint8_t touch_count = buf[5] & 0x0F;
    if (touch_count == 0) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    uint8_t status = buf[0] & 0x0F;
    if (status != CST328_TOUCH_STATUS_PRESSED) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    uint16_t x = ((uint16_t)buf[1] << 4) | ((buf[3] >> 4) & 0x0F);
    uint16_t y = ((uint16_t)buf[2] << 4) | (buf[3] & 0x0F);

    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
}

static void cst328_reset(int rst_gpio)
{
    gpio_set_level(rst_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(rst_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

esp_err_t cst328_init(const cst328_config_t *cfg)
{
    ESP_LOGI(TAG, "Initializing on I2C (SDA=%d, SCL=%d, RST=%d, INT=%d)",
             cfg->sda_gpio, cfg->scl_gpio, cfg->rst_gpio, cfg->int_gpio);

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = (i2c_port_num_t)cfg->i2c_port,
        .sda_io_num = cfg->sda_gpio,
        .scl_io_num = cfg->scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus_handle));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CST328_I2C_ADDR,
        .scl_speed_hz = CST328_I2C_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_cfg, &dev_handle));

    if (cfg->rst_gpio >= 0) {
        gpio_config_t rst_gpio_cfg = {
            .pin_bit_mask = (1ULL << cfg->rst_gpio),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&rst_gpio_cfg);
        cst328_reset(cfg->rst_gpio);
    }

    esp_err_t ret = cst328_write_cmd(CST328_REG_MODE_NORMAL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set normal mode: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    lvgl_port_lock(0);
    lv_indev_t *indev = lv_indev_create();
    if (indev == NULL) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "Failed to create LVGL indev");
        return ESP_FAIL;
    }
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, cst328_lvgl_read_cb);
    lv_display_t *disp = lv_disp_get_default();
    if (disp) {
        lv_indev_set_display(indev, disp);
    }
    lvgl_indev = indev;
    lvgl_port_unlock();

    ESP_LOGI(TAG, "CST328 initialized");
    return ESP_OK;
}

esp_err_t cst328_deinit(void)
{
    if (lvgl_indev) {
        lvgl_port_lock(0);
        lv_indev_delete(lvgl_indev);
        lvgl_port_unlock();
        lvgl_indev = NULL;
    }
    if (dev_handle) {
        i2c_master_bus_rm_device(dev_handle);
        dev_handle = NULL;
    }
    if (bus_handle) {
        i2c_del_master_bus(bus_handle);
        bus_handle = NULL;
    }
    return ESP_OK;
}

lv_indev_t *cst328_get_lvgl_indev(void)
{
    return lvgl_indev;
}
