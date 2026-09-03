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
#include "sf_config.h"
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
    lv_timer_t *scan_timer;     /* 10s timer: periodic background scan while the page is open */
    volatile bool refresh_pending;  /* set by wifi_event_cb on the event task, consumed by refresh_timer */
    lv_obj_t *pwd_page;        /* open password sheet (overlay on window); NULL when closed */
} wifi_page_priv_t;

typedef struct {
    wifi_page_priv_t *page_priv;
    lv_obj_t *page;
    lv_obj_t *pwd_ta;
    lv_obj_t *kb;
    lv_obj_t *connect_btn;
    lv_obj_t *sec_dd;          /* security type dropdown */
    lv_obj_t *identity_ta;     /* EAP identity (enterprise only) */
    lv_obj_t *username_ta;     /* EAP username (enterprise only) */
    lv_obj_t *identity_row;    /* container toggled hidden for non-enterprise */
    lv_obj_t *username_row;
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

/* ── Periodic scan timer callback (runs inside the LVGL thread) ────────────
 * Keeps the surrounding network list fresh: triggers a background scan every
 * 10s while the page is open. Skips when Wi-Fi is off or a scan is already
 * running (avoid re-entering esp_wifi_scan_start). */
static void scan_timer_cb(lv_timer_t *t)
{
    wifi_page_priv_t *priv = lv_timer_get_user_data(t);
    if (!priv) return;
    if (!sf_wifi_is_enabled()) return;
    if (sf_wifi_get_state() == SF_WIFI_STATE_SCANNING) return;
    sf_wifi_start_scan();
}

/* ── Security selection helpers (for the password/EAP page) ── */

static void ta_focus_cb(lv_event_t *e);  /* defined below; used by create_eap_row */

/* Dropdown options; the selected index maps 1:1 to sf_wifi_security_t */
static const char *k_sec_options =
    "Open\nWEP\nWPA\nWPA2\nWPA/WPA2\nWPA3\nWPA2 Enterprise\nWPA3 Enterprise";

static sf_wifi_security_t authmode_to_security(uint8_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_WEP:             return SF_WIFI_SEC_WEP;
    case WIFI_AUTH_WPA_PSK:         return SF_WIFI_SEC_WPA_PSK;
    case WIFI_AUTH_WPA2_PSK:        return SF_WIFI_SEC_WPA2_PSK;
    case WIFI_AUTH_WPA_WPA2_PSK:    return SF_WIFI_SEC_WPA_WPA2_PSK;
    case WIFI_AUTH_WPA3_PSK:        return SF_WIFI_SEC_WPA3_PSK;
    case WIFI_AUTH_WPA2_ENTERPRISE: return SF_WIFI_SEC_WPA2_ENTERPRISE;
    case WIFI_AUTH_WPA3_ENTERPRISE: return SF_WIFI_SEC_WPA3_ENTERPRISE;
    default:                        return SF_WIFI_SEC_WPA_WPA2_PSK;
    }
}

static bool sec_is_enterprise(sf_wifi_security_t s)
{
    return s == SF_WIFI_SEC_WPA2_ENTERPRISE || s == SF_WIFI_SEC_WPA3_ENTERPRISE;
}

static void sec_dd_cb(lv_event_t *e)
{
    wifi_pwd_priv_t *ppriv = lv_event_get_user_data(e);
    if (!ppriv) return;
    bool ent = sec_is_enterprise((sf_wifi_security_t)lv_dropdown_get_selected(ppriv->sec_dd));
    lvgl_port_lock(0);
    if (ent) {
        lv_obj_clear_flag(ppriv->identity_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ppriv->username_row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ppriv->identity_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ppriv->username_row, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
}

/* Create a label + textarea row (used for EAP Identity / Username) */
static lv_obj_t *create_eap_row(lv_obj_t *parent, const char *label_text,
                                const char *placeholder, wifi_pwd_priv_t *ppriv,
                                lv_obj_t **ta_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(row, 0, 0);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label_text);
    lv_obj_add_style(lbl, sf_theme_get_style(SF_STYLE_TXT_MUTED), 0);
    lv_obj_set_style_text_font(lbl, SF_FONT_SM, 0);
    lv_obj_set_style_pad_left(lbl, SF_UI(16), 0);
    lv_obj_set_style_pad_top(lbl, SF_UI(16), 0);
    lv_obj_set_style_pad_bottom(lbl, SF_UI(4), 0);

    lv_obj_t *ta = lv_textarea_create(row);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_obj_set_height(ta, 40);
    lv_obj_add_style(ta, sf_theme_get_style(SF_STYLE_BG_CARD), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_add_style(ta, sf_theme_get_style(SF_STYLE_BORDER_ACTIVE), 0);
    lv_obj_set_style_radius(ta, 8, 0);
    lv_obj_set_style_pad_all(ta, 6, 0);
    lv_obj_add_style(ta, sf_theme_get_style(SF_STYLE_TXT_PRIMARY), 0);
    lv_obj_set_style_text_font(ta, SF_FONT_SM, 0);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_max_length(ta, SF_WIFI_PASS_MAX_LEN - 1);
    lv_obj_set_style_pad_left(ta, SF_UI(12), 0);
    lv_obj_set_style_pad_right(ta, SF_UI(12), 0);
    lv_obj_set_style_border_side(ta, LV_BORDER_SIDE_FULL, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_FOCUSED, ppriv);
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_CLICKED, ppriv);

    if (ta_out) *ta_out = ta;
    return row;
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

    /* Status indicator: derive live from the connection state so it stays in
     * sync with Wi-Fi events (no stale per-scan flags, no polling).
     * - connected  : this item IS the current target and IP is up  -> checkmark
     * - connecting : this item IS the current target but not yet up -> "..."
     *                (covers CONNECTING and the DISCONNECTED-reconnect phase of a
     *                 manual switch; the previous network is no longer the target,
     *                 so it correctly shows nothing)
     * - otherwise  : no marker (per request, "Secured" is not shown) */
    const char *cur_ssid = sf_wifi_get_ssid();
    sf_wifi_state_t st = sf_wifi_get_state();
    bool is_target = (cur_ssid[0] && strcmp(cur_ssid, r->ssid) == 0);

    if (is_target && st == SF_WIFI_STATE_CONNECTED) {
        lv_obj_t *check_lbl = lv_label_create(item);
        lv_label_set_text(check_lbl, LV_SYMBOL_OK);
        lv_obj_add_style(check_lbl, sf_theme_get_style(SF_STYLE_TXT_ACTIVE), 0);
    } else if (is_target && sf_wifi_is_connecting()) {
        /* Attempting to connect to this network (first connect, switch, or drop) */
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
    const char *cur = sf_wifi_get_ssid();

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

        /* Show a synthetic entry for the saved/connected network while connected,
         * connecting, scanning, or reconnecting after a drop */
        if (cur_ssid && cur_ssid[0] &&
            (st == SF_WIFI_STATE_CONNECTED || st == SF_WIFI_STATE_CONNECTING ||
             st == SF_WIFI_STATE_SCANNING || st == SF_WIFI_STATE_DISCONNECTED)) {
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
            } else if (sf_wifi_is_connecting()) {
                /* Attempting to reach the current target: first connect, manual
                   switch, or reconnect after a drop. */
                const char *hint = sf_wifi_is_attempt_active() ? "Connecting..." : "Reconnecting...";
                lv_obj_t *con_lbl = lv_label_create(priv->list_cont);
                lv_label_set_text(con_lbl, hint);
                lv_obj_add_style(con_lbl, sf_theme_get_style(SF_STYLE_TXT_MUTED), 0);
                lv_obj_set_style_pad_top(con_lbl, SF_UI(12), 0);
                lv_obj_set_width(con_lbl, LV_PCT(100));
                lv_obj_set_style_text_align(con_lbl, LV_TEXT_ALIGN_CENTER, 0);
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
                                (strcmp(scan.results[idx].ssid, cur) == 0 &&
                                 st == SF_WIFI_STATE_CONNECTED) ? &priv->conn_sig_lbl : NULL);

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
                                (strcmp(scan.results[idx].ssid, cur) == 0 &&
                                 st == SF_WIFI_STATE_CONNECTED) ? &priv->conn_sig_lbl : NULL);

            /* Separator between items */
            if (i < other_count - 1) {
                settings_create_item_sep(card);
            }
        }
    }
}

/* ── Password entry page ─────────────────────────────────── */

/* Frees the password-sheet private struct when its page object is deleted.
 * Covers both the Connect path and the app/system back path, so ppriv never
 * leaks. */
static void pwd_page_delete_cb(lv_event_t *e)
{
    wifi_pwd_priv_t *ppriv = lv_event_get_user_data(e);
    if (ppriv) {
        if (ppriv->page_priv) ppriv->page_priv->pwd_page = NULL;
        free(ppriv);
    }
}

/* Called by the app-level back handler. If the password sheet is open, close it
 * and return true so the system back gesture dismisses the sheet and returns to
 * the Wi-Fi list (instead of jumping straight to the home page). */
bool sf_settings_wifi_dismiss_sheet(void)
{
    wifi_page_priv_t *priv = s_active_page;
    if (priv && priv->pwd_page) {
        lvgl_port_lock(0);
        lv_obj_del(priv->pwd_page);   /* fires pwd_page_delete_cb -> clears pwd_page + frees ppriv */
        lvgl_port_unlock();
        return true;
    }
    return false;
}

static void pwd_connect_cb(lv_event_t *e)
{
    wifi_pwd_priv_t *ppriv = lv_event_get_user_data(e);
    const char *pwd = lv_textarea_get_text(ppriv->pwd_ta);
    sf_wifi_security_t sec = (sf_wifi_security_t)lv_dropdown_get_selected(ppriv->sec_dd);

    sf_wifi_connect_params_t p = {
        .ssid = ppriv->ssid,
        .security = sec,
        .pass = pwd,
        .identity = NULL,
        .username = NULL,
    };
    if (sec_is_enterprise(sec)) {
        p.identity = lv_textarea_get_text(ppriv->identity_ta);
        p.username = lv_textarea_get_text(ppriv->username_ta);
    }

    ESP_LOGI(TAG, "connecting to '%s' (security=%d)", ppriv->ssid, sec);
    sf_wifi_connect(&p);

    /* Return to the network list. Deleting the page triggers pwd_page_delete_cb
     * (which frees ppriv), so clear the back-reference first and refresh via a
     * saved pointer to avoid a use-after-free. */
    wifi_page_priv_t *np = ppriv->page_priv;
    lvgl_port_lock(0);
    lv_obj_del(ppriv->page);
    np->pwd_page = NULL;
    refresh_network_list(np);
    lvgl_port_unlock();
}

static void ta_focus_cb(lv_event_t *e)
{
    wifi_pwd_priv_t *ppriv = lv_event_get_user_data(e);
    if (!ppriv || !ppriv->kb) return;
    lv_keyboard_set_textarea(ppriv->kb, lv_event_get_target(e));
}

static void show_password_page(wifi_page_priv_t *page_priv, const char *ssid, sf_wifi_security_t sec_hint)
{
    wifi_pwd_priv_t *ppriv = calloc(1, sizeof(wifi_pwd_priv_t));
    if (!ppriv) return;
    strncpy(ppriv->ssid, ssid, sizeof(ppriv->ssid) - 1);
    ppriv->page_priv = page_priv;

    lvgl_port_lock(0);

    lv_obj_t *page = lv_obj_create(page_priv->ctx->window);
    ppriv->page = page;
    page_priv->pwd_page = page;
    lv_obj_add_event_cb(page, pwd_page_delete_cb, LV_EVENT_DELETE, ppriv);
    lv_obj_remove_style_all(page);
    lv_obj_set_width(page, LV_PCT(100));
    lv_obj_set_height(page, LV_PCT(100));
    lv_obj_add_style(page, sf_theme_get_style(SF_STYLE_BG_PAGE), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    /* Scrollable form area: holds the header and all inputs; the keyboard and
     * the Connect button live below it (on `page`), so the form can scroll to
     * reach them instead of being clipped off-screen. */
    lv_obj_t *form_cont = lv_obj_create(page);
    lv_obj_remove_style_all(form_cont);
    lv_obj_set_width(form_cont, LV_PCT(100));
    lv_obj_set_flex_grow(form_cont, 1);
    lv_obj_set_style_pad_top(form_cont, SF_UI(8), 0);
    lv_obj_set_style_pad_left(form_cont, 0, 0);
    lv_obj_set_style_pad_right(form_cont, 0, 0);
    lv_obj_set_style_pad_bottom(form_cont, 0, 0);
    lv_obj_set_scrollbar_mode(form_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(form_cont, LV_DIR_VER);
    lv_obj_set_flex_flow(form_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(form_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* Security type */
    lv_obj_t *sec_lbl = lv_label_create(form_cont);
    lv_label_set_text(sec_lbl, "Security");
    lv_obj_add_style(sec_lbl, sf_theme_get_style(SF_STYLE_TXT_MUTED), 0);
    lv_obj_set_style_text_font(sec_lbl, SF_FONT_SM, 0);
    lv_obj_set_style_pad_left(sec_lbl, SF_UI(16), 0);
    lv_obj_set_style_pad_top(sec_lbl, SF_UI(16), 0);
    lv_obj_set_style_pad_bottom(sec_lbl, SF_UI(4), 0);

    ppriv->sec_dd = lv_dropdown_create(form_cont);
    lv_obj_set_width(ppriv->sec_dd, LV_PCT(100));
    lv_dropdown_set_options(ppriv->sec_dd, k_sec_options);
    lv_obj_add_style(ppriv->sec_dd, sf_theme_get_style(SF_STYLE_BG_CARD), 0);
    lv_obj_set_style_bg_opa(ppriv->sec_dd, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ppriv->sec_dd, 1, 0);
    lv_obj_add_style(ppriv->sec_dd, sf_theme_get_style(SF_STYLE_BORDER_ACTIVE), 0);
    lv_obj_set_style_radius(ppriv->sec_dd, 8, 0);
    lv_obj_set_style_pad_all(ppriv->sec_dd, 6, 0);
    lv_obj_add_style(ppriv->sec_dd, sf_theme_get_style(SF_STYLE_TXT_PRIMARY), 0);
    lv_obj_set_style_text_font(ppriv->sec_dd, SF_FONT_SM, 0);
    lv_dropdown_set_selected(ppriv->sec_dd, sec_hint);
    lv_obj_add_event_cb(ppriv->sec_dd, sec_dd_cb, LV_EVENT_VALUE_CHANGED, ppriv);

    /* Password label */
    lv_obj_t *lbl = lv_label_create(form_cont);
    lv_label_set_text(lbl, "Password");
    lv_obj_add_style(lbl, sf_theme_get_style(SF_STYLE_TXT_MUTED), 0);
    lv_obj_set_style_text_font(lbl, SF_FONT_SM, 0);
    lv_obj_set_style_pad_left(lbl, SF_UI(16), 0);
    lv_obj_set_style_pad_top(lbl, SF_UI(16), 0);
    lv_obj_set_style_pad_bottom(lbl, SF_UI(4), 0);

    /* Password input field */
    ppriv->pwd_ta = lv_textarea_create(form_cont);
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

    /* Enterprise (EAP) fields - only relevant for WPA2/WPA3 Enterprise */
    ppriv->identity_row = create_eap_row(form_cont, "Identity", "EAP identity...", ppriv, &ppriv->identity_ta);
    ppriv->username_row = create_eap_row(form_cont, "Username", "EAP username...", ppriv, &ppriv->username_ta);
    if (!sec_is_enterprise(sec_hint)) {
        lv_obj_add_flag(ppriv->identity_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ppriv->username_row, LV_OBJ_FLAG_HIDDEN);
    }

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

    /* Keyboard: fixed height, pinned at the bottom of `page` (below the form
     * and the Connect button). The form scrolls in the area above it. */
    ppriv->kb = lv_keyboard_create(page);
    lv_obj_add_style(ppriv->kb, sf_theme_get_style(SF_STYLE_BG_LAUNCHER), 0);
    lv_obj_set_style_bg_opa(ppriv->kb, LV_OPA_COVER, 0);
    lv_obj_set_width(ppriv->kb, LV_PCT(100));
    lv_obj_set_height(ppriv->kb, LV_PCT(42));
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

    wifi_profile_t pr;
    if (r->authmode == 0) {
        /* Open network: connect directly */
        sf_wifi_connect_params_t p = {
            .ssid = r->ssid,
            .security = SF_WIFI_SEC_OPEN,
            .pass = NULL,
        };
        sf_wifi_connect(&p);
        lvgl_port_lock(0);
        refresh_network_list(priv);
        lvgl_port_unlock();
    } else if (sf_config_get_wifi_profile_by_ssid(r->ssid, &pr)) {
        /* Encrypted but already configured: connect with stored credentials,
           no need to re-enter the password. */
        sf_wifi_connect_params_t p = {
            .ssid     = pr.ssid,
            .security = (sf_wifi_security_t)pr.security,
            .pass     = pr.pass[0] ? pr.pass : NULL,
            .identity = pr.identity[0] ? pr.identity : NULL,
            .username = pr.username[0] ? pr.username : NULL,
        };
        sf_wifi_connect(&p);
        lvgl_port_lock(0);
        refresh_network_list(priv);
        lvgl_port_unlock();
    } else {
        /* Encrypted and not yet configured: bring up the password entry
           (preselect security from scan). */
        show_password_page(priv, r->ssid, authmode_to_security(r->authmode));
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

    /* Delete the periodic scan timer */
    if (priv->scan_timer) {
        lv_timer_del(priv->scan_timer);
        priv->scan_timer = NULL;
    }

    /* Close any open password sheet. It lives on the window as a sibling of
     * this page, so LVGL will not cascade-delete it automatically when this
     * page is destroyed (e.g. via the system/app back). */
    if (priv->pwd_page) {
        lv_obj_del(priv->pwd_page);   /* fires pwd_page_delete_cb → frees ppriv */
        priv->pwd_page = NULL;
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

    /* Proactively scan once when the page opens so the list is fresh, then keep
     * it updated with a 10s periodic background scan while the page stays open. */
    if (sf_wifi_is_enabled() && sf_wifi_get_state() != SF_WIFI_STATE_SCANNING) {
        sf_wifi_start_scan();
    }
    priv->scan_timer = lv_timer_create(scan_timer_cb, 10000, priv);

    lv_obj_set_user_data(page, priv);

    /* Register the page delete callback (resource cleanup) */
    lv_obj_add_event_cb(page, wifi_page_delete_cb, LV_EVENT_DELETE, NULL);

    lvgl_port_unlock();

    return page;
}
