#include "sf_app_settings.h"
#include "sf_settings_pages.h"
#include "sf_theme.h"
#include "sf_sys.h"
#include "sf_gui.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include <string.h>

static const char *TAG = "sf_app_settings";

typedef struct {
    lv_obj_t *content;
    settings_ctx_t pg;
} settings_priv_t;

/* ---- shared UI helpers ---- */

lv_obj_t *settings_create_page_header(lv_obj_t *parent, const char *title,
                                       lv_event_cb_t back_cb, void *user_data)
{
    lv_obj_t *header = lv_obj_create(parent);
    lv_obj_remove_style_all(header);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(header, 8, 0);
    lv_obj_set_scrollbar_mode(header, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back_btn = lv_obj_create(header);
    lv_obj_remove_style_all(back_btn);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(back_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(back_btn, SF_UI(36), SF_UI(36));
    lv_obj_set_style_radius(back_btn, SF_UI(18), 0);
    lv_obj_add_style(back_btn, sf_theme_get_style(SF_STYLE_BG_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_10, LV_STATE_PRESSED);

    lv_obj_t *back_icon = lv_label_create(back_btn);
    lv_label_set_text(back_icon, LV_SYMBOL_LEFT);
    lv_obj_add_style(back_icon, sf_theme_get_style(SF_STYLE_TXT_PRIMARY), 0);
    lv_obj_set_style_text_font(back_icon, SF_FONT_SM, 0);
    lv_obj_center(back_icon);

    lv_obj_add_event_cb(back_btn, back_cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *title_lbl = lv_label_create(header);
    lv_label_set_text(title_lbl, title);
    lv_obj_add_style(title_lbl, sf_theme_get_style(SF_STYLE_TXT_PRIMARY), 0);
    lv_obj_set_style_text_font(title_lbl, SF_FONT_LG, 0);

    return header;
}

lv_obj_t *settings_create_separator(lv_obj_t *parent)
{
    lv_obj_t *sep = lv_obj_create(parent);
    lv_obj_remove_style_all(sep);
    lv_obj_set_width(sep, LV_PCT(100));
    lv_obj_set_height(sep, 1);
    lv_obj_add_style(sep, sf_theme_get_style(SF_STYLE_BG_SEP_PRIMARY), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_clear_flag(sep, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(sep, LV_SCROLLBAR_MODE_OFF);
    return sep;
}

static void on_wifi_click(lv_event_t *e)
{
    settings_ctx_t *ctx = lv_event_get_user_data(e);
    ctx->cur_page = sf_settings_wifi_create(ctx->window, ctx);
    lv_obj_add_flag(ctx->main_page, LV_OBJ_FLAG_HIDDEN);
}

static void on_bt_click(lv_event_t *e)
{
    settings_ctx_t *ctx = lv_event_get_user_data(e);
    ctx->cur_page = sf_settings_bt_create(ctx->window, ctx);
    lv_obj_add_flag(ctx->main_page, LV_OBJ_FLAG_HIDDEN);
}

static void on_local_click(lv_event_t *e)
{
    settings_ctx_t *ctx = lv_event_get_user_data(e);
    ctx->cur_page = sf_settings_local_create(ctx->window, ctx);
    lv_obj_add_flag(ctx->main_page, LV_OBJ_FLAG_HIDDEN);
}

static void on_device_click(lv_event_t *e)
{
    settings_ctx_t *ctx = lv_event_get_user_data(e);
    ctx->cur_page = sf_settings_device_create(ctx->window, ctx);
    lv_obj_add_flag(ctx->main_page, LV_OBJ_FLAG_HIDDEN);
}

void settings_show_main(settings_ctx_t *ctx)
{
    if (ctx->cur_page) {
        lv_obj_del(ctx->cur_page);
        ctx->cur_page = NULL;
    }
    lv_obj_clear_flag(ctx->main_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ctx->main_page);
}

lv_obj_t *settings_page_create(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);                 /* strip theme "card" style (pad_row=10, border, etc.) */
    lv_obj_set_width(page, LV_PCT(100));
    lv_obj_set_height(page, LV_PCT(100));
    lv_obj_add_style(page, sf_theme_get_style(SF_STYLE_BG_PAGE), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_style_pad_row(page, 0, 0);          /* no gap between flex children — separators sit flush */
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    return page;
}

/* ---- helper: thin separator inside a card (indented after icon) ---- */
lv_obj_t *settings_create_item_sep(lv_obj_t *parent)
{
    lv_obj_t *sep = lv_obj_create(parent);
    lv_obj_remove_style_all(sep);
    lv_obj_set_width(sep, LV_PCT(100));
    lv_obj_set_height(sep, 1);
    lv_obj_add_style(sep, sf_theme_get_style(SF_STYLE_BG_SEPARATOR), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(sep, SF_UI(52), 0);   /* indent to align after icon + gap */
    lv_obj_clear_flag(sep, LV_OBJ_FLAG_SCROLLABLE);
    return sep;
}

/* ---- helper: create a category section (header + card) ---- */
lv_obj_t *settings_create_category(lv_obj_t *parent, const char *title)
{
    lv_obj_t *section = lv_obj_create(parent);
    lv_obj_remove_style_all(section);
    lv_obj_set_width(section, LV_PCT(100));
    lv_obj_set_height(section, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(section, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(section, 4, 0);  /* small gap between header and card */
    lv_obj_clear_flag(section, LV_OBJ_FLAG_SCROLLABLE);

    if (title) {
        lv_obj_t *hdr = lv_label_create(section);
        lv_label_set_text(hdr, title);
        lv_obj_add_style(hdr, sf_theme_get_style(SF_STYLE_TXT_MUTED), 0);
        lv_obj_set_style_text_font(hdr, SF_FONT_SM, 0);
        lv_obj_set_width(hdr, LV_PCT(100));
        lv_obj_set_style_pad_left(hdr, SF_UI(16), 0);
        lv_obj_set_style_pad_top(hdr, 0, 0);
        lv_obj_set_style_pad_bottom(hdr, 0, 0);
    }

    lv_obj_t *card = lv_obj_create(section);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_add_style(card, sf_theme_get_style(SF_STYLE_BG_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 12, 0);
    /* Note: do not set clip_corner on the category cards. In LVGL 9, when clip_corner && radius!=0
       is set, the object allocates two ARGB8888 off-screen layers (one radius-height band at the top
       and one at the bottom). A full-width card can trigger an off-screen allocation failure and cause
       an infinite refresh loop that triggers the watchdog. The inner list items have 16px side padding,
       so dropping clipping causes no visual overflow. */
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_pad_row(card, 0, 0);     /* items separated by create_item_sep, not pad_row */
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    return card;
}

static lv_obj_t *create_list_item(lv_obj_t *parent, const char *icon, const char *text,
                                   lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *item = lv_obj_create(parent);
    lv_obj_remove_style_all(item);                 /* strip theme "card" style (pad_all=16, border, etc.) */
    lv_obj_set_width(item, LV_PCT(100));
    lv_obj_set_height(item, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(item, SF_UI(8), 0);          /* 8px above text */
    lv_obj_set_style_pad_bottom(item, SF_UI(8), 0);       /* 8px below text */
    lv_obj_set_style_pad_left(item, SF_UI(16), 0);
    lv_obj_set_style_pad_right(item, SF_UI(16), 0);
    lv_obj_set_style_pad_column(item, SF_UI(4), 0);       /* gap between icon and text */
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);

    /* pressed feedback */
    lv_obj_add_style(item, sf_theme_get_style(SF_STYLE_BG_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(item, LV_OPA_10, LV_STATE_PRESSED);

    /* left icon */
    lv_obj_t *icon_lbl = lv_label_create(item);
    lv_label_set_text(icon_lbl, icon);
    lv_obj_add_style(icon_lbl, sf_theme_get_style(SF_STYLE_TXT_ACCENT), 0);
    lv_obj_set_style_text_font(icon_lbl, SF_FONT_SM, 0);
    lv_obj_set_width(icon_lbl, SF_UI(20));

    /* text — flex_grow fills remaining space, pushing arrow to the right */
    lv_obj_t *txt = lv_label_create(item);
    lv_label_set_text(txt, text);
    lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_add_style(txt, sf_theme_get_style(SF_STYLE_TXT_SECONDARY), 0);
    lv_obj_set_style_text_font(txt, SF_FONT_XS, 0);
    lv_obj_set_flex_grow(txt, 1);

    /* right arrow — pinned to far right */
    lv_obj_t *arrow = lv_label_create(item);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_add_style(arrow, sf_theme_get_style(SF_STYLE_TXT_ARROW), 0);
    lv_obj_set_style_text_font(arrow, SF_FONT_SM, 0);

    lv_obj_add_event_cb(item, cb, LV_EVENT_CLICKED, user_data);
    return item;
}

static esp_err_t on_create(sf_app_ctx_t *ctx)
{
    settings_priv_t *priv = calloc(1, sizeof(settings_priv_t));
    if (!priv) return ESP_ERR_NO_MEM;
    ctx->user_data = priv;

    lvgl_port_lock(0);

    lv_obj_t *root = sf_gui_app_get_root(ctx);
    priv->pg.window = root;

    /* ---- main scrollable content (vertical scroll only) ---- */
    lv_obj_t *content = lv_obj_create(root);
    priv->content = content;
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_height(content, LV_PCT(100));
    lv_obj_add_style(content, sf_theme_get_style(SF_STYLE_BG_PAGE), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_style_pad_bottom(content, SF_UI(16), 0);
    lv_obj_set_style_min_height(content, 0, 0);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content, SF_UI(16), 0);

    /* ---- title bar ---- */
    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, "Settings");
    lv_obj_add_style(title, sf_theme_get_style(SF_STYLE_TXT_PRIMARY), 0);
    lv_obj_set_style_text_font(title, SF_FONT_LG, 0);
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(title, SF_UI(16), 0);
    lv_obj_set_style_pad_bottom(title, SF_UI(4), 0);

    priv->pg.main_page = content;

    /* ---- category: Network ---- */
    lv_obj_t *card = settings_create_category(content, "Network");
    create_list_item(card, LV_SYMBOL_WIFI, "Wi-Fi", on_wifi_click, &priv->pg);
    settings_create_item_sep(card);
    create_list_item(card, LV_SYMBOL_BLUETOOTH, "Bluetooth", on_bt_click, &priv->pg);

    /* ---- category: Preferences ---- */
    card = settings_create_category(content, "Preferences");
    create_list_item(card, LV_SYMBOL_SETTINGS, "Local", on_local_click, &priv->pg);

    /* ---- category: About ---- */
    card = settings_create_category(content, "About");
    create_list_item(card, LV_SYMBOL_LIST, "Device Info", on_device_click, &priv->pg);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "on_create");
    return ESP_OK;
}

static void on_start(sf_app_ctx_t *ctx)    { ESP_LOGI(TAG, "on_start"); }
static void on_stop(sf_app_ctx_t *ctx)     { ESP_LOGI(TAG, "on_stop"); }

static void on_resume(sf_app_ctx_t *ctx)
{
    ESP_LOGI(TAG, "on_resume");
}

static void on_pause(sf_app_ctx_t *ctx)
{
    ESP_LOGI(TAG, "on_pause");
}

static void on_destroy(sf_app_ctx_t *ctx)
{
    settings_priv_t *priv = ctx->user_data;
    if (priv) {
        if (priv->content) {
            lvgl_port_lock(0);
            lv_obj_del(priv->content);
            lvgl_port_unlock();
        }
        free(priv);
        ctx->user_data = NULL;
    }
    ESP_LOGI(TAG, "on_destroy");
}

static bool on_back(sf_app_ctx_t *ctx)
{
    settings_priv_t *priv = ctx->user_data;
    if (priv && priv->pg.cur_page) {
        lvgl_port_lock(0);
        settings_show_main(&priv->pg);
        lvgl_port_unlock();
        return true;
    }
    return false;
}

static void on_event(sf_app_ctx_t *ctx, esp_event_base_t base, int32_t id, void *event_data)
{
    ESP_LOGI(TAG, "on_event base=%s id=%d", base, (int)id);

    if (strcmp(base, SF_EVENT_BASE) == 0 && id == SF_EVENT_APP_INTENT) {
        const sf_intent_t *intent = event_data;
        if (!intent) return;

        const char *page = sf_intent_extra_string(intent, "page", NULL);
        if (!page) return;

        ESP_LOGI(TAG, "navigate to page=%s", page);
        if (strcmp(page, "wifi") == 0) {
            settings_priv_t *priv = ctx->user_data;
            if (!priv) return;
            lvgl_port_lock(0);
            if (priv->pg.cur_page) {
                lv_obj_del(priv->pg.cur_page);
                priv->pg.cur_page = NULL;
            }
            lv_obj_add_flag(priv->pg.main_page, LV_OBJ_FLAG_HIDDEN);
            priv->pg.cur_page = sf_settings_wifi_create(priv->pg.window, &priv->pg);
            lvgl_port_unlock();
        } else {
            ESP_LOGW(TAG, "unknown page: %s", page);
        }
    }
}

const sf_app_ops_t g_settings_app_ops = {
    .on_create = on_create,
    .on_start = on_start,
    .on_resume = on_resume,
    .on_pause = on_pause,
    .on_stop = on_stop,
    .on_destroy = on_destroy,
    .on_back = on_back,
    .on_event = on_event,
};

static const sf_app_intent_filter_t s_intent_filters[] = {
    SF_APP_INTENT_FILTER(SF_INTENT_ACTION_SETTINGS, SF_INTENT_CATEGORY_DEFAULT),
};

const sf_app_manifest_t g_settings_app_manifest = {
    .id = "settings",
    .name = "Settings",
    .icon = LV_SYMBOL_SETTINGS,
    .version = "1.0.0",
    .flags = SF_APP_FLAG_SHOW_IN_LAUNCHER,
    .ops = &g_settings_app_ops,
    .intent_filters = s_intent_filters,
    .intent_filters_count = 1,
};
