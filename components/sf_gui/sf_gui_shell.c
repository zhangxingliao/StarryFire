#include "sf_gui.h"
#include "sf_gui_internal.h"
#include "sf_theme.h"
#include "sf_sys.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "sf_gui_shell";

static void (*s_go_home_cb)(void);

lv_obj_t *sf_gui_app_get_root(struct sf_app_ctx_t *ctx)
{
    return ctx ? (lv_obj_t *)ctx->ui_root : NULL;
}

void sf_gui_shell_go_home(void)
{
    if (s_go_home_cb) s_go_home_cb();
}

void sf_gui_set_go_home_cb(void (*cb)(void))
{
    s_go_home_cb = cb;
}

esp_err_t sf_gui_init(void)
{
    ESP_LOGI(TAG, "initializing GUI shell");

    lvgl_port_lock(0);

    /* Theme init must run before any UI widget is created */
    ESP_ERROR_CHECK(sf_theme_init());

#if CONFIG_SF_GUI_SHELL_PHONE
    ESP_LOGI(TAG, "shell: phone");
    ESP_ERROR_CHECK(sf_gui_phone_shell_init());
#elif CONFIG_SF_GUI_SHELL_WATCH
    ESP_LOGI(TAG, "shell: watch (not implemented)");
#else
#error "No GUI shell selected — configure CONFIG_SF_GUI_SHELL_PHONE or _WATCH"
#endif

    lvgl_port_unlock();

    ESP_LOGI(TAG, "GUI shell ready");
    return ESP_OK;
}
