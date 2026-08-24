#include "sf_gui.h"
#include "sf_theme.h"
#include "sf_wifi.h"
#include "sf_config.h"
#include "sf_ntp.h"
#include "esp_lvgl_port.h"
#include <time.h>
#include <stdlib.h>
#include <string.h>

static lv_obj_t *s_time_label;
static lv_obj_t *s_wifi_icon;
static lv_obj_t *s_battery_icon;
static lv_obj_t *s_ac_label;

/* Wi-Fi icon drawing params */
#define WIFI_ICON_W   18
#define WIFI_ICON_H   15

static int s_wifi_level = -1;

/* Battery state */
#define BAT_ICON_W   20
#define BAT_ICON_H   12

static bool   s_batt_present = false;
static uint8_t s_batt_percent = 0;
static bool   s_batt_charging = false;

/* ── Map RSSI to signal level ─────────────────────── */

static int rssi_to_level(int8_t rssi)
{
    if (rssi >= -60) return 2;
    if (rssi >= -70) return 1;
    return 0;
}

/* ── Custom-drawn Wi-Fi icon ─────────────────────── */

static void wifi_draw_cb(lv_event_t *e)
{
    lv_obj_t  *obj   = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);

    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);

    lv_point_t center;
    center.x = coords.x1 + WIFI_ICON_W / 2;
    center.y = coords.y1 + WIFI_ICON_H - 2;

    const lv_color_t col_on  = SF_COLOR_TEXT_PRIMARY;
    const lv_color_t col_off = SF_COLOR_DIM;

    bool connected = (s_wifi_level >= 0);

    static const uint16_t radii[2] = {7, 12};
    lv_draw_arc_dsc_t arc;
    for (int i = 0; i < 2; i++) {
        lv_draw_arc_dsc_init(&arc);
        arc.center      = center;
        arc.radius      = radii[i];
        arc.width       = 2;
        arc.start_angle = 225;
        arc.end_angle   = 315;
        arc.rounded     = 1;
        arc.color = (connected && s_wifi_level >= (i + 1)) ? col_on : col_off;
        arc.opa   = LV_OPA_COVER;
        lv_draw_arc(layer, &arc);
    }

    lv_area_t dot;
    dot.x1 = center.x - 2;
    dot.y1 = center.y - 2;
    dot.x2 = center.x + 1;
    dot.y2 = center.y + 1;
    lv_draw_rect_dsc_t dot_dsc;
    lv_draw_rect_dsc_init(&dot_dsc);
    dot_dsc.radius   = LV_RADIUS_CIRCLE;
    dot_dsc.bg_opa   = LV_OPA_COVER;
    dot_dsc.bg_color = connected ? col_on : col_off;
    lv_draw_rect(layer, &dot_dsc, &dot);
}

/* ── Custom-drawn battery icon ───────────────────── */

static lv_color_t battery_fill_color(void)
{
    if (s_batt_charging) return SF_COLOR_ACTIVE;
    if (s_batt_percent >= 60) return SF_COLOR_RSSI_GREEN;
    if (s_batt_percent >= 20) return SF_COLOR_RSSI_AMBER;
    return SF_COLOR_RSSI_RED;
}

static void battery_draw_cb(lv_event_t *e)
{
    lv_obj_t   *obj   = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);

    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);

    if (!s_batt_present) return;

    int x = coords.x1;
    int y = coords.y1;
    int w = coords.x2 - coords.x1 + 1;
    int h = coords.y2 - coords.y1 + 1;

    const lv_color_t fg = SF_COLOR_TEXT_PRIMARY;
    const lv_color_t fill = battery_fill_color();

    /* Battery body (rounded-rect outline) */
    int body_w = w - 3;
    int body_h = h;
    lv_area_t body;
    body.x1 = x;
    body.y1 = y;
    body.x2 = x + body_w - 1;
    body.y2 = y + body_h - 1;

    lv_draw_rect_dsc_t body_dsc;
    lv_draw_rect_dsc_init(&body_dsc);
    body_dsc.radius        = 2;
    body_dsc.border_width  = 1;
    body_dsc.border_color  = fg;
    body_dsc.border_opa    = LV_OPA_COVER;
    body_dsc.bg_opa        = LV_OPA_TRANSP;
    lv_draw_rect(layer, &body_dsc, &body);

    /* Battery terminal (small rect on the right) */
    lv_area_t term;
    term.x1 = x + body_w;
    term.y1 = y + body_h / 2 - 2;
    term.x2 = x + body_w + 2;
    term.y2 = y + body_h / 2 + 1;
    lv_draw_rect_dsc_t term_dsc;
    lv_draw_rect_dsc_init(&term_dsc);
    term_dsc.bg_opa    = LV_OPA_COVER;
    term_dsc.bg_color  = fg;
    term_dsc.radius    = 1;
    lv_draw_rect(layer, &term_dsc, &term);

    /* Charge fill (inset 2px to leave the border) */
    int fill_w = (body_w - 4) * s_batt_percent / 100;
    if (fill_w > 0) {
        lv_area_t fill_body;
        fill_body.x1 = x + 2;
        fill_body.y1 = y + 2;
        fill_body.x2 = x + 2 + fill_w - 1;
        fill_body.y2 = y + body_h - 3;

        lv_draw_rect_dsc_t fill_dsc;
        lv_draw_rect_dsc_init(&fill_dsc);
        fill_dsc.radius   = 1;
        fill_dsc.bg_opa   = LV_OPA_COVER;
        fill_dsc.bg_color = fill;
        lv_draw_rect(layer, &fill_dsc, &fill_body);

        /* White highlight overlaid while charging */
        if (s_batt_charging) {
            lv_draw_rect_dsc_t hi;
            lv_draw_rect_dsc_init(&hi);
            hi.radius   = 1;
            hi.bg_opa   = LV_OPA_30;
            hi.bg_color = lv_color_white();
            lv_draw_rect(layer, &hi, &fill_body);
        }
    }
}

/* ── Refresh Wi-Fi level ─────────────────────────── */

static void update_wifi_indicator(void)
{
    if (!s_wifi_icon) return;

    int level;
    sf_wifi_state_t st = sf_wifi_get_state();
    if (st == SF_WIFI_STATE_CONNECTED) {
        level = rssi_to_level(sf_wifi_get_rssi());
    } else {
        level = -1;
    }

    if (level != s_wifi_level) {
        s_wifi_level = level;
        lv_obj_invalidate(s_wifi_icon);
    }
}

/* ── Clock timer (1s) ────────────────────────────── */

static void clock_timer_cb(lv_timer_t *t)
{
    (void)t;

    if (s_time_label) {
        char buf[16];
        if (sf_ntp_get_local_time(buf, sizeof(buf))) {
            lv_label_set_text(s_time_label, buf);
        } else {
            lv_label_set_text(s_time_label, "--:--");
        }
    }

    update_wifi_indicator();
}

/* ── Status bar creation ─────────────────────────── */

lv_obj_t *sf_gui_status_bar_create(lv_obj_t *parent)
{
    lv_obj_t *sb = lv_obj_create(parent);
    lv_obj_set_width(sb, LV_PCT(100));
    lv_obj_set_height(sb, SF_UI(20));
    lv_obj_add_style(sb, sf_theme_get_style(SF_STYLE_STATUS_BAR), 0);
    lv_obj_set_scrollbar_mode(sb, LV_SCROLLBAR_MODE_OFF);

    /* Centered: real-time clock */
    s_time_label = lv_label_create(sb);
    lv_label_set_text(s_time_label, "--:--");
    lv_obj_add_style(s_time_label, sf_theme_get_style(SF_STYLE_TXT_PRIMARY), 0);
    lv_obj_set_style_text_font(s_time_label, SF_FONT_SM, 0);
    lv_obj_align(s_time_label, LV_ALIGN_CENTER, 0, 0);

    /* Left: custom-drawn battery icon */
    s_battery_icon = lv_obj_create(sb);
    lv_obj_remove_style_all(s_battery_icon);
    lv_obj_set_size(s_battery_icon, BAT_ICON_W, BAT_ICON_H);
    lv_obj_clear_flag(s_battery_icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_battery_icon, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_add_event_cb(s_battery_icon, battery_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    /* "AC" text shown when no battery (on by default; hidden by sf_gui_status_set_battery once one is present) */
    s_ac_label = lv_label_create(sb);
    lv_label_set_text(s_ac_label, "AC");
    lv_obj_add_style(s_ac_label, sf_theme_get_style(SF_STYLE_TXT_MUTED), 0);
    lv_obj_set_style_text_font(s_ac_label, SF_FONT_XS, 0);
    lv_obj_align(s_ac_label, LV_ALIGN_LEFT_MID, 4, 0);

    /* Right: custom-drawn Wi-Fi signal icon */
    s_wifi_icon = lv_obj_create(sb);
    lv_obj_remove_style_all(s_wifi_icon);
    lv_obj_set_size(s_wifi_icon, WIFI_ICON_W, WIFI_ICON_H);
    lv_obj_clear_flag(s_wifi_icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_wifi_icon, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(s_wifi_icon, wifi_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    clock_timer_cb(NULL);
    lv_timer_create(clock_timer_cb, 1000, NULL);

    return sb;
}

void sf_gui_status_set_time(const char *time_str)
{
    if (s_time_label) {
        lvgl_port_lock(0);
        lv_label_set_text(s_time_label, time_str);
        lvgl_port_unlock();
    }
}

void sf_gui_status_set_battery(uint8_t percent, bool charging)
{
    if (percent > 100) {
        s_batt_present = false;
    } else {
        s_batt_present = true;
        s_batt_percent = percent;
        s_batt_charging = charging;
    }
    if (s_battery_icon || s_ac_label) {
        lvgl_port_lock(0);
        if (s_battery_icon) {
            lv_obj_invalidate(s_battery_icon);
        }
        if (s_ac_label) {
            if (s_batt_present) {
                lv_obj_add_flag(s_ac_label, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_clear_flag(s_ac_label, LV_OBJ_FLAG_HIDDEN);
            }
        }
        lvgl_port_unlock();
    }
}
