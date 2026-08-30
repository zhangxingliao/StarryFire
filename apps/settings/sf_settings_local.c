/**
 * sf_settings_local.c — Local settings page
 *
 * Provides basic configuration: brightness, timezone, screen timeout, etc.
 * Settings are persisted to SPIFFS JSON through the sf_config module.
 */
#include "sf_settings_pages.h"
#include "sf_config.h"
#include "sf_ntp.h"
#include "sf_hal.h"
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_system.h"
#include "lvgl.h"
#include "sf_theme.h"

static const char *TAG = "sf_settings_local";

/* ── Timezone options (UTC-12 ~ UTC+12, full coverage) ───────────── */

typedef struct {
    const char *label;   /* Display name */
    const char *tz;      /* POSIX TZ value */
} tz_option_t;

#define TZ_OFFSET_MIN  (-12)
#define TZ_OFFSET_MAX  (12)
#define TZ_COUNT       (TZ_OFFSET_MAX - TZ_OFFSET_MIN + 1)   /* 25 */

/* Static buffers to avoid dynamic allocation */
static char s_tz_labels[TZ_COUNT][16];
static char s_tz_vals[TZ_COUNT][16];
static tz_option_t tz_options[TZ_COUNT];
static bool s_tz_inited = false;

/* Generate all hour-offset timezones from UTC-12 to UTC+12 */
static void init_tz_options(void)
{
    if (s_tz_inited) return;
    for (int i = 0; i < (int)TZ_COUNT; i++) {
        int hour = TZ_OFFSET_MIN + i;
        snprintf(s_tz_labels[i], sizeof(s_tz_labels[i]), "UTC%+d:00", hour);
        /* POSIX TZ offset = -hour (east zones get a positive offset, hence the minus sign) */
        snprintf(s_tz_vals[i], sizeof(s_tz_vals[i]), "UTC%d", -hour);
        tz_options[i].label = s_tz_labels[i];
        tz_options[i].tz    = s_tz_vals[i];
    }
    s_tz_inited = true;
}

/* ── Screen timeout options ─────────────────────────────────── */

typedef struct {
    const char *label;
    int seconds;
} timeout_option_t;

static const timeout_option_t timeout_options[] = {
    {"10s",   10},
    {"30s",   30},
    {"1min",  60},
    {"5min",  300},
    {"Never", 0},
};

#define TIMEOUT_COUNT (sizeof(timeout_options) / sizeof(timeout_options[0]))

/* ── Brightness levels ─────────────────────────────────────── */

#define BRIGHTNESS_STEP  10
#define BRIGHTNESS_COUNT 10

/* ── Page private data ─────────────────────────────────── */

typedef struct {
    settings_ctx_t *ctx;
    lv_obj_t *page;
    lv_obj_t *bright_dd;
    lv_obj_t *tz_dd;
    lv_obj_t *timeout_dd;
    lv_obj_t *theme_dd;
} local_priv_t;

/* ── Back callback ─────────────────────────────────────── */

static void local_back_cb(lv_event_t *e)
{
    settings_ctx_t *ctx = lv_event_get_user_data(e);
    settings_show_main(ctx);
}

/* ── Dropdown callbacks ────────────────────────────────── */

static void brightness_cb(lv_event_t *e)
{
    local_priv_t *priv = lv_event_get_user_data(e);
    uint16_t sel = lv_dropdown_get_selected(priv->bright_dd);
    int val = (sel + 1) * BRIGHTNESS_STEP;
    sf_hal_display_set_brightness((uint8_t)val);
    sf_config_set_brightness(val);
    sf_config_save();
}

static void timezone_cb(lv_event_t *e)
{
    local_priv_t *priv = lv_event_get_user_data(e);
    uint16_t sel = lv_dropdown_get_selected(priv->tz_dd);
    if (sel < TZ_COUNT) {
        sf_config_set_timezone(tz_options[sel].tz);
        sf_config_save();
        setenv("TZ", tz_options[sel].tz, 1);
        tzset();
        sf_ntp_set_tz(tz_options[sel].tz);   /* Notify the NTP module too; affects the status bar display */
        ESP_LOGI(TAG, "timezone set to %s", tz_options[sel].label);
    }
}

static void timeout_cb(lv_event_t *e)
{
    local_priv_t *priv = lv_event_get_user_data(e);
    uint16_t sel = lv_dropdown_get_selected(priv->timeout_dd);
    if (sel < TIMEOUT_COUNT) {
        sf_config_set_screen_timeout(timeout_options[sel].seconds);
        sf_config_save();
        ESP_LOGI(TAG, "screen timeout set to %ds", timeout_options[sel].seconds);
    }
}

/* ── Theme switch callback ─────────────────────────────────── */

static void theme_cb(lv_event_t *e)
{
    local_priv_t *priv = lv_event_get_user_data(e);
    uint16_t sel = lv_dropdown_get_selected(priv->theme_dd);
    if (sel < SF_THEME_COUNT) {
        sf_config_set_theme((int)sel);
        sf_config_save();
        ESP_LOGI(TAG, "theme set to %s, restarting...", sf_theme_name((sf_theme_id_t)sel));
        esp_restart();
    }
}

/* ── Create a config row with icon (no arrow; right side holds a custom widget) ── */

static lv_obj_t *create_config_row(lv_obj_t *parent, const char *icon, const char *text)
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
    lv_label_set_text(txt, text);
    lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_add_style(txt, sf_theme_get_style(SF_STYLE_TXT_SECONDARY), 0);
    lv_obj_set_style_text_font(txt, SF_FONT_XS, 0);
    lv_obj_set_flex_grow(txt, 1);

    return item;
}

/* ── Dropdown state callback: shows > when collapsed, ↓ when expanded ────── */

static void dropdown_state_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    bool opened = lv_obj_has_state(dd, LV_STATE_CHECKED);
    lv_dropdown_set_symbol(dd, opened ? LV_SYMBOL_DOWN : LV_SYMBOL_RIGHT);
}

/* ── Create a uniformly styled Dropdown ──────────────────────── */

static lv_obj_t *create_dropdown(lv_obj_t *parent)
{
    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_obj_add_style(dd, sf_theme_get_style(SF_STYLE_TXT_SECONDARY), 0);
    lv_obj_set_style_text_font(dd, SF_FONT_SM, 0);
    lv_obj_add_style(dd, sf_theme_get_style(SF_STYLE_BG_PAGE), 0);
    lv_obj_set_style_bg_opa(dd, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dd, 1, 0);
    lv_obj_add_style(dd, sf_theme_get_style(SF_STYLE_BORDER_SEP_PRIMARY), 0);
    lv_obj_set_style_pad_left(dd, SF_UI(8), 0);
    lv_obj_set_style_pad_right(dd, SF_UI(8), 0);
    lv_obj_set_style_pad_top(dd, SF_UI(4), 0);
    lv_obj_set_style_pad_bottom(dd, SF_UI(4), 0);
    lv_dropdown_set_symbol(dd, LV_SYMBOL_RIGHT);   /* Default (collapsed): right arrow */

    /* Dropdown list styling */
    lv_obj_t *list = lv_dropdown_get_list(dd);
    lv_obj_add_style(list, sf_theme_get_style(SF_STYLE_BG_PAGE), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_add_style(list, sf_theme_get_style(SF_STYLE_TXT_SECONDARY), 0);
    lv_obj_set_style_text_font(list, SF_FONT_SM, 0);
    lv_obj_set_style_border_width(list, 1, 0);
    lv_obj_add_style(list, sf_theme_get_style(SF_STYLE_BORDER_SEP_PRIMARY), 0);
    lv_obj_set_style_max_height(list, 150, 0);

    /* Swap the right-side symbol when expanding/collapsing */
    lv_obj_add_event_cb(dd, dropdown_state_cb, LV_EVENT_STATE_CHANGED, NULL);

    return dd;
}

/* ── Page delete callback (frees priv) ─────────────────────── */

static void local_page_delete_cb(lv_event_t *e)
{
    lv_obj_t *page = lv_event_get_target(e);
    local_priv_t *priv = lv_obj_get_user_data(page);
    free(priv);
    lv_obj_set_user_data(page, NULL);
}

/* ── Page creation ─────────────────────────────────────── */

lv_obj_t *sf_settings_local_create(lv_obj_t *parent, settings_ctx_t *ctx)
{
    local_priv_t *priv = calloc(1, sizeof(local_priv_t));
    if (!priv) return NULL;
    priv->ctx = ctx;

    init_tz_options();

    lvgl_port_lock(0);

    lv_obj_t *page = settings_page_create(parent);
    priv->page = page;

    /* Header */
    settings_create_page_header(page, "Local", local_back_cb, ctx);

    /* Separator */
    settings_create_separator(page);

    /* Scrollable content area */
    lv_obj_t *content = lv_obj_create(page);
    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_style_pad_left(content, 0, 0);
    lv_obj_set_style_pad_right(content, 0, 0);
    lv_obj_set_style_pad_top(content, SF_UI(8), 0);
    lv_obj_set_style_pad_bottom(content, SF_UI(16), 0);
    lv_obj_set_style_pad_row(content, 16, 0);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* ── Category: Display ── */
    lv_obj_t *card = settings_create_category(content, "Display");

    /* Brightness row */
    lv_obj_t *row = create_config_row(card, LV_SYMBOL_IMAGE, "Brightness");
    priv->bright_dd = create_dropdown(row);
    lv_obj_set_width(priv->bright_dd, 100);

    /* Build brightness options: 10% ~ 100% */
    char bri_str[64] = {0};
    int bri_sel = 0;
    int cur_bri = sf_config_get_brightness();
    for (int i = 0; i < BRIGHTNESS_COUNT; i++) {
        if (i > 0) strcat(bri_str, "\n");
        char tmp[8];
        snprintf(tmp, sizeof(tmp), "%d%%", (i + 1) * BRIGHTNESS_STEP);
        strcat(bri_str, tmp);
        if (cur_bri >= (i + 1) * BRIGHTNESS_STEP) bri_sel = i;
    }
    lv_dropdown_set_options(priv->bright_dd, bri_str);
    lv_dropdown_set_selected(priv->bright_dd, bri_sel);
    lv_obj_add_event_cb(priv->bright_dd, brightness_cb, LV_EVENT_VALUE_CHANGED, priv);

    /* Separator */
    settings_create_item_sep(card);

    /* Screen timeout row */
    row = create_config_row(card, LV_SYMBOL_POWER, "Screen Timeout");
    priv->timeout_dd = create_dropdown(row);
    lv_obj_set_width(priv->timeout_dd, 100);

    /* Build the timeout option string */
    char timeout_str[64] = {0};
    int timeout_sel = 0;
    int cur_timeout = sf_config_get_screen_timeout();
    for (int i = 0; i < (int)TIMEOUT_COUNT; i++) {
        if (i > 0) strcat(timeout_str, "\n");
        strcat(timeout_str, timeout_options[i].label);
        if (cur_timeout == timeout_options[i].seconds) timeout_sel = i;
    }
    lv_dropdown_set_options(priv->timeout_dd, timeout_str);
    lv_dropdown_set_selected(priv->timeout_dd, timeout_sel);
    lv_obj_add_event_cb(priv->timeout_dd, timeout_cb, LV_EVENT_VALUE_CHANGED, priv);

    /* ── Category: System ── */
    card = settings_create_category(content, "System");

    /* Timezone row */
    row = create_config_row(card, LV_SYMBOL_REFRESH, "Timezone");
    priv->tz_dd = create_dropdown(row);
    lv_obj_set_width(priv->tz_dd, 100);

    /* Build the timezone option string */
    char tz_str[320] = {0};
    int tz_sel = (8 - TZ_OFFSET_MIN);   /* Default UTC+8 (fallback when old config doesn't match) */
    const char *cur_tz = sf_config_get_timezone();
    for (int i = 0; i < (int)TZ_COUNT; i++) {
        if (i > 0) strcat(tz_str, "\n");
        strcat(tz_str, tz_options[i].label);
        if (strcmp(cur_tz, tz_options[i].tz) == 0) tz_sel = i;
    }
    lv_dropdown_set_options(priv->tz_dd, tz_str);
    lv_dropdown_set_selected(priv->tz_dd, tz_sel);
    lv_obj_add_event_cb(priv->tz_dd, timezone_cb, LV_EVENT_VALUE_CHANGED, priv);

    /* ── Category: Appearance ── */
    card = settings_create_category(content, "Appearance");

    /* Theme row */
    row = create_config_row(card, LV_SYMBOL_TINT, "Theme");
    priv->theme_dd = create_dropdown(row);
    lv_obj_set_width(priv->theme_dd, 120);

    /* Build the theme options */
    char theme_str[64] = {0};
    int theme_sel = 0;
    int cur_theme = sf_config_get_theme();
    for (int i = 0; i < (int)SF_THEME_COUNT; i++) {
        if (i > 0) strcat(theme_str, "\n");
        strcat(theme_str, sf_theme_name((sf_theme_id_t)i));
        if (cur_theme == i) theme_sel = i;
    }
    lv_dropdown_set_options(priv->theme_dd, theme_str);
    lv_dropdown_set_selected(priv->theme_dd, theme_sel);

    /* Theme switch event */
    lv_obj_add_event_cb(priv->theme_dd, theme_cb, LV_EVENT_VALUE_CHANGED, priv);

    /* Apply the initial timezone */
    setenv("TZ", sf_config_get_timezone(), 1);
    tzset();

    lv_obj_set_user_data(page, priv);

    /* Register the page delete callback (frees priv) */
    lv_obj_add_event_cb(page, local_page_delete_cb, LV_EVENT_DELETE, NULL);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "Local settings page created");
    return page;
}
