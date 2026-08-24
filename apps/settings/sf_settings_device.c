/**
 * sf_settings_device.c — Device Info settings page
 *
 * Displays device information (serial number, uptime, firmware version)
 * and offers the OTA upgrade entry point. The UI matches the Local page
 * style: category cards + icon rows + inline separators.
 */
#include "sf_settings_pages.h"
#include "sf_sys.h"
#include "sf_gui.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_system.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"
#include "sf_theme.h"

static const char *TAG = "sf_settings_device";

#ifdef CONFIG_FIRMWARE_OTA_URL
#define OTA_FIRMWARE_URL CONFIG_FIRMWARE_OTA_URL
#define OTA_CONFIGURED   1
#else
#define OTA_FIRMWARE_URL ""
#define OTA_CONFIGURED   0
#endif

typedef struct {
    lv_obj_t *status_lbl;
    lv_obj_t *uptime_lbl;
    lv_timer_t *uptime_timer;
} device_priv_t;

static void format_uptime(char *buf, size_t len)
{
    uint64_t us = esp_timer_get_time();
    uint64_t s = us / 1000000;
    uint64_t d = s / 86400; s %= 86400;
    uint64_t h = s / 3600;  s %= 3600;
    uint64_t m = s / 60;    s %= 60;

    /* Start from the highest non-zero unit: most significant unit is not zero-padded, lower units padded to two digits */
    if (d > 0) {
        snprintf(buf, len, "%lluday %02llu:%02llu:%02llu", d, h, m, s);
    } else if (h > 0) {
        snprintf(buf, len, "%llu:%02llu:%02llu", h, m, s);
    } else if (m > 0) {
        snprintf(buf, len, "%llu:%02llu", m, s);
    } else {
        snprintf(buf, len, "%llus", s);
    }
}

static void get_mac_str(char *buf, size_t len)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, len, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ── Info row (icon + label + value on the right, same style as the Local page) ── */
static lv_obj_t *create_info_row(lv_obj_t *parent, const char *icon,
                                 const char *label, const char *value,
                                 lv_obj_t **value_out)
{
    lv_obj_t *item = lv_obj_create(parent);
    lv_obj_remove_style_all(item);
    lv_obj_set_width(item, LV_PCT(100));
    lv_obj_set_height(item, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(item, SF_UI(8), 0);
    lv_obj_set_style_pad_bottom(item, SF_UI(8), 0);
    lv_obj_set_style_pad_left(item, SF_UI(16), 0);
    lv_obj_set_style_pad_right(item, SF_UI(16), 0);
    lv_obj_set_style_pad_column(item, SF_UI(4), 0);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon_lbl = lv_label_create(item);
    lv_label_set_text(icon_lbl, icon);
    lv_obj_add_style(icon_lbl, sf_theme_get_style(SF_STYLE_TXT_ACCENT), 0);
    lv_obj_set_style_text_font(icon_lbl, SF_FONT_SM, 0);
    lv_obj_set_width(icon_lbl, SF_UI(20));

    lv_obj_t *txt = lv_label_create(item);
    lv_label_set_text(txt, label);
    lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_add_style(txt, sf_theme_get_style(SF_STYLE_TXT_SECONDARY), 0);
    lv_obj_set_style_text_font(txt, SF_FONT_XS, 0);
    lv_obj_set_flex_grow(txt, 1);

    lv_obj_t *val = lv_label_create(item);
    lv_label_set_text(val, value);
    lv_label_set_long_mode(val, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_add_style(val, sf_theme_get_style(SF_STYLE_TXT_PRIMARY), 0);
    lv_obj_set_style_text_font(val, SF_FONT_SM, 0);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);

    if (value_out) *value_out = val;

    return item;
}

static void back_click_cb(lv_event_t *e)
{
    settings_ctx_t *ctx = lv_event_get_user_data(e);
    settings_show_main(ctx);
}

/* ── Uptime live-refresh timer (1s) ── */
static void uptime_timer_cb(lv_timer_t *t)
{
    device_priv_t *priv = lv_timer_get_user_data(t);
    if (!priv || !priv->uptime_lbl) return;

    char uptime[32];
    format_uptime(uptime, sizeof(uptime));
    lv_label_set_text(priv->uptime_lbl, uptime);
}

/* ── Page delete callback (cleans up the timer) ── */
static void device_page_delete_cb(lv_event_t *e)
{
    lv_obj_t *page = lv_event_get_target(e);
    device_priv_t *priv = lv_obj_get_user_data(page);
    if (!priv) return;

    if (priv->uptime_timer) {
        lv_timer_del(priv->uptime_timer);
        priv->uptime_timer = NULL;
    }

    free(priv);
    lv_obj_set_user_data(page, NULL);
    ESP_LOGI(TAG, "device page destroyed");
}

static void ota_task(void *arg)
{
    device_priv_t *priv = arg;

    lvgl_port_lock(0);
    lv_label_set_text(priv->status_lbl, "Connecting...");
    lv_obj_clear_flag(priv->status_lbl, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();

    esp_http_client_config_t http_cfg = {
        .url = OTA_FIRMWARE_URL,
        .timeout_ms = 10000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t ret = esp_https_ota(&ota_cfg);
    if (ret == ESP_OK) {
        lvgl_port_lock(0);
        lv_label_set_text(priv->status_lbl, "Upgrade complete, restarting...");
        lvgl_port_unlock();
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    } else {
        lvgl_port_lock(0);
        lv_label_set_text(priv->status_lbl, "Upgrade failed, tap to retry");
        lvgl_port_unlock();
    }

    vTaskDelete(NULL);
}

static void ota_click_cb(lv_event_t *e)
{
    device_priv_t *priv = lv_event_get_user_data(e);
    xTaskCreate(ota_task, "settings_ota", 8192, priv, 5, NULL);
}

lv_obj_t *sf_settings_device_create(lv_obj_t *parent, settings_ctx_t *ctx)
{
    device_priv_t *priv = calloc(1, sizeof(device_priv_t));
    if (!priv) return NULL;

    lvgl_port_lock(0);

    lv_obj_t *page = settings_page_create(parent);

    /* ── Header ── */
    settings_create_page_header(page, "Device Info", back_click_cb, ctx);

    /* ── Separator ── */
    settings_create_separator(page);

    /* ── Scrollable content area (same as Local page) ── */
    lv_obj_t *content = lv_obj_create(page);
    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_style_pad_left(content, 0, 0);
    lv_obj_set_style_pad_right(content, 0, 0);
    lv_obj_set_style_pad_top(content, SF_UI(8), 0);
    lv_obj_set_style_pad_bottom(content, SF_UI(16), 0);
    lv_obj_set_style_pad_row(content, SF_UI(16), 0);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* ── Category: Device ── */
    lv_obj_t *card = settings_create_category(content, "Device");

    char sn[32];
    get_mac_str(sn, sizeof(sn));
    create_info_row(card, LV_SYMBOL_LIST, "SN", sn, NULL);
    settings_create_item_sep(card);

    char uptime[32];
    format_uptime(uptime, sizeof(uptime));
    create_info_row(card, LV_SYMBOL_REFRESH, "Uptime", uptime, &priv->uptime_lbl);
    /* Scroll the Uptime label when it gets too wide (statically right-aligned when short) */
    lv_obj_set_width(priv->uptime_lbl, SF_UI(120));
    lv_label_set_long_mode(priv->uptime_lbl, LV_LABEL_LONG_MODE_SCROLL);
    settings_create_item_sep(card);

    const esp_app_desc_t *desc = esp_app_get_description();
    char fw[128];
    snprintf(fw, sizeof(fw), "v%s", desc->version);
    create_info_row(card, LV_SYMBOL_SETTINGS, "Version", fw, NULL);
    settings_create_item_sep(card);

    /* SPIFFS storage usage (used / total, in KB) */
    size_t spiffs_total = 0, spiffs_used = 0;
    char storage[32] = "N/A";
    if (esp_spiffs_info("spiffs", &spiffs_total, &spiffs_used) == ESP_OK) {
        snprintf(storage, sizeof(storage), "%u/%u KB",
                 (unsigned)(spiffs_used / 1024), (unsigned)(spiffs_total / 1024));
    }
    create_info_row(card, LV_SYMBOL_SAVE, "Storage", storage, NULL);
    settings_create_item_sep(card);

    /* Memory usage (used / total, in KB) */
    size_t mem_free  = esp_get_free_heap_size();
    size_t mem_total = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    char memory[32];
    snprintf(memory, sizeof(memory), "%u/%u KB",
             (unsigned)((mem_total - mem_free) / 1024), (unsigned)(mem_total / 1024));
    create_info_row(card, LV_SYMBOL_POWER, "Memory", memory, NULL);

    /* ── Category: System ── */
    card = settings_create_category(content, "System");

    /* OTA upgrade row (icon + label + ">" arrow on the right; tap to trigger upgrade) */
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(row, SF_UI(8), 0);
    lv_obj_set_style_pad_bottom(row, SF_UI(8), 0);
    lv_obj_set_style_pad_left(row, SF_UI(16), 0);
    lv_obj_set_style_pad_right(row, SF_UI(16), 0);
    lv_obj_set_style_pad_column(row, SF_UI(4), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_style(row, sf_theme_get_style(SF_STYLE_BG_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_10, LV_STATE_PRESSED);

    lv_obj_t *icon_lbl = lv_label_create(row);
    lv_label_set_text(icon_lbl, LV_SYMBOL_DOWNLOAD);
    lv_obj_add_style(icon_lbl, sf_theme_get_style(SF_STYLE_TXT_ACCENT), 0);
    lv_obj_set_style_text_font(icon_lbl, SF_FONT_SM, 0);
    lv_obj_set_width(icon_lbl, SF_UI(20));

    lv_obj_t *txt = lv_label_create(row);
    lv_label_set_text(txt, "Firmware Update");
    lv_obj_add_style(txt, sf_theme_get_style(SF_STYLE_TXT_SECONDARY), 0);
    lv_obj_set_style_text_font(txt, SF_FONT_XS, 0);
    lv_obj_set_flex_grow(txt, 1);

    lv_obj_t *arrow = lv_label_create(row);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_add_style(arrow, sf_theme_get_style(SF_STYLE_TXT_ARROW), 0);
    lv_obj_set_style_text_font(arrow, SF_FONT_SM, 0);

#if OTA_CONFIGURED
    lv_obj_add_event_cb(row, ota_click_cb, LV_EVENT_CLICKED, priv);
#else
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_style(txt, sf_theme_get_style(SF_STYLE_TXT_MUTED), 0);
    lv_obj_add_style(icon_lbl, sf_theme_get_style(SF_STYLE_TXT_MUTED), 0);
#endif

    /* Upgrade status text */
    priv->status_lbl = lv_label_create(card);
    lv_label_set_text(priv->status_lbl, "");
    lv_obj_add_style(priv->status_lbl, sf_theme_get_style(SF_STYLE_TXT_MUTED), 0);
    lv_obj_set_style_text_font(priv->status_lbl, SF_FONT_SM, 0);
    lv_obj_set_style_pad_left(priv->status_lbl, SF_UI(16), 0);
    lv_obj_set_style_pad_top(priv->status_lbl, SF_UI(8), 0);
    lv_obj_add_flag(priv->status_lbl, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_user_data(page, priv);

    /* Register the page delete callback (cleans up the timer) */
    lv_obj_add_event_cb(page, device_page_delete_cb, LV_EVENT_DELETE, NULL);

    /* Uptime live-refresh timer (1s) */
    priv->uptime_timer = lv_timer_create(uptime_timer_cb, 1000, priv);

    lvgl_port_unlock();

    return page;
}
