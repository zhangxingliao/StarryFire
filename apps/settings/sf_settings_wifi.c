/**
 * sf_settings_wifi.c — Wi-Fi settings page
 *
 * Features:
 * - Wi-Fi toggle switch
 * - Scanned network list (grouped into saved/other networks, phone-like UI)
 * - Password entry (LVGL keyboard)
 * - Connection status indicators (spinner while connecting, checkmark when connected)
 */
#include "sf_settings_pages.h"
#include "sf_sys.h"
#include "sf_wifi.h"
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_lvgl_port.h"
#include "sf_theme.h"

static const char *TAG = "sf_settings_wifi";

/* ── Forward declarations ─────────────────────────────────────── */

typedef struct {
    settings_ctx_t *ctx;
    lv_obj_t *page;
    lv_obj_t *toggle_sw;
    lv_obj_t *list_cont;
    lv_obj_t *spinner_lbl;       /* scanning spinner (inside the toggle row) */
    lv_obj_t *conn_sig_lbl;     /* signal icon of the connected network (color refreshed by timer) */
    lv_timer_t *rssi_timer;     /* 2s timer: refreshes the connected network's signal */
    lv_timer_t *refresh_timer;  /* ~100ms timer (LVGL thread): aggregates event-driven refresh requests */
    volatile bool refresh_pending;  /* set by wifi_event_cb on the event task, consumed by refresh_timer */
    char pending_ssid[SF_WIFI_SSID_MAX_LEN];
} wifi_page_priv_t;

typedef struct {
    wifi_page_priv_t *page_priv;
    lv_obj_t *page;
    lv_obj_t *pwd_ta;
    lv_obj_t *kb;
    lv_obj_t *connect_btn;
    char ssid[SF_WIFI_SSID_MAX_LEN];
} wifi_pwd_priv_t;

/* Pointer to the currently active Wi-Fi page (for safe access from event callbacks) */
static wifi_page_priv_t *s_active_page = NULL;

/* Forward declarations */
static void network_item_click_cb(lv_event_t *e);
static void wifi_event_cb(esp_event_base_t base, int32_t id, void *event_data, void *user_data);
static void refresh_network_list(wifi_page_priv_t *priv);

/* ── Utility functions ─────────────────────────────────────── */

static lv_color_t rssi_to_color(int8_t rssi)
{
    if (rssi >= -55) return SF_COLOR_RSSI_GREEN;
    if (rssi >= -70) return SF_COLOR_RSSI_AMBER;
    return SF_COLOR_RSSI_RED;
}

/* ── RSSI timer callback (runs inside the LVGL thread) ────────────────── */

static void rssi_timer_cb(lv_timer_t *t)
{
    wifi_page_priv_t *priv = lv_timer_get_user_data(t);
    if (!priv || !priv->conn_sig_lbl) return;

    /* Only update while connected */
    if (sf_wifi_get_state() != SF_WIFI_STATE_CONNECTED) return;

    int8_t rssi = sf_wifi_get_rssi();
    lv_obj_set_style_text_color(priv->conn_sig_lbl, rssi_to_color(rssi), 0);
}

/* ── Creating network list items ───────────────────────────────── */

/*
 * Create a network list item.
 * rssi_label_out: output the signal icon label pointer (so it can be swapped for a spinner while connecting)
 */
static void create_network_item(lv_obj_t *parent, const sf_wifi_scan_result_t *r,
                                 wifi_page_priv_t *priv, int idx,
                                 lv_obj_t **rssi_label_out)
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
    lv_obj_set_style_min_height(item, 0, 0);
    lv_obj_set_scrollbar_mode(item, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(item, sf_theme_get_style(SF_STYLE_BG_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(item, LV_OPA_10, LV_STATE_PRESSED);

    /* Signal icon */
    lv_obj_t *sig_lbl = lv_label_create(item);
    lv_label_set_text(sig_lbl, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(sig_lbl, rssi_to_color(r->rssi), 0);
    lv_obj_set_width(sig_lbl, SF_UI(20));
    if (rssi_label_out) *rssi_label_out = sig_lbl;

    /* SSID */
    lv_obj_t *ssid_lbl = lv_label_create(item);
    lv_label_set_text(ssid_lbl, r->ssid);
    lv_label_set_long_mode(ssid_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_add_style(ssid_lbl, sf_theme_get_style(SF_STYLE_TXT_PRIMARY), 0);
    lv_obj_set_style_text_font(ssid_lbl, SF_FONT_SM, 0);
    lv_obj_set_flex_grow(ssid_lbl, 1);

    /* Status indicator: connected / connecting / encrypted */
    sf_wifi_state_t st = sf_wifi_get_state();
    bool is_pending = (priv->pending_ssid[0] &&
                       strcmp(priv->pending_ssid, r->ssid) == 0 &&
                       (st == SF_WIFI_STATE_CONNECTING));

    if (r->is_connected) {
        lv_obj_t *check_lbl = lv_label_create(item);
        lv_label_set_text(check_lbl, LV_SYMBOL_OK);
        lv_obj_add_style(check_lbl, sf_theme_get_style(SF_STYLE_TXT_ACTIVE), 0);
    } else if (is_pending) {
        /* Connecting: show spinner text */
        lv_obj_t *conn_lbl = lv_label_create(item);
        lv_label_set_text(conn_lbl, "...");
        lv_obj_add_style(conn_lbl, sf_theme_get_style(SF_STYLE_TXT_ACTIVE), 0);
    }

    /* Click event: store index +1 in user_data (to avoid 0 == NULL) */
    lv_obj_set_user_data(item, (void *)(intptr_t)(idx + 1));
    lv_obj_add_event_cb(item, network_item_click_cb, LV_EVENT_CLICKED, priv);
}

/* ── Refreshing the network list ─────────────────────────────────── */

static void refresh_network_list(wifi_page_priv_t *priv)
{
    if (!priv || !priv->list_cont) return;

    priv->conn_sig_lbl = NULL;  /* clear the pointer; it is reassigned when rebuilt below */
    lv_obj_clean(priv->list_cont);

    sf_wifi_state_t st = sf_wifi_get_state();

    /* The refresh button's appearance changes with state */
    if (priv->spinner_lbl) {
        if (st == SF_WIFI_STATE_SCANNING) {
            lv_obj_set_style_text_color(priv->spinner_lbl, SF_COLOR_ACTIVE, 0);
        } else {
            lv_obj_set_style_text_color(priv->spinner_lbl, SF_COLOR_TEXT_MUTED, 0);
        }
    }

    /* Show a hint when Wi-Fi is disabled */
    if (st == SF_WIFI_STATE_DISABLED) {
        lv_obj_t *lbl = lv_label_create(priv->list_cont);
        lv_label_set_text(lbl, "Turn on Wi-Fi to see\navailable networks");
        lv_obj_add_style(lbl, sf_theme_get_style(SF_STYLE_TXT_MUTED), 0);
        lv_obj_set_style_pad_top(lbl, SF_UI(24), 0);
        lv_obj_set_width(lbl, LV_PCT(100));
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    sf_wifi_scan_list_t scan;
    if (sf_wifi_get_scan_results(&scan) != ESP_OK) return;

    /* Handle the case with no scan results */
    if (scan.count == 0) {
        const char *cur_ssid = sf_wifi_get_ssid();

        /* Show a synthetic entry for saved networks even while scanning/connecting */
        if (cur_ssid && cur_ssid[0] &&
            (st == SF_WIFI_STATE_CONNECTED || st == SF_WIFI_STATE_CONNECTING ||
             st == SF_WIFI_STATE_SCANNING)) {
            /* Build a synthetic result to show the saved/connected network */
            sf_wifi_scan_result_t synthetic;
            memset(&synthetic, 0, sizeof(synthetic));
            strncpy(synthetic.ssid, cur_ssid, sizeof(synthetic.ssid) - 1);
            synthetic.rssi = -50;
            synthetic.is_connected = (st == SF_WIFI_STATE_CONNECTED);
            synthetic.is_saved = true;

            lv_obj_t *card = settings_create_category(priv->list_cont, "My Networks");

            /* idx=-1 → user_data is 0, so the click callback returns immediately */
            create_network_item(card, &synthetic, priv, -1,
                                synthetic.is_connected ? &priv->conn_sig_lbl : NULL);

            /* Show a hint below while scanning */
            if (st == SF_WIFI_STATE_SCANNING) {
                lv_obj_t *scan_lbl = lv_label_create(priv->list_cont);
                lv_label_set_text(scan_lbl, "Scanning for other networks...");
                lv_obj_add_style(scan_lbl, sf_theme_get_style(SF_STYLE_TXT_MUTED), 0);
                lv_obj_set_style_pad_top(scan_lbl, SF_UI(12), 0);
                lv_obj_set_width(scan_lbl, LV_PCT(100));
                lv_obj_set_style_text_align(scan_lbl, LV_TEXT_ALIGN_CENTER, 0);
            }
        } else if (st == SF_WIFI_STATE_SCANNING) {
            lv_obj_t *lbl = lv_label_create(priv->list_cont);
            lv_label_set_text(lbl, "Scanning...");
            lv_obj_add_style(lbl, sf_theme_get_style(SF_STYLE_TXT_MUTED), 0);
            lv_obj_set_style_pad_top(lbl, SF_UI(24), 0);
            lv_obj_set_width(lbl, LV_PCT(100));
            lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        } else {
            lv_obj_t *lbl = lv_label_create(priv->list_cont);
            lv_label_set_text(lbl, "No networks found\nTap " LV_SYMBOL_REFRESH " to scan");
            lv_obj_add_style(lbl, sf_theme_get_style(SF_STYLE_TXT_MUTED), 0);
            lv_obj_set_style_pad_top(lbl, SF_UI(24), 0);
            lv_obj_set_width(lbl, LV_PCT(100));
            lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        }
        return;
    }

    /* Separate saved and unsaved networks */
    int saved_indices[SF_WIFI_MAX_SCAN_RESULTS];
    int other_indices[SF_WIFI_MAX_SCAN_RESULTS];
    int saved_count = 0;
    int other_count = 0;

    for (int i = 0; i < scan.count; i++) {
        if (scan.results[i].is_saved) {
            saved_indices[saved_count++] = i;
        } else {
            other_indices[other_count++] = i;
        }
    }

    /* "My Networks" group */
    if (saved_count > 0) {
        lv_obj_t *card = settings_create_category(priv->list_cont, "My Networks");

        for (int i = 0; i < saved_count; i++) {
            int idx = saved_indices[i];
            create_network_item(card, &scan.results[idx], priv, idx,
                                scan.results[idx].is_connected ? &priv->conn_sig_lbl : NULL);

            /* Separator between items */
            if (i < saved_count - 1) {
                settings_create_item_sep(card);
            }
        }
    }

    /* "Other Networks" group */
    if (other_count > 0) {
        lv_obj_t *card = settings_create_category(priv->list_cont, "Other Networks");

        for (int i = 0; i < other_count; i++) {
            int idx = other_indices[i];
            create_network_item(card, &scan.results[idx], priv, idx,
                                scan.results[idx].is_connected ? &priv->conn_sig_lbl : NULL);

            /* Separator between items */
            if (i < other_count - 1) {
                settings_create_item_sep(card);
            }
        }
    }
}

/* ── Password entry page ─────────────────────────────────── */

static void pwd_back_cb(lv_event_t *e)
{
    wifi_pwd_priv_t *ppriv = lv_event_get_user_data(e);
    lvgl_port_lock(0);
    if (ppriv->page) {
        lv_obj_del(ppriv->page);
        ppriv->page = NULL;
    }
    lvgl_port_unlock();
    free(ppriv);
}

static void pwd_connect_cb(lv_event_t *e)
{
    wifi_pwd_priv_t *ppriv = lv_event_get_user_data(e);
    const char *pwd = lv_textarea_get_text(ppriv->pwd_ta);

    ESP_LOGI(TAG, "connecting to '%s' with password", ppriv->ssid);
    sf_wifi_connect(ppriv->ssid, pwd);

    /* Record the SSID being connected to (used by the UI to show "connecting" state) */
    if (ppriv->page_priv) {
        strncpy(ppriv->page_priv->pending_ssid, ppriv->ssid,
                sizeof(ppriv->page_priv->pending_ssid) - 1);
    }

    /* Return to the network list page */
    lvgl_port_lock(0);
    if (ppriv->page) {
        lv_obj_del(ppriv->page);
        ppriv->page = NULL;
    }
    /* Refresh the list to show the connecting state */
    if (ppriv->page_priv) {
        refresh_network_list(ppriv->page_priv);
    }
    lvgl_port_unlock();
    free(ppriv);
}

static void ta_focus_cb(lv_event_t *e)
{
    wifi_pwd_priv_t *ppriv = lv_event_get_user_data(e);
    if (!ppriv || !ppriv->kb) return;
    lv_keyboard_set_textarea(ppriv->kb, ppriv->pwd_ta);
}

static void show_password_page(wifi_page_priv_t *page_priv, const char *ssid)
{
    wifi_pwd_priv_t *ppriv = calloc(1, sizeof(wifi_pwd_priv_t));
    if (!ppriv) return;
    strncpy(ppriv->ssid, ssid, sizeof(ppriv->ssid) - 1);
    ppriv->page_priv = page_priv;

    lvgl_port_lock(0);

    lv_obj_t *page = lv_obj_create(page_priv->ctx->window);
    ppriv->page = page;
    lv_obj_remove_style_all(page);
    lv_obj_set_width(page, LV_PCT(100));
    lv_obj_set_height(page, LV_PCT(100));
    lv_obj_add_style(page, sf_theme_get_style(SF_STYLE_BG_PAGE), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    /* Header */
    settings_create_page_header(page, ssid, pwd_back_cb, ppriv);

    /* Separator */
    settings_create_separator(page);

    /* Password label */
    lv_obj_t *lbl = lv_label_create(page);
    lv_label_set_text(lbl, "Password");
    lv_obj_add_style(lbl, sf_theme_get_style(SF_STYLE_TXT_MUTED), 0);
    lv_obj_set_style_text_font(lbl, SF_FONT_SM, 0);
    lv_obj_set_style_pad_left(lbl, SF_UI(16), 0);
    lv_obj_set_style_pad_top(lbl, SF_UI(16), 0);
    lv_obj_set_style_pad_bottom(lbl, SF_UI(4), 0);

    /* Password input field */
    ppriv->pwd_ta = lv_textarea_create(page);
    lv_obj_set_width(ppriv->pwd_ta, LV_PCT(100));
    lv_obj_set_height(ppriv->pwd_ta, 40);
    lv_obj_add_style(ppriv->pwd_ta, sf_theme_get_style(SF_STYLE_BG_CARD), 0);
    lv_obj_set_style_bg_opa(ppriv->pwd_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ppriv->pwd_ta, 1, 0);
    lv_obj_add_style(ppriv->pwd_ta, sf_theme_get_style(SF_STYLE_BORDER_ACTIVE), 0);
    lv_obj_set_style_radius(ppriv->pwd_ta, 8, 0);
    lv_obj_set_style_pad_all(ppriv->pwd_ta, 6, 0);
    lv_obj_add_style(ppriv->pwd_ta, sf_theme_get_style(SF_STYLE_TXT_PRIMARY), 0);
    lv_obj_set_style_text_font(ppriv->pwd_ta, SF_FONT_SM, 0);
    lv_textarea_set_password_mode(ppriv->pwd_ta, true);
    lv_textarea_set_placeholder_text(ppriv->pwd_ta, "Enter password...");
    lv_textarea_set_max_length(ppriv->pwd_ta, SF_WIFI_PASS_MAX_LEN - 1);
    lv_obj_set_style_pad_left(ppriv->pwd_ta, SF_UI(12), 0);
    lv_obj_set_style_pad_right(ppriv->pwd_ta, SF_UI(12), 0);
    lv_obj_set_style_border_side(ppriv->pwd_ta, LV_BORDER_SIDE_FULL, LV_STATE_FOCUSED);

    lv_obj_add_event_cb(ppriv->pwd_ta, ta_focus_cb, LV_EVENT_FOCUSED, ppriv);
    lv_obj_add_event_cb(ppriv->pwd_ta, ta_focus_cb, LV_EVENT_CLICKED, ppriv);

    /* Connect button */
    ppriv->connect_btn = lv_obj_create(page);
    lv_obj_remove_style_all(ppriv->connect_btn);
    lv_obj_add_flag(ppriv->connect_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(ppriv->connect_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(ppriv->connect_btn, LV_PCT(100));
    lv_obj_set_height(ppriv->connect_btn, 40);
    lv_obj_set_style_radius(ppriv->connect_btn, 8, 0);
    lv_obj_add_style(ppriv->connect_btn, sf_theme_get_style(SF_STYLE_BG_ACTIVE), 0);
    lv_obj_set_style_bg_opa(ppriv->connect_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(ppriv->connect_btn, 0, 0);

    lv_obj_t *btn_lbl = lv_label_create(ppriv->connect_btn);
    lv_label_set_text(btn_lbl, "Connect");
    lv_obj_add_style(btn_lbl, sf_theme_get_style(SF_STYLE_TXT_PRIMARY), 0);
    lv_obj_set_style_text_font(btn_lbl, SF_FONT_SM, 0);
    lv_obj_center(btn_lbl);

    lv_obj_add_event_cb(ppriv->connect_btn, pwd_connect_cb, LV_EVENT_CLICKED, ppriv);

    /* Keyboard */
    ppriv->kb = lv_keyboard_create(page);
    lv_obj_add_style(ppriv->kb, sf_theme_get_style(SF_STYLE_BG_LAUNCHER), 0);
    lv_obj_set_style_bg_opa(ppriv->kb, LV_OPA_COVER, 0);
    lv_keyboard_set_textarea(ppriv->kb, ppriv->pwd_ta);

    lv_obj_move_foreground(page);
    lvgl_port_unlock();
}

/* ── Network list click ─────────────────────────────────── */

static void network_item_click_cb(lv_event_t *e)
{
    /* Use current_target to get the item object the callback was registered on */
    lv_obj_t *item = lv_event_get_current_target(e);
    if (!item) return;

    /* user_data holds idx+1 (to avoid 0 == NULL) */
    intptr_t ud = (intptr_t)lv_obj_get_user_data(item);
    if (ud == 0) return;
    int idx = (int)ud - 1;

    sf_wifi_scan_list_t scan;
    if (sf_wifi_get_scan_results(&scan) != ESP_OK) return;
    if (idx < 0 || idx >= scan.count) return;

    const sf_wifi_scan_result_t *r = &scan.results[idx];

    /* Ignore if already connected to this network */
    if (r->is_connected) return;

    wifi_page_priv_t *priv = s_active_page;
    if (!priv) return;

    strncpy(priv->pending_ssid, r->ssid, sizeof(priv->pending_ssid) - 1);

    if (r->authmode == 0) {
        /* Open network: connect directly */
        sf_wifi_connect(r->ssid, NULL);
        lvgl_port_lock(0);
        refresh_network_list(priv);
        lvgl_port_unlock();
    } else {
        /* Encrypted network: bring up the password entry */
        show_password_page(priv, r->ssid);
    }
}

/* ── Toggle / refresh callbacks ──────────────────────────────── */

static void toggle_cb(lv_event_t *e)
{
    wifi_page_priv_t *priv = lv_event_get_user_data(e);
    bool checked = lv_obj_has_state(priv->toggle_sw, LV_STATE_CHECKED);

    ESP_LOGI(TAG, "Wi-Fi toggle -> %s", checked ? "ON" : "OFF");
    sf_wifi_set_enabled(checked);

    lvgl_port_lock(0);
    if (checked) {
        /* Scan immediately after enabling */
        sf_wifi_start_scan();
    } else {
        /* Clear the pending SSID when disabling */
        priv->pending_ssid[0] = '\0';
    }
    refresh_network_list(priv);
    lvgl_port_unlock();
}

static void refresh_cb(lv_event_t *e)
{
    wifi_page_priv_t *priv = lv_event_get_user_data(e);
    ESP_LOGI(TAG, "refresh scan");

    sf_wifi_state_t st = sf_wifi_get_state();
    if (st == SF_WIFI_STATE_SCANNING) return;

    sf_wifi_start_scan();

    lvgl_port_lock(0);
    refresh_network_list(priv);
    lvgl_port_unlock();
}

/* ── Back callback ─────────────────────────────────────── */

static void wifi_back_cb(lv_event_t *e)
{
    settings_ctx_t *ctx = lv_event_get_user_data(e);
    settings_show_main(ctx);
}

/* ── Page delete callback (resource cleanup) ─────────────────────── */

static void wifi_page_delete_cb(lv_event_t *e)
{
    lv_obj_t *page = lv_event_get_target(e);
    wifi_page_priv_t *priv = lv_obj_get_user_data(page);
    if (!priv) return;

    /* Unsubscribe from Event Bus events */
    sf_event_unsubscribe(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event_cb, priv);
    sf_event_unsubscribe(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, wifi_event_cb, priv);
    sf_event_unsubscribe(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_cb, priv);

    /* Delete the RSSI timer */
    if (priv->rssi_timer) {
        lv_timer_del(priv->rssi_timer);
        priv->rssi_timer = NULL;
    }

    /* Delete the refresh timer */
    if (priv->refresh_timer) {
        lv_timer_del(priv->refresh_timer);
        priv->refresh_timer = NULL;
    }

    /* Clear the active page pointer */
    if (s_active_page == priv) {
        s_active_page = NULL;
    }

    free(priv);
    lv_obj_set_user_data(page, NULL);
    ESP_LOGI(TAG, "wifi page destroyed");
}

/* ── Event Bus callback (executed on the event task) ──────────── */

static void wifi_event_cb(esp_event_base_t base, int32_t id, void *event_data, void *user_data)
{
    wifi_page_priv_t *priv = (wifi_page_priv_t *)user_data;
    if (!priv || priv != s_active_page || !priv->page) return;

    /* Got IP → connected successfully, clear the pending entry */
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        priv->pending_ssid[0] = '\0';
    }

    /* Do NOT touch LVGL here: this callback runs on the esp_event task.
     * Only mark the page dirty; refresh_timer (LVGL thread) performs the
     * actual UI rebuild. */
    priv->refresh_pending = true;
}

/* Runs on the LVGL thread (timer): performs the deferred UI refresh that the
 * event callback requested. Keeps all LVGL access on the GUI thread. */
static void refresh_timer_cb(lv_timer_t *t)
{
    wifi_page_priv_t *priv = lv_timer_get_user_data(t);
    if (!priv || !priv->page || !priv->refresh_pending) return;

    priv->refresh_pending = false;

    /* Sync the toggle switch state with the actual Wi-Fi enabled flag */
    if (priv->toggle_sw) {
        bool en = sf_wifi_is_enabled();
        if (en) lv_obj_add_state(priv->toggle_sw, LV_STATE_CHECKED);
        else    lv_obj_clear_state(priv->toggle_sw, LV_STATE_CHECKED);
    }

    refresh_network_list(priv);
}

/* ── Page creation ─────────────────────────────────────── */

lv_obj_t *sf_settings_wifi_create(lv_obj_t *parent, settings_ctx_t *ctx)
{
    wifi_page_priv_t *priv = calloc(1, sizeof(wifi_page_priv_t));
    if (!priv) return NULL;
    priv->ctx = ctx;

    lvgl_port_lock(0);

    lv_obj_t *page = settings_page_create(parent);
    priv->page = page;

    /* Header */
    settings_create_page_header(page, "Wi-Fi", wifi_back_cb, ctx);

    /* Separator */
    settings_create_separator(page);

    /* Toggle row */
    lv_obj_t *toggle_row = lv_obj_create(page);
    lv_obj_remove_style_all(toggle_row);
    lv_obj_set_width(toggle_row, LV_PCT(100));
    lv_obj_set_height(toggle_row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(toggle_row, SF_UI(8), 0);
    lv_obj_set_style_pad_bottom(toggle_row, SF_UI(8), 0);
    lv_obj_set_style_pad_left(toggle_row, SF_UI(16), 0);
    lv_obj_set_style_pad_right(toggle_row, SF_UI(16), 0);
    lv_obj_set_style_pad_column(toggle_row, SF_UI(4), 0);
    lv_obj_set_scrollbar_mode(toggle_row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(toggle_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(toggle_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(toggle_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *wifi_icon = lv_label_create(toggle_row);
    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_add_style(wifi_icon, sf_theme_get_style(SF_STYLE_TXT_ACCENT), 0);
    lv_obj_set_style_text_font(wifi_icon, SF_FONT_SM, 0);
    lv_obj_set_width(wifi_icon, SF_UI(20));

    lv_obj_t *toggle_lbl = lv_label_create(toggle_row);
    lv_label_set_text(toggle_lbl, "Wi-Fi");
    lv_obj_add_style(toggle_lbl, sf_theme_get_style(SF_STYLE_TXT_PRIMARY), 0);
    lv_obj_set_style_text_font(toggle_lbl, SF_FONT_SM, 0);
    lv_obj_set_flex_grow(toggle_lbl, 1);

    /* Refresh button (always visible; turns blue while scanning) */
    priv->spinner_lbl = lv_label_create(toggle_row);
    lv_label_set_text(priv->spinner_lbl, LV_SYMBOL_REFRESH);
    lv_obj_add_style(priv->spinner_lbl, sf_theme_get_style(SF_STYLE_TXT_MUTED), 0);
    lv_obj_add_flag(priv->spinner_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(priv->spinner_lbl, refresh_cb, LV_EVENT_CLICKED, priv);

    /* Toggle switch */
    priv->toggle_sw = lv_switch_create(toggle_row);
    lv_obj_add_style(priv->toggle_sw, sf_theme_get_style(SF_STYLE_BG_SEP_PRIMARY), 0);
    lv_obj_add_style(priv->toggle_sw, sf_theme_get_style(SF_STYLE_BG_ACTIVE), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (sf_wifi_is_enabled()) {
        lv_obj_add_state(priv->toggle_sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(priv->toggle_sw, toggle_cb, LV_EVENT_VALUE_CHANGED, priv);

    /* Separator */
    settings_create_separator(page);

    /* Network list container (scrollable) */
    priv->list_cont = lv_obj_create(page);
    lv_obj_remove_style_all(priv->list_cont);
    lv_obj_set_width(priv->list_cont, LV_PCT(100));
    lv_obj_set_flex_grow(priv->list_cont, 1);
    lv_obj_set_style_pad_all(priv->list_cont, 0, 0);
    lv_obj_set_style_pad_left(priv->list_cont, 0, 0);
    lv_obj_set_style_pad_right(priv->list_cont, 0, 0);
    lv_obj_set_style_pad_top(priv->list_cont, SF_UI(8), 0);
    lv_obj_set_style_pad_row(priv->list_cont, 16, 0);
    lv_obj_set_scrollbar_mode(priv->list_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(priv->list_cont, LV_DIR_VER);
    lv_obj_set_flex_flow(priv->list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(priv->list_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* Register as the active page */
    s_active_page = priv;

    /* Subscribe to Event Bus events */
    sf_event_subscribe(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event_cb, priv);
    sf_event_subscribe(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, wifi_event_cb, priv);
    sf_event_subscribe(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_cb, priv);

    /* Initial refresh */
    refresh_network_list(priv);

    /* Create the RSSI timer (refreshes the connected network's signal every 2s) */
    priv->rssi_timer = lv_timer_create(rssi_timer_cb, 2000, priv);

    /* Create the refresh timer (LVGL thread): aggregates event-driven refresh
     * requests from wifi_event_cb (esp_event task) into periodic UI rebuilds */
    priv->refresh_timer = lv_timer_create(refresh_timer_cb, 100, priv);

    /* Auto-scan if Wi-Fi is on but there are no scan results yet */
    sf_wifi_scan_list_t scan;
    if (sf_wifi_is_enabled() &&
        sf_wifi_get_scan_results(&scan) == ESP_OK && scan.count == 0) {
        sf_wifi_start_scan();
    }

    lv_obj_set_user_data(page, priv);

    /* Register the page delete callback (resource cleanup) */
    lv_obj_add_event_cb(page, wifi_page_delete_cb, LV_EVENT_DELETE, NULL);

    lvgl_port_unlock();

    return page;
}
