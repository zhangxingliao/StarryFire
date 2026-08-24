#include "sf_gui_launcher.h"
#include "sf_gui.h"
#include "sf_theme.h"
#include "sf_sys.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "sf_gui_launcher";

typedef struct {
    lv_obj_t *btn;
    const sf_app_manifest_t *manifest;
} app_item_t;

static void app_click_cb(lv_event_t *e)
{
    app_item_t *item = lv_event_get_user_data(e);
    if (item && item->manifest) {
        sf_app_start(item->manifest->id);
    }
}

static void app_item_delete_cb(lv_event_t *e)
{
    app_item_t *item = lv_event_get_user_data(e);
    free(item);
}

static bool collect_apps_cb(sf_app_ctx_t *ctx, void *arg)
{
    lv_obj_t *grid = arg;
    const sf_app_manifest_t *m = ctx->manifest;

    if (!(m->flags & SF_APP_FLAG_SHOW_IN_LAUNCHER))
        return true;

    app_item_t *item = calloc(1, sizeof(app_item_t));
    if (!item) return true;
    item->manifest = m;

    lv_obj_t *cont = lv_obj_create(grid);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_width(cont, SF_UI(65));
    lv_obj_set_height(cont, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    item->btn = lv_obj_create(cont);
    lv_obj_add_flag(item->btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(item->btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(item->btn, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(item->btn, SF_UI(56), SF_UI(56));
    lv_obj_add_style(item->btn, sf_theme_get_style(SF_STYLE_BTN), 0);
    lv_obj_add_style(item->btn, sf_theme_get_style(SF_STYLE_BTN_PRESSED), LV_STATE_PRESSED);

    const char *sym = m->icon ? m->icon : LV_SYMBOL_SETTINGS;
    lv_obj_t *ic = lv_label_create(item->btn);
    lv_label_set_text(ic, sym);
    lv_obj_add_style(ic, sf_theme_get_style(SF_STYLE_ICON), 0);
    lv_obj_center(ic);

    lv_obj_add_event_cb(item->btn, app_click_cb, LV_EVENT_CLICKED, item);
    lv_obj_add_event_cb(cont, app_item_delete_cb, LV_EVENT_DELETE, item);

    lv_obj_t *lbl = lv_label_create(cont);
    lv_label_set_text(lbl, m->name);
    lv_obj_set_width(lbl, SF_UI(60));
    lv_obj_add_style(lbl, sf_theme_get_style(SF_STYLE_LABEL), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);

    return true;
}

void sf_gui_launcher_refresh(lv_obj_t *grid)
{
    sf_app_foreach(collect_apps_cb, grid);
}

lv_obj_t *sf_gui_launcher_create(lv_obj_t *parent)
{
    lv_obj_t *content = lv_obj_create(parent);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_height(content, LV_PCT(100));
    lv_obj_add_style(content, sf_theme_get_style(SF_STYLE_BG_LAUNCHER), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_80, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *grid = lv_obj_create(content);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_height(grid, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, SF_UI(12), 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    sf_gui_launcher_refresh(grid);

    ESP_LOGI(TAG, "desktop created");
    return content;
}
