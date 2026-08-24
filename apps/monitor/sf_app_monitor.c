/**
 * sf_app_monitor.c — System Monitor app (integration shell)
 *
 * This file only handles "integration": the tabview container, the custom-drawn header title, the
 * page indicator dots, and the App lifecycle. It also holds the background collection task inside
 * the shell — periodically calling each data module (tasks/mem/disk) through
 * sf_monitor_do_collect_xxx to fill their snapshots; UI refresh is routed by the active page to the
 * matching *_ui.c refresh().
 *
 * The actual rendering logic lives in:
 *   - sf_monitor_tasks_ui.c / sf_monitor_tasks_data.c
 *   - sf_monitor_mem_ui.c   / sf_monitor_mem_data.c
 *   - sf_monitor_disk_ui.c  / sf_monitor_disk_data.c
 * Data acquisition and UI rendering are decoupled; each feature is a self-contained "data + ui" pair.
 *
 * lv_tabview hosts the 3 pages (Tasks / Regions / Storage) with the built-in tab bar hidden and
 * replaced by a custom single large centered title: the header shows only the current page name, and
 * swiping the content left/right switches pages while keeping the title in sync.
 */
#include "sf_app_monitor.h"
#include "sf_monitor_ui.h"
#include "sf_monitor_core.h"
#include "sf_gui.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "sf_theme.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "sf_app_monitor";

/* Tab definitions (held by the shell; UI files reference name/placeholder) */
static const tab_def_t s_tabs[] = {
    { "Tasks",   "Tasks (todo)" },
    { "Regions", "Regions (todo)" },
    { "Storage", "Storage (todo)" },
};

/* ── Header title & page dots track the active page ─────────────────── */
static void update_header(monitor_priv_t *priv)
{
    uint32_t idx = lv_tabview_get_tab_active(priv->tv);
    if ((int)idx >= TAB_COUNT) idx = 0;

    if (priv->title) lv_label_set_text(priv->title, s_tabs[idx].name);

    for (int i = 0; i < TAB_COUNT; i++) {
        if (!priv->dots[i]) continue;
        bool on = (i == (int)idx);
        lv_obj_set_style_bg_color(priv->dots[i],
            (on ? C_ACCENT : C_SEP), 0);
        lv_obj_set_style_bg_opa(priv->dots[i], LV_OPA_COVER, 0);
    }
}

/* Fired when the tabview content is swiped to switch pages */
static void tab_changed_cb(lv_event_t *e)
{
    monitor_priv_t *priv = lv_event_get_user_data(e);
    if (!priv) return;
    priv->cur_tab = (int)lv_tabview_get_tab_active(priv->tv);
    update_header(priv);
    if (priv->cur_tab == TAB_REGIONS) {
        sf_monitor_mem_reset_chart(priv);   /* Switch to Regions: clear line-chart history, realtime only */
    }
}

/* ── Background collection task (held in the shell; periodically drives each data module) ── */
#define COLLECT_INTERVAL_MS  1000
/* Collection task stack: do_collect_tasks calls uxTaskGetSystemState() (suspends the scheduler and
 * walks all tasks) before logging; in practice 2048 overflows the stack (high-water mark dropped to
 * a mere 16B before crashing), while 3072 leaves enough headroom.
 * NOTE: this stack belongs to sf_monitor's own background task and is unrelated to the LVGL
 * task_stack in sf_hal_core.c; adjusting it does not touch any shared/HAL configuration. */
#define COLLECT_TASK_STACK   3072
#define COLLECT_TASK_PRIO    2      /* Above IDLE(0), below critical tasks */

static TaskHandle_t s_task_handle = NULL;
static volatile bool s_stop_req = false;

static void collect_task_entry(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "collect task started");
    sf_monitor_tasks_reset_baseline();   /* Reset once per collection-thread start so cpu=0 on the first frame */
    while (!s_stop_req) {
        sf_monitor_do_collect_tasks();
        sf_monitor_do_collect_regions();
        sf_monitor_do_collect_disk();

        sf_monitor_log_tasks();      /* Guarded internally by SF_MONITOR_LOG_TASKS */
        sf_monitor_log_regions();    /* Guarded internally by SF_MONITOR_LOG_REGIONS */
        sf_monitor_log_disk();       /* Currently an empty implementation */

        /* Wait one period, or wake up on stop (to exit immediately) */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(COLLECT_INTERVAL_MS));
    }
    ESP_LOGI(TAG, "collect task stopped");
    s_task_handle = NULL;
    vTaskDelete(NULL);
}

/* Start the background collection task (idempotent) */
static esp_err_t monitor_collect_start(void)
{
    if (s_task_handle != NULL) return ESP_OK;   /* Already running */
    s_stop_req = false;
    BaseType_t r = xTaskCreate(collect_task_entry, "sf_mon",
                               COLLECT_TASK_STACK, NULL,
                               COLLECT_TASK_PRIO, &s_task_handle);
    if (r != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* Stop the background collection task (idempotent) */
static esp_err_t monitor_collect_stop(void)
{
    if (s_task_handle == NULL) return ESP_OK;
    s_stop_req = true;
    xTaskNotify(s_task_handle, 0, eNoAction);   /* Wake it so it can exit promptly */

    /* Wait for the background task to exit on its own (typically a few ms; bounded by the guard at worst) */
    int guard = 0;
    while (s_task_handle != NULL && guard++ < 100) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    s_stop_req = false;
    return ESP_OK;
}

/* ── UI refresh: only the active page refreshes (other pages / exiting app do not) ── */
static void refresh_cb(lv_timer_t *t)
{
    monitor_priv_t *priv = lv_timer_get_user_data(t);
    if (!priv) return;

    switch (priv->cur_tab) {
        case TAB_TASKS:   sf_monitor_tasks_refresh(priv); break;
        case TAB_REGIONS: sf_monitor_mem_refresh(priv);   break;
        case TAB_STORAGE: sf_monitor_disk_refresh(priv);  break;
        default: break;
    }
}

/* ── App lifecycle ──────────────────────────────────── */

static esp_err_t on_create(sf_app_ctx_t *ctx)
{
    monitor_priv_t *priv = calloc(1, sizeof(monitor_priv_t));
    if (!priv) return ESP_ERR_NO_MEM;
    ctx->user_data = priv;

    lvgl_port_lock(0);

    lv_obj_t *root = sf_gui_app_get_root(ctx);

    /* Root container: vertical flex (header + tabview + page dots) */
    lv_obj_t *content = lv_obj_create(root);
    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_height(content, LV_PCT(100));
    lv_obj_add_style(content, sf_theme_get_style(SF_STYLE_BG_PAGE), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_style_pad_row(content, 0, 0);

    /* ── Header: shows only the current page title (large, centered) ── */
    lv_obj_t *header = lv_obj_create(content);
    lv_obj_remove_style_all(header);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, HEADER_H);
    lv_obj_add_style(header, sf_theme_get_style(SF_STYLE_BG_PAGE), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, s_tabs[0].name);
    lv_obj_add_style(title, sf_theme_get_style(SF_STYLE_TXT_PRIMARY), 0);
    lv_obj_set_style_text_font(title, SF_FONT_LG, 0);
    lv_obj_center(title);   /* Horizontally + vertically centered within the header */
    priv->title = title;

    /* Separator line below the header */
    lv_obj_t *hsep = lv_obj_create(content);
    lv_obj_remove_style_all(hsep);
    lv_obj_set_width(hsep, LV_PCT(100));
    lv_obj_set_height(hsep, 1);
    lv_obj_add_style(hsep, sf_theme_get_style(SF_STYLE_BG_SEPARATOR), 0);
    lv_obj_set_style_bg_opa(hsep, LV_OPA_COVER, 0);
    lv_obj_clear_flag(hsep, LV_OBJ_FLAG_SCROLLABLE);

    /* ── tabview: holds the page content, with the built-in tab bar hidden ── */
    lv_obj_t *tv = lv_tabview_create(content);
    lv_obj_set_width(tv, LV_PCT(100));
    lv_obj_set_flex_grow(tv, 1);
    lv_obj_add_style(tv, sf_theme_get_style(SF_STYLE_BG_PAGE), 0);
    lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tv, 0, 0);
    lv_obj_set_style_radius(tv, 0, 0);
    priv->tv = tv;

    /* Hide the built-in tab bar (no longer show all tab headers at once) */
    lv_tabview_set_tab_bar_size(tv, 0);
    lv_obj_t *bar = lv_tabview_get_tab_bar(tv);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);

    /* Each page: content container + card wrapper, building delegated to the matching UI module */
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_t *page = lv_tabview_add_tab(tv, s_tabs[i].name);
        lv_obj_remove_style_all(page);
        lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
        lv_obj_add_style(page, sf_theme_get_style(SF_STYLE_BG_PAGE), 0);
        lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(page, 0, 0);   /* Stretch fully (cards flush to the edges, matching the app style) */
        lv_obj_set_style_pad_ver(page, 0, 0);   /* Table hugs the screen; no top/bottom margins */
        lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);

        /* Card container: follows the app card style (0x252540 / radius 0), filling the page */
        lv_obj_t *card = lv_obj_create(page);
        lv_obj_remove_style_all(card);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_flex_grow(card, 1);
        lv_obj_add_style(card, sf_theme_get_style(SF_STYLE_BG_CARD), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 0, 0);   /* Table doesn't need rounded corners */
        /* Do not set clip_corner: LVGL skips it anyway at radius=0, but this avoids triggering a
           full-screen offscreen-layer allocation if radius is enlarged later (LV_MEM is only 64KB;
           this once caused a Wi-Fi-scan-after refresh crash/dead-loop) */
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_set_style_pad_row(card, 0, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        switch (i) {
            case TAB_TASKS:   sf_monitor_tasks_build(card, priv); break;
            case TAB_REGIONS: sf_monitor_mem_build(card, priv);   break;
            case TAB_STORAGE: sf_monitor_disk_build(card, priv);  break;
            default: break;
        }
    }

    /* Keep the header title in sync when content is swiped sideways */
    lv_obj_add_event_cb(tv, tab_changed_cb, LV_EVENT_VALUE_CHANGED, priv);

    /* ── Bottom page indicator dots (hint that the content can be swiped) ── */
    lv_obj_t *dotbar = lv_obj_create(content);
    lv_obj_remove_style_all(dotbar);
    lv_obj_set_width(dotbar, LV_PCT(100));
    lv_obj_set_height(dotbar, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(dotbar, 8, 0);
    lv_obj_set_style_pad_column(dotbar, 6, 0);
    lv_obj_set_flex_flow(dotbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dotbar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(dotbar, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_t *dot = lv_obj_create(dotbar);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 7, 7);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_add_style(dot, sf_theme_get_style(SF_STYLE_BG_SEPARATOR), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        priv->dots[i] = dot;
    }

    update_header(priv);   /* Initialize the title + highlight the first page dot */

    lvgl_port_unlock();

    ESP_LOGI(TAG, "on_create (tabview + Tasks lv_table + Regions chart/bars + Storage bars)");
    return ESP_OK;
}

static void on_start(sf_app_ctx_t *ctx)  { ESP_LOGI(TAG, "on_start"); }
static void on_stop(sf_app_ctx_t *ctx)   { ESP_LOGI(TAG, "on_stop"); }

static void on_resume(sf_app_ctx_t *ctx)
{
    monitor_priv_t *priv = ctx->user_data;
    if (!priv) return;
    monitor_collect_start();              /* Start the background collection task (data keeps updating) */
    if (!priv->ui_timer) {               /* Only refresh_cb on the visible page actually refreshes */
        priv->ui_timer = lv_timer_create(refresh_cb, 1000, priv);
    }
    sf_monitor_mem_reset_chart(priv);   /* Entering the app: clear line-chart history, realtime only */
    ESP_LOGI(TAG, "on_resume");
}

static void on_pause(sf_app_ctx_t *ctx)
{
    monitor_priv_t *priv = ctx->user_data;
    if (priv && priv->ui_timer) {
        lv_timer_del(priv->ui_timer);
        priv->ui_timer = NULL;
    }
    monitor_collect_stop();               /* Stop the background collection task */
    ESP_LOGI(TAG, "on_pause");
}

static void on_destroy(sf_app_ctx_t *ctx)
{
    monitor_priv_t *priv = ctx->user_data;
    if (!priv) return;

    lvgl_port_lock(0);
    /* content is a root child; deleting it cascades to header/tabview/dotbar */
    lv_obj_t *root = sf_gui_app_get_root(ctx);
    if (root) lv_obj_clean(root);
    priv->tv = NULL;
    priv->title = NULL;
    priv->task_table = NULL;
    lvgl_port_unlock();

    free(priv);
    ctx->user_data = NULL;
    ESP_LOGI(TAG, "on_destroy");
}

static bool on_back(sf_app_ctx_t *ctx)
{
    /* Single-page app with no subpages; the back event is left to the system */
    return false;
}

static const sf_app_ops_t g_monitor_app_ops = {
    .on_create  = on_create,
    .on_start   = on_start,
    .on_resume  = on_resume,
    .on_pause   = on_pause,
    .on_stop    = on_stop,
    .on_destroy = on_destroy,
    .on_back    = on_back,
};

static const sf_app_intent_filter_t s_intent_filters[] = {
    SF_APP_INTENT_FILTER(SF_INTENT_ACTION_VIEW, SF_INTENT_CATEGORY_DEFAULT),
};

const sf_app_manifest_t g_monitor_app_manifest = {
    .id      = "monitor",
    .name    = "Monitor",
    .icon    = LV_SYMBOL_DRIVE,
    .version = "1.0.0",
    .flags   = SF_APP_FLAG_SHOW_IN_LAUNCHER,
    .ops     = &g_monitor_app_ops,
    .intent_filters = s_intent_filters,
    .intent_filters_count = 1,
};
