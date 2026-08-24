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
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

static const char *TAG = "sf_wifi";

/* Bit set when the scan finishes */
#define BIT_SCAN_DONE   (1 << 0)
#define BIT_CONNECT_FAIL (1 << 1)

static sf_wifi_state_t s_state = SF_WIFI_STATE_DISABLED;
static char s_cur_ssid[SF_WIFI_SSID_MAX_LEN] = {0};
static char s_saved_ssid[SF_WIFI_SSID_MAX_LEN] = {0};  /* SSID saved in the config */
static char s_cur_ip[16] = {0};
static bool s_inited = false;
static bool s_enabled = false;
static bool s_scan_requested = false;   /* set to delay a retry when the scan fails before STA_START */

static EventGroupHandle_t s_event_group = NULL;
static sf_wifi_scan_list_t s_scan_list = {0};
static SemaphoreHandle_t s_scan_mutex = NULL;   /* protects s_scan_list (esp_event task ↔ GUI task) */
static esp_netif_t *s_sta_netif = NULL;

/* ── Internal helpers ─────────────────────────────────── */

static void set_state(sf_wifi_state_t new_state)
{
    if (s_state == new_state) return;
    s_state = new_state;
    ESP_LOGI(TAG, "state -> %d", (int)new_state);
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
                    if (s_saved_ssid[0]) {
                        set_state(SF_WIFI_STATE_CONNECTING);
                        esp_wifi_connect();
                    } else {
                        set_state(SF_WIFI_STATE_DISCONNECTED);
                    }
                } else if (s_saved_ssid[0]) {
                    /* Also connect while scanning */
                    esp_wifi_connect();
                }
            } else if (s_saved_ssid[0]) {
                set_state(SF_WIFI_STATE_CONNECTING);
                esp_wifi_connect();
            } else {
                set_state(SF_WIFI_STATE_DISCONNECTED);
            }
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *disc = data;
            ESP_LOGW(TAG, "disconnected, reason=%d", disc->reason);

            /* Clear the IP */
            s_cur_ip[0] = '\0';

            /* Reason 201 = no AP found, 202 = auth failed — do not retry */
            if (disc->reason == 201 || disc->reason == 202) {
                ESP_LOGW(TAG, "connect failed (reason %d), not retrying", disc->reason);
                set_state(SF_WIFI_STATE_ERROR);
                if (s_event_group) xEventGroupSetBits(s_event_group, BIT_CONNECT_FAIL);
            } else {
                /* Other reasons: reconnect automatically */
                set_state(SF_WIFI_STATE_CONNECTING);
                esp_wifi_connect();
            }
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
                    (strcmp(s_scan_list.results[idx].ssid, s_cur_ssid) == 0 &&
                     s_cur_ip[0] != '\0');
                s_scan_list.results[idx].is_saved =
                    (s_saved_ssid[0] &&
                     strcmp(s_scan_list.results[idx].ssid, s_saved_ssid) == 0);
                idx++;
            }
            s_scan_list.count = idx;
            xSemaphoreGive(s_scan_mutex);

            if (aps) free(aps);

            /* Restore the state based on the actual connection, not on s_cur_ssid */
            if (s_state == SF_WIFI_STATE_SCANNING) {
                if (s_cur_ip[0]) {
                    set_state(SF_WIFI_STATE_CONNECTED);
                } else if (s_cur_ssid[0]) {
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
            set_state(SF_WIFI_STATE_CONNECTED);
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

    /* Check whether the config has saved credentials */
    bool auto_enable = sf_config_get_wifi_enabled();
    if (auto_enable && sf_config_has_wifi_creds()) {
        const char *ssid = sf_config_get_wifi_ssid();
        const char *pass = sf_config_get_wifi_pass();
        strncpy(s_cur_ssid, ssid, sizeof(s_cur_ssid) - 1);
        strncpy(s_saved_ssid, ssid, sizeof(s_saved_ssid) - 1);
        ESP_LOGI(TAG, "auto-connecting to '%s'", ssid);

        wifi_config_t wifi_cfg = {0};
        strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
        if (pass[0]) {
            strncpy((char *)wifi_cfg.sta.password, pass,
                    sizeof(wifi_cfg.sta.password) - 1);
        }
        esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);

        ESP_ERROR_CHECK(esp_wifi_start());
        s_enabled = true;
        s_inited = true;
        return ESP_OK;
    }

    set_state(SF_WIFI_STATE_DISABLED);
    s_inited = true;
    ESP_LOGI(TAG, "Wi-Fi service initialized (disabled)");
    return ESP_OK;
}

esp_err_t sf_wifi_set_enabled(bool enabled)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    if (enabled && !s_enabled) {
        ESP_ERROR_CHECK(esp_wifi_start());
        s_enabled = true;
        sf_config_set_wifi_enabled(true);
        sf_config_save();
        /* Restore s_cur_ssid so the UI can show the saved network being connected */
        if (s_saved_ssid[0]) {
            strncpy(s_cur_ssid, s_saved_ssid, sizeof(s_cur_ssid) - 1);
            s_cur_ssid[sizeof(s_cur_ssid) - 1] = '\0';
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
        s_cur_ssid[0] = '\0';
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
    return s_cur_ssid;
}

const char *sf_wifi_get_saved_ssid(void)
{
    return s_saved_ssid;
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
    if (!s_inited || !ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;

    if (!s_enabled) {
        ESP_ERROR_CHECK(esp_wifi_start());
        s_enabled = true;
    }

    /* If currently connected to another network, disconnect first */
    if (s_state == SF_WIFI_STATE_CONNECTED || s_state == SF_WIFI_STATE_CONNECTING) {
        esp_wifi_disconnect();
    }

    /* Save the credentials to the config */
    sf_config_set_wifi_creds(ssid, pass);
    sf_config_save();
    strncpy(s_cur_ssid, ssid, sizeof(s_cur_ssid) - 1);
    s_cur_ssid[sizeof(s_cur_ssid) - 1] = '\0';
    strncpy(s_saved_ssid, ssid, sizeof(s_saved_ssid) - 1);
    s_saved_ssid[sizeof(s_saved_ssid) - 1] = '\0';

    /* Configure and connect */
    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    if (pass && pass[0]) {
        strncpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password) - 1);
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));

    set_state(SF_WIFI_STATE_CONNECTING);
    /* Clear any bit leftover from a previous attempt */
    if (s_event_group) xEventGroupClearBits(s_event_group, BIT_CONNECT_FAIL);

    esp_wifi_connect();

    ESP_LOGI(TAG, "connecting to '%s'...", ssid);
    return ESP_OK;
}

esp_err_t sf_wifi_disconnect(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    esp_wifi_disconnect();
    s_cur_ip[0] = '\0';
    set_state(SF_WIFI_STATE_DISCONNECTED);
    return ESP_OK;
}

esp_err_t sf_wifi_forget(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    esp_wifi_disconnect();
    sf_config_clear_wifi_creds();
    sf_config_save();
    s_cur_ssid[0] = '\0';
    s_saved_ssid[0] = '\0';
    s_cur_ip[0] = '\0';
    set_state(SF_WIFI_STATE_DISCONNECTED);
    ESP_LOGI(TAG, "forgot saved network");
    return ESP_OK;
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
