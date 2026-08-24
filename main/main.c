#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sf_hal.h"
#include "sf_sys.h"
#include "sf_config.h"
#include "sf_gui.h"
#include "sf_app_settings.h"
#include "sf_app_monitor.h"
#include "esp_lvgl_port.h"
#include <time.h>
#include <stdlib.h>

static const char *TAG = "sf_main";

void app_main(void)
{
    ESP_LOGI(TAG, "StarryFire OS starting...");

    ESP_ERROR_CHECK(sf_hal_init());
    ESP_ERROR_CHECK(sf_sys_init());

    /* Apply the saved brightness setting */
    sf_hal_display_set_brightness((uint8_t)sf_config_get_brightness());

    /* Register apps (before GUI init, so the Launcher can enumerate all registered apps) */
    sf_app_register(&g_settings_app_manifest);
    sf_app_register(&g_monitor_app_manifest);

    ESP_ERROR_CHECK(sf_gui_init());

    ESP_LOGI(TAG, "Startup complete, LVGL task running");
    vTaskDelete(NULL);
}
