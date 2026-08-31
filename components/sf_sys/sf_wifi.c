/**
 * sf_wifi.c — StarryFire Wi-Fi service implementation
 *
 * Wraps ESP-IDF Wi-Fi STA mode.
 * Credentials are stored in the SPIFFS JSON configuration (via the sf_config module).
 * State-change listeners should register ESP-IDF WIFI_EVENT / IP_EVENT handlers directly.
 */
#include "sf_wifi.h"
#include "sf_config.h"
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_eap_client.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

static const char *TAG = "sf_wifi";

/* Bit set when the scan finishes */
#define BIT_SCAN_DONE   (1 << 0)

static sf_wifi_state_t s_state = SF_WIFI_STATE_DISABLED;
static char s_ssid[SF_WIFI_SSID_MAX_LEN] = {0};  /* target / current SSID; drives reconnect, persists across disable */
static char s_cur_ip[16] = {0};
static bool s_inited = false;
static bool s_enabled = false;
static bool s_scan_requested = false;   /* set to delay a retry when the scan fails before STA_START */

static EventGroupHandle_t s_event_group = NULL;
static sf_wifi_scan_list_t s_scan_list = {0};
static SemaphoreHandle_t s_scan_mutex = NULL;   /* protects s_scan_list (esp_event task ↔ GUI task) */
static esp_netif_t *s_sta_netif = NULL;

/* True once any connection has ever succeeded; distinguishes a drop (show
   DISCONNECTED) from the initial connection attempt (show CONNECTING). */
static bool s_ever_connected = false;

/* Target network to connect/reconnect to. Held in memory and drives reconnect;
   only persisted (via sf_config) once a connection actually succeeds. */
static char s_target_pass[SF_WIFI_PASS_MAX_LEN] = {0};
static sf_wifi_security_t s_target_security = SF_WIFI_SEC_OPEN;
static char s_target_identity[SF_WIFI_PASS_MAX_LEN] = {0};
static char s_target_username[SF_WIFI_PASS_MAX_LEN] = {0};

/* ── Internal helpers ─────────────────────────────────── */

static void set_state(sf_wifi_state_t new_state)
{
    if (s_state == new_state) return;
    s_state = new_state;
    ESP_LOGI(TAG, "state -> %d", (int)new_state);
}

/* Copy the requested network into the in-memory target (drives reconnect). */
static void store_target(const sf_wifi_connect_params_t *p)
{
    strncpy(s_ssid, p->ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    strncpy(s_target_pass, p->pass ? p->pass : "", sizeof(s_target_pass) - 1);
    s_target_pass[sizeof(s_target_pass) - 1] = '\0';
    s_target_security = p->security;
    strncpy(s_target_identity, p->identity ? p->identity : "", sizeof(s_target_identity) - 1);
    s_target_identity[sizeof(s_target_identity) - 1] = '\0';
    strncpy(s_target_username, p->username ? p->username : "", sizeof(s_target_username) - 1);
    s_target_username[sizeof(s_target_username) - 1] = '\0';
}

static wifi_auth_mode_t security_to_authmode(sf_wifi_security_t s)
{
    switch (s) {
    case SF_WIFI_SEC_WEP:             return WIFI_AUTH_WEP;
    case SF_WIFI_SEC_WPA_PSK:         return WIFI_AUTH_WPA_PSK;
    case SF_WIFI_SEC_WPA2_PSK:        return WIFI_AUTH_WPA2_PSK;
    case SF_WIFI_SEC_WPA_WPA2_PSK:    return WIFI_AUTH_WPA_WPA2_PSK;
    case SF_WIFI_SEC_WPA3_PSK:        return WIFI_AUTH_WPA3_PSK;
    case SF_WIFI_SEC_WPA2_ENTERPRISE: return WIFI_AUTH_WPA2_ENTERPRISE;
    case SF_WIFI_SEC_WPA3_ENTERPRISE: return WIFI_AUTH_WPA3_ENTERPRISE;
    case SF_WIFI_SEC_OPEN:
    default:                          return WIFI_AUTH_OPEN;
    }
}

static bool is_enterprise(sf_wifi_security_t s)
{
    return s == SF_WIFI_SEC_WPA2_ENTERPRISE || s == SF_WIFI_SEC_WPA3_ENTERPRISE;
}

/* Build the station config from the in-memory target and apply it.
   Config is applied before the radio is brought up (set_config before start). */
static void apply_sta_config(void)
{
    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, s_ssid, sizeof(wifi_cfg.sta.ssid) - 1);

    /* failure_retry_cnt takes effect only with scan_method = WIFI_ALL_CHANNEL_SCAN. */
    wifi_cfg.sta.failure_retry_cnt = 5;
    wifi_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_cfg.sta.threshold.authmode = security_to_authmode(s_target_security);

    if (is_enterprise(s_target_security)) {
        /* Enterprise auth is via EAP; the PSK field is unused. */
        if (s_target_identity[0])
            esp_eap_client_set_identity((const unsigned char *)s_target_identity,
                                        strlen(s_target_identity));
        if (s_target_username[0])
            esp_eap_client_set_username((const unsigned char *)s_target_username,
                                        strlen(s_target_username));
        if (s_target_pass[0])
            esp_eap_client_set_password((const unsigned char *)s_target_pass,
                                        strlen(s_target_pass));
        esp_wifi_sta_enterprise_enable();
    } else {
        if (s_target_pass[0]) {
            strncpy((char *)wifi_cfg.sta.password, s_target_pass,
                    sizeof(wifi_cfg.sta.password) - 1);
        }
        esp_wifi_sta_enterprise_disable();
    }

    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
}

/* Bring the Wi-Fi driver up exactly once (paired with esp_wifi_stop in set_enabled(false)). */
static esp_err_t ensure_started(void)
{
    if (s_enabled) return ESP_OK;
    esp_err_t err = esp_wifi_start();
    if (err == ESP_OK) s_enabled = true;
    return err;
}

/* ── Wi-Fi event callbacks ─────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA started");
            if (s_scan_requested) {
                /* Earlier scan_start failed; Wi-Fi is now ready, so retry the scan */
                s_scan_requested = false;
                wifi_scan_config_t scan_cfg = {
                    .ssid = NULL, .bssid = NULL, .channel = 0,
                    .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE,
                };
                set_state(SF_WIFI_STATE_SCANNING);
                esp_err_t scan_err = esp_wifi_scan_start(&scan_cfg, false);
                if (scan_err != ESP_OK) {
                    ESP_LOGW(TAG, "scan start failed: %s", esp_err_to_name(scan_err));
                    /* Scan could not be started: connect directly (without relying on scan results) */
                    if (s_ssid[0]) {
                        set_state(SF_WIFI_STATE_CONNECTING);
                        esp_wifi_connect();
                    } else {
                        set_state(SF_WIFI_STATE_DISCONNECTED);
                    }
                } else if (s_ssid[0]) {
                    /* Also connect while scanning */
                    esp_wifi_connect();
                }
            } else if (s_ssid[0]) {
                set_state(SF_WIFI_STATE_CONNECTING);
                esp_wifi_connect();
            } else {
                set_state(SF_WIFI_STATE_DISCONNECTED);
            }
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            /* Ignore disconnect events that arrive after Wi-Fi was explicitly
               disabled/stopped: the radio is down and there is nothing to reconnect. */
            if (!s_enabled) break;

            wifi_event_sta_disconnected_t *disc = data;
            ESP_LOGW(TAG, "disconnected, reason=%d", disc->reason);

            /* Clear the IP */
            s_cur_ip[0] = '\0';

            /* Reconnect immediately for any reason (incl. 201 = no AP, 202 = auth fail). */
            set_state(s_ever_connected ? SF_WIFI_STATE_DISCONNECTED
                                       : SF_WIFI_STATE_CONNECTING);
            esp_wifi_connect();
            break;
        }

        case WIFI_EVENT_SCAN_DONE: {
            ESP_LOGI(TAG, "scan done");
            /* Fetch the scan results */
            uint16_t ap_count = 0;
            esp_wifi_scan_get_ap_num(&ap_count);

            wifi_ap_record_t *aps = NULL;
            if (ap_count > 0) {
                aps = calloc(ap_count, sizeof(wifi_ap_record_t));
                if (aps) {
                    esp_wifi_scan_get_ap_records(&ap_count, aps);
                }
            }

            /* Fold results into s_scan_list, keeping only the strongest signal per unique SSID */
            xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
            memset(&s_scan_list, 0, sizeof(s_scan_list));
            int idx = 0;
            for (int i = 0; i < ap_count && idx < SF_WIFI_MAX_SCAN_RESULTS; i++) {
                /* Skip hidden SSIDs */
                if (aps[i].ssid[0] == '\0') continue;

                /* Check whether it already exists */
                bool dup = false;
                for (int j = 0; j < idx; j++) {
                    if (strcmp(s_scan_list.results[j].ssid, (char *)aps[i].ssid) == 0) {
                        /* Keep the one with the stronger signal */
                        if (aps[i].rssi > s_scan_list.results[j].rssi) {
                            s_scan_list.results[j].rssi = aps[i].rssi;
                            s_scan_list.results[j].authmode = aps[i].authmode;
                            s_scan_list.results[j].channel = aps[i].primary;
                        }
                        dup = true;
                        break;
                    }
                }
                if (dup) continue;

                strncpy(s_scan_list.results[idx].ssid, (char *)aps[i].ssid,
                        SF_WIFI_SSID_MAX_LEN - 1);
                s_scan_list.results[idx].ssid[SF_WIFI_SSID_MAX_LEN - 1] = '\0';
                s_scan_list.results[idx].rssi = aps[i].rssi;
                s_scan_list.results[idx].authmode = aps[i].authmode;
                s_scan_list.results[idx].channel = aps[i].primary;
                s_scan_list.results[idx].is_connected =
                    (strcmp(s_scan_list.results[idx].ssid, s_ssid) == 0 &&
                     s_cur_ip[0] != '\0');
                s_scan_list.results[idx].is_saved =
                    (s_ssid[0] &&
                     strcmp(s_scan_list.results[idx].ssid, s_ssid) == 0);
                idx++;
            }
            s_scan_list.count = idx;
            xSemaphoreGive(s_scan_mutex);

            if (aps) free(aps);

            /* Restore the state based on the actual connection, not on s_ssid */
            if (s_state == SF_WIFI_STATE_SCANNING) {
                if (s_cur_ip[0]) {
                    set_state(SF_WIFI_STATE_CONNECTED);
                } else if (s_ssid[0]) {
                    set_state(SF_WIFI_STATE_CONNECTING);
                } else {
                    set_state(SF_WIFI_STATE_DISCONNECTED);
                }
            }

            /* Notify upper layers that the scan is complete */
            if (s_event_group) xEventGroupSetBits(s_event_group, BIT_SCAN_DONE);
            break;
        }

        default:
            break;
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = data;
            snprintf(s_cur_ip, sizeof(s_cur_ip), IPSTR, IP2STR(&event->ip_info.ip));
            ESP_LOGI(TAG, "got IP: %s", s_cur_ip);
            s_ever_connected = true;
            set_state(SF_WIFI_STATE_CONNECTED);
            /* Persist credentials only after a successful connection */
            sf_config_set_wifi_creds_ex(s_ssid, (int)s_target_security,
                                        s_target_pass[0] ? s_target_pass : NULL,
                                        s_target_identity[0] ? s_target_identity : NULL,
                                        s_target_username[0] ? s_target_username : NULL);
            sf_config_save();
        }
    }
}

/* ── Public API ─────────────────────────────────────── */

esp_err_t sf_wifi_init(void)
{
    if (s_inited) return ESP_OK;

    /* Init NVS (Wi-Fi credential storage + internal ESP-IDF use) */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition full/new version, erasing...");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Init the TCP/IP stack */
    ESP_ERROR_CHECK(esp_netif_init());

    /* Create the default event loop (if not already present) */
    esp_err_t ev_err = esp_event_loop_create_default();
    if (ev_err != ESP_OK && ev_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(ev_err));
        return ev_err;
    }

    /* Create the STA netif */
    s_sta_netif = esp_netif_create_default_wifi_sta();

    /* Init Wi-Fi (STA mode) */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Create the event group and the scan-list mutex */
    s_event_group = xEventGroupCreate();
    s_scan_mutex = xSemaphoreCreateMutex();
    if (!s_scan_mutex) return ESP_ERR_NO_MEM;

    /* Register event handlers */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    /* STA mode */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    s_inited = true;

    /* Auto-connect if Wi-Fi is enabled and saved credentials exist */
    if (sf_config_get_wifi_enabled() && sf_config_has_wifi_creds()) {
        sf_wifi_connect_params_t p = {
            .ssid     = sf_config_get_wifi_ssid(),
            .security = (sf_wifi_security_t)sf_config_get_wifi_security(),
            .pass     = sf_config_get_wifi_pass(),
            .identity = sf_config_get_wifi_identity(),
            .username = sf_config_get_wifi_username(),
        };
        ESP_LOGI(TAG, "auto-connecting to '%s'", p.ssid);
        sf_wifi_connect_ex(&p);
        return ESP_OK;
    }

    set_state(SF_WIFI_STATE_DISABLED);
    ESP_LOGI(TAG, "Wi-Fi service initialized (disabled)");
    return ESP_OK;
}

esp_err_t sf_wifi_set_enabled(bool enabled)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    if (enabled && !s_enabled) {
        apply_sta_config();
        ESP_ERROR_CHECK(ensure_started());
        sf_config_set_wifi_enabled(true);
        sf_config_save();
        /* s_ssid (the saved target) is already set; show it as connecting. */
        if (s_ssid[0]) {
            set_state(SF_WIFI_STATE_CONNECTING);
        } else {
            set_state(SF_WIFI_STATE_DISCONNECTED);
        }
        ESP_LOGI(TAG, "Wi-Fi enabled");
    } else if (!enabled && s_enabled) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        s_enabled = false;
        sf_config_set_wifi_enabled(false);
        sf_config_save();
        s_scan_requested = false;
        s_cur_ip[0] = '\0';
        xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
        memset(&s_scan_list, 0, sizeof(s_scan_list));
        xSemaphoreGive(s_scan_mutex);
        set_state(SF_WIFI_STATE_DISABLED);
        ESP_LOGI(TAG, "Wi-Fi disabled");
    }
    return ESP_OK;
}

bool sf_wifi_is_enabled(void)
{
    return s_enabled;
}

sf_wifi_state_t sf_wifi_get_state(void)
{
    return s_state;
}

const char *sf_wifi_get_ssid(void)
{
    return s_ssid;
}

const char *sf_wifi_get_saved_ssid(void)
{
    return s_ssid;
}

const char *sf_wifi_get_ip_str(void)
{
    return s_cur_ip;
}

int8_t sf_wifi_get_rssi(void)
{
    if (s_state != SF_WIFI_STATE_CONNECTED || !s_enabled) return 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}

esp_err_t sf_wifi_connect(const char *ssid, const char *pass)
{
    sf_wifi_connect_params_t p = {
        .ssid     = ssid,
        .security = (pass && pass[0]) ? SF_WIFI_SEC_WPA_WPA2_PSK : SF_WIFI_SEC_OPEN,
        .pass     = pass,
        .identity = NULL,
        .username = NULL,
    };
    return sf_wifi_connect_ex(&p);
}

esp_err_t sf_wifi_connect_ex(const sf_wifi_connect_params_t *params)
{
    if (!s_inited || !params || !params->ssid || !params->ssid[0])
        return ESP_ERR_INVALID_ARG;

    /* Remember the target network (drives reconnect). Persist only on success. */
    store_target(params);
    set_state(SF_WIFI_STATE_CONNECTING);

    bool was_enabled = s_enabled;

    /* Apply station config (SSID/pass/security/EAP) before bringing the radio up. */
    apply_sta_config();

    if (!was_enabled) {
        ESP_ERROR_CHECK(ensure_started());   /* STA_START handler connects to s_ssid */
    } else {
        esp_wifi_connect();                  /* radio already up: connect / switch network */
    }

    ESP_LOGI(TAG, "connecting to '%s'...", params->ssid);
    return ESP_OK;
}

/* Disconnect turns Wi-Fi off entirely: stop the radio so it cannot auto-reconnect. */
esp_err_t sf_wifi_disconnect(void)
{
    return sf_wifi_set_enabled(false);
}

/* Forget drops the saved credentials and turns Wi-Fi off: with no network left
   to connect to, there is nothing to auto-reconnect to. */
esp_err_t sf_wifi_forget(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    s_ever_connected = false;
    sf_config_clear_wifi_creds();
    s_ssid[0] = '\0';
    s_target_pass[0] = '\0';
    s_target_identity[0] = '\0';
    s_target_username[0] = '\0';
    ESP_LOGI(TAG, "forgot saved network");
    sf_config_save();
    return sf_wifi_set_enabled(false);
}

esp_err_t sf_wifi_start_scan(void)
{
    if (!s_inited || !s_enabled) return ESP_ERR_INVALID_STATE;

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    set_state(SF_WIFI_STATE_SCANNING);
    if (s_event_group) xEventGroupClearBits(s_event_group, BIT_SCAN_DONE);

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan start failed: %s, will retry on STA_START", esp_err_to_name(err));
        s_scan_requested = true;
        /* Keep the SCANNING state; it will be retried on STA_START */
    }
    return err;
}

esp_err_t sf_wifi_get_scan_results(sf_wifi_scan_list_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    if (s_scan_mutex) {
        xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
        memcpy(out, &s_scan_list, sizeof(s_scan_list));
        xSemaphoreGive(s_scan_mutex);
    } else {
        memcpy(out, &s_scan_list, sizeof(s_scan_list));
    }
    return ESP_OK;
}
