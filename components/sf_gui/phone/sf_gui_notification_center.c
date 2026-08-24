#include "sf_gui_notification_center.h"
#include "sf_gui_phone.h"
#include "sf_sys.h"
#include "sf_notification.h"
#include "sf_theme.h"
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"

static const char *TAG = "sf_notif_center";

static lv_obj_t *s_overlay;
static lv_obj_t *s_panel;
static lv_obj_t *s_list;
static bool s_visible;
static lv_timer_t *s_timer;
static uint32_t s_list_gen;

#define BANNER_H SF_UI(48)
static lv_obj_t *s_banner;
static lv_timer_t *s_banner_timer;
static bool s_subscribed;

static void close_panel(void);
static void banner_dismiss(void);

static void navigate_action(const char *action)
{
    if (!action || !action[0]) return;

    char intent_buf[SF_NOTIF_ACTION_MAX];
    strncpy(intent_buf, action, sizeof(intent_buf) - 1);
    intent_buf[sizeof(intent_buf) - 1] = '\0';

    char *page = strchr(intent_buf, ':');
    if (page) {
        *page = '\0';
        page++;
        if (!page[0]) page = NULL;
    }

    sf_intent_extra_t extras[1];
    sf_intent_t intent = {
        .action = intent_buf,
        .category = NULL,
        .target_app_id = NULL,
        .extras = NULL,
        .extras_count = 0,
    };

    if (page) {
        extras[0].key = "page";
        extras[0].type = SF_EXTRA_STRING;
        extras[0].str_val = page;
        intent.extras = extras;
        intent.extras_count = 1;
    }

    ESP_LOGI(TAG, "navigate action=%s page=%s", intent_buf, page ? page : "NULL");
    esp_err_t err = sf_app_start_intent(&intent);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sf_app_start_intent failed: %s", esp_err_to_name(err));
    }
}

typedef struct {
    char action[SF_NOTIF_ACTION_MAX];
    uint32_t notif_id;
} nav_task_t;

static void do_navigate(void *user_data)
{
    nav_task_t *task = user_data;
    if (!task) return;
    ESP_LOGI(TAG, "do_navigate id=%u action=%s", (unsigned)task->notif_id, task->action);
    sf_notification_dismiss(task->notif_id);
    if (task->action[0]) navigate_action(task->action);
    free(task);
}

static uint32_t s_press_gen;
static uint32_t s_press_notif_id;
static char s_press_action[SF_NOTIF_ACTION_MAX];

static void on_card_press(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const sf_notification_t *n = sf_notification_get(idx);
    if (!n || !n->action[0]) {
        s_press_gen = 0;
        return;
    }
    s_press_gen = s_list_gen;
    s_press_notif_id = n->id;
    strncpy(s_press_action, n->action, sizeof(s_press_action) - 1);
    s_press_action[sizeof(s_press_action) - 1] = '\0';
}

static void on_card_clicked(lv_event_t *e)
{
    (void)e;
    if (!s_visible) return;
    if (s_press_gen != s_list_gen) return;

    nav_task_t *task = malloc(sizeof(nav_task_t));
    if (!task) return;
    strncpy(task->action, s_press_action, sizeof(task->action) - 1);
    task->action[sizeof(task->action) - 1] = '\0';
    task->notif_id = s_press_notif_id;

    close_panel();
    lv_async_call(do_navigate, task);
}

static void on_banner_click(lv_event_t *e)
{
    lv_obj_t *banner = lv_event_get_target(e);
    nav_task_t *task = lv_obj_get_user_data(banner);
    if (!task || !task->action[0]) return;

    lv_obj_set_user_data(banner, NULL);
    banner_dismiss();
    lv_async_call(do_navigate, task);
}

static void format_time(char *buf, size_t len, int64_t ts_ms)
{
    int64_t now = esp_log_timestamp();
    int64_t diff_s = (now - ts_ms) / 1000;
    if (diff_s < 60) {
        snprintf(buf, len, "now");
    } else if (diff_s < 3600) {
        snprintf(buf, len, "%" PRId64 "m", diff_s / 60);
    } else {
        snprintf(buf, len, "%" PRId64 "h", diff_s / 3600);
    }
}

static void refresh_list(void *user_data)
{
    (void)user_data;
    if (!s_list) return;
    s_list_gen++;
    lv_obj_clean(s_list);

    int count = sf_notification_get_count();

    if (count == 0) {
        lv_obj_set_flex_align(s_list,
            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *empty = lv_label_create(s_list);
        lv_label_set_text(empty, "No notifications");
        lv_obj_add_style(empty, sf_theme_get_style(SF_STYLE_TXT_MUTED), 0);
        lv_obj_set_style_text_font(empty, SF_FONT_SM, 0);
        return;
    }

    lv_obj_set_flex_align(s_list,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < count; i++) {
        const sf_notification_t *n = sf_notification_get(i);
        if (!n) break;

        lv_obj_t *card = lv_obj_create(s_list);
        lv_obj_remove_style_all(card);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(card, SF_UI(6), 0);
        lv_obj_add_style(card, sf_theme_get_style(SF_STYLE_BG_CARD), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_60, 0);
        lv_obj_set_style_radius(card, 6, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(card, SF_UI(8), 0);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(card, on_card_clicked, LV_EVENT_SHORT_CLICKED, NULL);
        lv_obj_add_event_cb(card, on_card_press, LV_EVENT_PRESSED, (void *)(intptr_t)i);

        lv_obj_t *accent = lv_obj_create(card);
        lv_obj_remove_style_all(accent);
        lv_obj_set_size(accent, 3, LV_PCT(100));
        lv_obj_add_style(accent, sf_theme_get_style(SF_STYLE_BG_ACCENT), 0);
        lv_obj_set_style_bg_opa(accent, n->read ? LV_OPA_0 : LV_OPA_COVER, 0);
        lv_obj_set_style_radius(accent, 2, 0);
        lv_obj_clear_flag(accent, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *icon_box = lv_obj_create(card);
        lv_obj_remove_style_all(icon_box);
        lv_obj_set_size(icon_box, SF_UI(24), SF_UI(24));
        lv_obj_set_style_radius(icon_box, 4, 0);
        lv_obj_add_style(icon_box, sf_theme_get_style(SF_STYLE_BG_ACCENT), 0);
        lv_obj_set_style_bg_opa(icon_box, LV_OPA_20, 0);
        lv_obj_set_flex_flow(icon_box, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(icon_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *icon = lv_label_create(icon_box);
        lv_label_set_text(icon, n->icon);
        lv_obj_add_style(icon, sf_theme_get_style(SF_STYLE_TXT_ACCENT), 0);
        lv_obj_set_style_text_font(icon, SF_FONT_XS, 0);

        lv_obj_t *col = lv_obj_create(card);
        lv_obj_remove_style_all(col);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_grow(col, 1);
        lv_obj_set_height(col, LV_SIZE_CONTENT);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *top_row = lv_obj_create(col);
        lv_obj_remove_style_all(top_row);
        lv_obj_set_width(top_row, LV_PCT(100));
        lv_obj_set_height(top_row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(top_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(top_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(top_row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(top_row, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *title = lv_label_create(top_row);
        lv_label_set_text(title, n->title);
        lv_obj_add_style(title, sf_theme_get_style(SF_STYLE_TXT_PRIMARY), 0);
        lv_obj_set_style_text_font(title, SF_FONT_SM, 0);
        lv_obj_set_flex_grow(title, 1);

        char time_str[16];
        format_time(time_str, sizeof(time_str), n->timestamp_ms);
        lv_obj_t *time_lbl = lv_label_create(top_row);
        lv_label_set_text(time_lbl, time_str);
        lv_obj_add_style(time_lbl, sf_theme_get_style(SF_STYLE_TXT_MUTED), 0);
        lv_obj_set_style_text_font(time_lbl, SF_FONT_XS, 0);

        if (n->body[0]) {
            lv_obj_t *body = lv_label_create(col);
            lv_label_set_text(body, n->body);
            lv_obj_add_style(body, sf_theme_get_style(SF_STYLE_TXT_SECONDARY), 0);
            lv_obj_set_style_text_font(body, SF_FONT_XS, 0);
            lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
            lv_obj_set_width(body, LV_PCT(100));
        }
    }
}

static void refresh_timestamps(lv_timer_t *t)
{
    (void)t;
    if (!s_list || !s_visible) return;
    int count = sf_notification_get_count();
    if (count == 0) return;

    uint32_t child_cnt = lv_obj_get_child_cnt(s_list);
    for (uint32_t i = 0; i < child_cnt; i++) {
        const sf_notification_t *n = sf_notification_get((int)i);
        if (!n) break;

        lv_obj_t *card = lv_obj_get_child(s_list, i);
        if (lv_obj_get_child_cnt(card) < 3) break;

        lv_obj_t *col = lv_obj_get_child(card, 2);
        if (lv_obj_get_child_cnt(col) < 1) break;
        lv_obj_t *top_row = lv_obj_get_child(col, 0);
        if (lv_obj_get_child_cnt(top_row) < 2) break;
        lv_obj_t *time_lbl = lv_obj_get_child(top_row, 1);

        char time_str[16];
        format_time(time_str, sizeof(time_str), n->timestamp_ms);
        lv_label_set_text(time_lbl, time_str);
    }
}

static void banner_hide_ready_cb(lv_anim_t *a)
{
    (void)a;
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
}

static void banner_dismiss(void)
{
    if (!s_banner) return;
    nav_task_t *prev = lv_obj_get_user_data(s_banner);
    if (prev) {
        free(prev);
        lv_obj_set_user_data(s_banner, NULL);
    }
    s_banner_timer = NULL;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_banner);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_values(&a, SF_UI(20), -BANNER_H);
    lv_anim_set_time(&a, 200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a, banner_hide_ready_cb);
    lv_anim_start(&a);
}

static void banner_auto_close_cb(lv_timer_t *t)
{
    (void)t;
    banner_dismiss();
}

static void show_banner(const sf_notification_t *n)
{
    if (!s_banner || !n) return;

    nav_task_t *prev = lv_obj_get_user_data(s_banner);
    if (prev) free(prev);

    nav_task_t *task = malloc(sizeof(nav_task_t));
    if (!task) return;
    strncpy(task->action, n->action, sizeof(task->action) - 1);
    task->action[sizeof(task->action) - 1] = '\0';
    task->notif_id = n->id;
    lv_obj_set_user_data(s_banner, task);

    if (s_banner_timer) {
        lv_timer_del(s_banner_timer);
        s_banner_timer = NULL;
    }

    lv_obj_t *icon_box = lv_obj_get_child(s_banner, 0);
    lv_obj_t *icon_lbl = lv_obj_get_child(icon_box, 0);
    lv_obj_t *col      = lv_obj_get_child(s_banner, 1);
    lv_obj_t *title    = lv_obj_get_child(col, 0);
    lv_obj_t *body     = lv_obj_get_child(col, 1);

    lv_label_set_text(icon_lbl, n->icon);
    lv_label_set_text(title, n->title);
    if (n->body[0]) {
        lv_label_set_text(body, n->body);
        lv_obj_clear_flag(body, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(body, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_set_y(s_banner, -BANNER_H);
    lv_obj_clear_flag(s_banner, LV_OBJ_FLAG_HIDDEN);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_banner);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_values(&a, -BANNER_H, SF_UI(20));
    lv_anim_set_time(&a, 200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    s_banner_timer = lv_timer_create(banner_auto_close_cb, 2000, NULL);
    lv_timer_set_repeat_count(s_banner_timer, 1);
}

static void on_show_banner(void *unused)
{
    (void)unused;
    if (sf_notification_get_count() > 0) show_banner(sf_notification_get(0));
}

static void on_notification_event(esp_event_base_t base, int32_t id,
                                   void *data, void *ctx)
{
    if (s_visible) {
        lv_async_call(refresh_list, NULL);
    } else {
        lv_async_call(on_show_banner, NULL);
    }
}

static void close_panel(void)
{
    if (!s_visible) return;
    s_visible = false;

    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }

    if (s_overlay) lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    if (s_panel)   lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);

    if (sf_gui_phone_bars_are_visible()) {
        lv_obj_clear_flag(sf_gui_phone_get_nav_bar(), LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_style(sf_gui_phone_get_status_bar(), sf_theme_get_style(SF_STYLE_BG_BAR), 0);
    lv_obj_set_style_bg_opa(sf_gui_phone_get_status_bar(), LV_OPA_10, 0);

    int count = sf_notification_get_count();
    for (int i = 0; i < count; i++) {
        const sf_notification_t *n = sf_notification_get(i);
        if (n) sf_notification_mark_read(n->id);
    }
}

static void overlay_click_cb(lv_event_t *e)
{
    (void)e;
    close_panel();
}

static void clear_all_cb(lv_event_t *e)
{
    (void)e;
    sf_notification_clear_all();
    refresh_list(NULL);
    close_panel();
}

lv_obj_t *sf_gui_notification_center_create(lv_obj_t *parent)
{
    /* Guard against duplicate creation: clean up old subscription and objects first */
    if (s_panel) {
        if (s_subscribed) {
            sf_event_unsubscribe(SF_EVENT_BASE, SF_EVENT_NOTIFICATION_POST,
                                 on_notification_event, NULL);
            s_subscribed = false;
        }
        lv_obj_del(s_overlay);
        lv_obj_del(s_panel);
        lv_obj_del(s_banner);
        s_overlay = NULL;
        s_panel = NULL;
        s_list = NULL;
        s_banner = NULL;
    }

    s_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_add_style(s_overlay, sf_theme_get_style(SF_STYLE_BG_LAUNCHER), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_30, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_overlay, overlay_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

    s_panel = lv_obj_create(parent);
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_size(s_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_add_style(s_panel, sf_theme_get_style(SF_STYLE_BG_LAUNCHER), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_panel, 0, 0);
    lv_obj_set_style_pad_top(s_panel, SF_UI(20), 0);
    lv_obj_set_scrollbar_mode(s_panel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *header = lv_obj_create(s_panel);
    lv_obj_remove_style_all(header);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, SF_UI(32));
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(header, SF_UI(10), 0);
    lv_obj_set_style_pad_right(header, SF_UI(10), 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_lbl = lv_label_create(header);
    lv_label_set_text(title_lbl, LV_SYMBOL_BELL " Notifications");
    lv_obj_add_style(title_lbl, sf_theme_get_style(SF_STYLE_TXT_PRIMARY), 0);
    lv_obj_set_style_text_font(title_lbl, SF_FONT_SM, 0);

    lv_obj_t *clear_btn = lv_label_create(header);
    lv_label_set_text(clear_btn, LV_SYMBOL_TRASH);
    lv_obj_add_style(clear_btn, sf_theme_get_style(SF_STYLE_TXT_ACCENT), 0);
    lv_obj_set_style_text_font(clear_btn, SF_FONT_SM, 0);
    lv_obj_add_flag(clear_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(clear_btn, clear_all_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_ext_click_area(clear_btn, 10);

    lv_obj_t *sep = lv_obj_create(s_panel);
    lv_obj_remove_style_all(sep);
    lv_obj_set_width(sep, LV_PCT(100));
    lv_obj_set_height(sep, 1);
    lv_obj_add_style(sep, sf_theme_get_style(SF_STYLE_BG_SEP_PRIMARY), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_10, 0);

    s_list = lv_obj_create(s_panel);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_style_pad_all(s_list, SF_UI(6), 0);
    lv_obj_set_style_pad_top(s_list, SF_UI(2), 0);
    lv_obj_set_style_pad_row(s_list, SF_UI(4), 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_clip_corner(s_list, true, 0);

    sf_event_subscribe(SF_EVENT_BASE, SF_EVENT_NOTIFICATION_POST,
                       on_notification_event, NULL);
    s_subscribed = true;

    s_banner = lv_button_create(parent);
    lv_obj_remove_style_all(s_banner);
    lv_obj_set_size(s_banner, LV_PCT(90), BANNER_H);
    lv_obj_set_pos(s_banner, LV_PCT(5), -BANNER_H);
    lv_obj_add_style(s_banner, sf_theme_get_style(SF_STYLE_BG_CARD), 0);
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_banner, 8, 0);
    lv_obj_set_style_pad_all(s_banner, SF_UI(6), 0);
    lv_obj_set_style_pad_column(s_banner, SF_UI(8), 0);
    lv_obj_set_style_border_width(s_banner, 0, 0);
    lv_obj_set_flex_flow(s_banner, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_banner, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_banner, on_banner_click, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_shadow_width(s_banner, 12, 0);
    lv_obj_set_style_shadow_opa(s_banner, LV_OPA_40, 0);
    lv_obj_set_style_shadow_offset_y(s_banner, 4, 0);

    lv_obj_t *icon_box = lv_obj_create(s_banner);
    lv_obj_remove_style_all(icon_box);
    lv_obj_set_size(icon_box, SF_UI(24), SF_UI(24));
    lv_obj_set_style_radius(icon_box, 4, 0);
    lv_obj_add_style(icon_box, sf_theme_get_style(SF_STYLE_BG_ACCENT), 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_20, 0);
    lv_obj_set_flex_flow(icon_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(icon_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *banner_icon = lv_label_create(icon_box);
    lv_obj_add_style(banner_icon, sf_theme_get_style(SF_STYLE_TXT_ACCENT), 0);
    lv_obj_set_style_text_font(banner_icon, SF_FONT_XS, 0);

    lv_obj_t *banner_col = lv_obj_create(s_banner);
    lv_obj_remove_style_all(banner_col);
    lv_obj_set_height(banner_col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(banner_col, 1);
    lv_obj_set_flex_flow(banner_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(banner_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(banner_col, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *banner_title = lv_label_create(banner_col);
    lv_obj_add_style(banner_title, sf_theme_get_style(SF_STYLE_TXT_PRIMARY), 0);
    lv_obj_set_style_text_font(banner_title, SF_FONT_SM, 0);
    lv_obj_set_flex_grow(banner_title, 1);

    lv_obj_t *banner_body = lv_label_create(banner_col);
    lv_obj_add_style(banner_body, sf_theme_get_style(SF_STYLE_TXT_SECONDARY), 0);
    lv_obj_set_style_text_font(banner_body, SF_FONT_XS, 0);
    lv_obj_set_width(banner_body, LV_PCT(100));
    lv_label_set_long_mode(banner_body, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);

    return s_panel;
}

void sf_gui_notification_center_show(void)
{
    if (s_visible) return;
    s_visible = true;

    if (s_banner_timer) {
        lv_timer_del(s_banner_timer);
        s_banner_timer = NULL;
    }
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    nav_task_t *prev_banner = lv_obj_get_user_data(s_banner);
    if (prev_banner) {
        free(prev_banner);
        lv_obj_set_user_data(s_banner, NULL);
    }

    if (s_overlay) {
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_panel) {
        lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_add_flag(sf_gui_phone_get_nav_bar(), LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_style(sf_gui_phone_get_status_bar(), sf_theme_get_style(SF_STYLE_BG_LAUNCHER), 0);
    lv_obj_set_style_bg_opa(sf_gui_phone_get_status_bar(), LV_OPA_90, 0);

    refresh_list(NULL);

    if (!s_timer) s_timer = lv_timer_create(refresh_timestamps, 10000, NULL);
}

void sf_gui_notification_center_hide(void)
{
    close_panel();
    banner_dismiss();
}

bool sf_gui_notification_center_is_visible(void)
{
    return s_visible;
}

void sf_gui_notification_center_toggle(void)
{
    if (s_visible) {
        close_panel();
    } else {
        sf_gui_notification_center_show();
    }
}
