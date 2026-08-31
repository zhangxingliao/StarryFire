/**
 * sf_config.c - StarryFire configuration storage implementation
 *
 * Persists device configuration using SPIFFS + cJSON.
 */
#include "sf_config.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_partition.h"
#include "cJSON.h"

static const char *TAG = "sf_config";

#define CONFIG_PATH  "/spiffs/config.json"
#define CONFIG_PART   "spiffs"

/* ── In-memory config cache ──────────────────────────────── */

typedef struct {
    /* THEME */
    int  theme_id;

    /* LOCALE */
    int  brightness;
    char timezone[64];
    int  screen_timeout;

    /* NETWORKS > wifi */
    bool wifi_enabled;
    char wifi_ssid[33];
    char wifi_pass[65];
    int  wifi_security;     /* sf_wifi_security_t; 0 = Open for legacy configs */
    char wifi_identity[65];
    char wifi_username[65];
    bool wifi_has_creds;

    /* NETWORKS > ethernet */
    bool eth_enabled;
} config_data_t;

static config_data_t s_cfg = {
    .theme_id       = 0,
    .brightness     = 100,
    .timezone       = "CST-8",
    .screen_timeout = 30,
    .wifi_enabled   = false,
    .wifi_ssid      = {0},
    .wifi_pass      = {0},
    .wifi_security  = 0,
    .wifi_identity  = {0},
    .wifi_username  = {0},
    .wifi_has_creds = false,
    .eth_enabled    = false,
};

static bool s_inited = false;
static bool s_spiffs_ok = false;   /* whether SPIFFS mounted successfully */

/* ── SPIFFS ────────────────────────────────────────── */

static esp_err_t init_spiffs(void)
{
    /* First check that the partition exists */
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, CONFIG_PART);
    if (!part) {
        ESP_LOGE(TAG, "SPIFFS partition '%s' not found in partition table!", CONFIG_PART);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "SPIFFS partition: offset=0x%08lx size=%luKB",
             (unsigned long)part->address, (unsigned long)(part->size / 1024));

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = CONFIG_PART,
        .max_files = 5,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount/format SPIFFS: %s", esp_err_to_name(ret));
        /* Try explicitly erasing and reformatting */
        ESP_LOGW(TAG, "Attempting erase + reformat...");
        if (esp_spiffs_format(CONFIG_PART) == ESP_OK) {
            esp_vfs_spiffs_unregister(CONFIG_PART);
            ret = esp_vfs_spiffs_register(&conf);
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPIFFS unavailable, config will not persist: %s",
                     esp_err_to_name(ret));
            return ret;
        }
    }

    size_t total = 0, used = 0;
    if (esp_spiffs_info(CONFIG_PART, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted: %zu KB total, %zu KB used", total / 1024, used / 1024);
    }
    s_spiffs_ok = true;
    return ESP_OK;
}

/* ── JSON loading ─────────────────────────────────────── */

static void load_from_json(cJSON *root)
{
    if (!root) return;

    /* THEME */
    cJSON *theme = cJSON_GetObjectItem(root, "THEME");
    if (theme) {
        cJSON *id = cJSON_GetObjectItem(theme, "theme_id");
        if (id && cJSON_IsNumber(id))
            s_cfg.theme_id = id->valueint;
    }

    /* LOCALE */
    cJSON *locale = cJSON_GetObjectItem(root, "LOCALE");
    if (locale) {
        cJSON *bri = cJSON_GetObjectItem(locale, "brightness");
        if (bri && cJSON_IsNumber(bri))
            s_cfg.brightness = bri->valueint;

        cJSON *tz = cJSON_GetObjectItem(locale, "timezone");
        if (tz && cJSON_IsString(tz))
            strncpy(s_cfg.timezone, tz->valuestring, sizeof(s_cfg.timezone) - 1);

        cJSON *sto = cJSON_GetObjectItem(locale, "screen_timeout");
        if (sto && cJSON_IsNumber(sto))
            s_cfg.screen_timeout = sto->valueint;
    }

    /* NETWORKS */
    cJSON *networks = cJSON_GetObjectItem(root, "NETWORKS");
    if (networks) {
        /* wifi */
        cJSON *wifi = cJSON_GetObjectItem(networks, "wifi");
        if (wifi) {
            cJSON *en = cJSON_GetObjectItem(wifi, "enabled");
            if (en && cJSON_IsBool(en))
                s_cfg.wifi_enabled = cJSON_IsTrue(en);

            cJSON *ssid = cJSON_GetObjectItem(wifi, "ssid");
            if (ssid && cJSON_IsString(ssid)) {
                strncpy(s_cfg.wifi_ssid, ssid->valuestring, sizeof(s_cfg.wifi_ssid) - 1);
                s_cfg.wifi_has_creds = true;
            }

            cJSON *pass = cJSON_GetObjectItem(wifi, "pass");
            if (pass && cJSON_IsString(pass))
                strncpy(s_cfg.wifi_pass, pass->valuestring, sizeof(s_cfg.wifi_pass) - 1);

            cJSON *sec = cJSON_GetObjectItem(wifi, "security");
            if (sec && cJSON_IsNumber(sec))
                s_cfg.wifi_security = sec->valueint;

            cJSON *idn = cJSON_GetObjectItem(wifi, "identity");
            if (idn && cJSON_IsString(idn))
                strncpy(s_cfg.wifi_identity, idn->valuestring, sizeof(s_cfg.wifi_identity) - 1);

            cJSON *usr = cJSON_GetObjectItem(wifi, "username");
            if (usr && cJSON_IsString(usr))
                strncpy(s_cfg.wifi_username, usr->valuestring, sizeof(s_cfg.wifi_username) - 1);
        }

        /* ethernet */
        cJSON *eth = cJSON_GetObjectItem(networks, "ethernet");
        if (eth) {
            cJSON *en = cJSON_GetObjectItem(eth, "enabled");
            if (en && cJSON_IsBool(en))
                s_cfg.eth_enabled = cJSON_IsTrue(en);
        }
    }
}

static esp_err_t load_config(void)
{
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        ESP_LOGI(TAG, "No config file found, using defaults");
        return ESP_OK;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > 8192) {
        ESP_LOGW(TAG, "Config file size %ld invalid, using defaults", sz);
        fclose(f);
        return ESP_OK;
    }

    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }

    size_t rd = fread(buf, 1, sz, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (!root) {
        ESP_LOGW(TAG, "Failed to parse config JSON, using defaults");
        return ESP_OK;
    }

    load_from_json(root);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Config loaded: brightness=%d tz=%s wifi=%s",
             s_cfg.brightness, s_cfg.timezone,
             s_cfg.wifi_has_creds ? s_cfg.wifi_ssid : "(none)");
    return ESP_OK;
}

/* ── JSON saving ─────────────────────────────────────── */

esp_err_t sf_config_save(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (!s_spiffs_ok) {
        ESP_LOGW(TAG, "SPIFFS not available, config not saved");
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_CreateObject();

    /* THEME */
    cJSON *theme = cJSON_CreateObject();
    cJSON_AddNumberToObject(theme, "theme_id", s_cfg.theme_id);
    cJSON_AddItemToObject(root, "THEME", theme);

    /* LOCALE */
    cJSON *locale = cJSON_CreateObject();
    cJSON_AddNumberToObject(locale, "brightness", s_cfg.brightness);
    cJSON_AddStringToObject(locale, "timezone", s_cfg.timezone);
    cJSON_AddNumberToObject(locale, "screen_timeout", s_cfg.screen_timeout);
    cJSON_AddItemToObject(root, "LOCALE", locale);

    /* NETWORKS */
    cJSON *networks = cJSON_CreateObject();

    cJSON *wifi = cJSON_CreateObject();
    cJSON_AddBoolToObject(wifi, "enabled", s_cfg.wifi_enabled);
    if (s_cfg.wifi_has_creds) {
        cJSON_AddStringToObject(wifi, "ssid", s_cfg.wifi_ssid);
        cJSON_AddStringToObject(wifi, "pass", s_cfg.wifi_pass);
        cJSON_AddNumberToObject(wifi, "security", s_cfg.wifi_security);
        if (s_cfg.wifi_identity[0])
            cJSON_AddStringToObject(wifi, "identity", s_cfg.wifi_identity);
        if (s_cfg.wifi_username[0])
            cJSON_AddStringToObject(wifi, "username", s_cfg.wifi_username);
    }
    cJSON_AddItemToObject(networks, "wifi", wifi);

    cJSON *eth = cJSON_CreateObject();
    cJSON_AddBoolToObject(eth, "enabled", s_cfg.eth_enabled);
    cJSON_AddItemToObject(networks, "ethernet", eth);

    cJSON_AddItemToObject(root, "NETWORKS", networks);

    char *str = cJSON_Print(root);
    cJSON_Delete(root);

    if (!str) return ESP_ERR_NO_MEM;

    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", CONFIG_PATH);
        free(str);
        return ESP_FAIL;
    }

    fputs(str, f);
    fclose(f);
    free(str);

    ESP_LOGI(TAG, "Config saved");
    return ESP_OK;
}

/* ── Initialization ────────────────────────────────────────── */

esp_err_t sf_config_init(void)
{
    if (s_inited) return ESP_OK;

    esp_err_t ret = init_spiffs();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS init failed, using in-memory defaults (config will NOT persist)");
        s_inited = true;  /* Mark as initialized; save() will safely skip */
        return ret;
    }

    load_config();

    s_inited = true;
    ESP_LOGI(TAG, "Config service ready");
    return ESP_OK;
}

/* ── LOCALE getters/setters ────────────────────────── */

int sf_config_get_brightness(void) { return s_cfg.brightness; }

void sf_config_set_brightness(int val)
{
    if (val < 1) val = 1;
    if (val > 100) val = 100;
    s_cfg.brightness = val;
}

const char *sf_config_get_timezone(void) { return s_cfg.timezone; }

void sf_config_set_timezone(const char *tz)
{
    if (!tz) return;
    strncpy(s_cfg.timezone, tz, sizeof(s_cfg.timezone) - 1);
    s_cfg.timezone[sizeof(s_cfg.timezone) - 1] = '\0';
}

int sf_config_get_screen_timeout(void) { return s_cfg.screen_timeout; }

void sf_config_set_screen_timeout(int sec)
{
    if (sec < 0) sec = 0;
    s_cfg.screen_timeout = sec;
}

/* ── NETWORKS > Wi-Fi getters/setters ──────────────── */

bool sf_config_get_wifi_enabled(void) { return s_cfg.wifi_enabled; }

void sf_config_set_wifi_enabled(bool en) { s_cfg.wifi_enabled = en; }

const char *sf_config_get_wifi_ssid(void) { return s_cfg.wifi_ssid; }
const char *sf_config_get_wifi_pass(void) { return s_cfg.wifi_pass; }

int sf_config_get_wifi_security(void) { return s_cfg.wifi_security; }
const char *sf_config_get_wifi_identity(void) { return s_cfg.wifi_identity; }
const char *sf_config_get_wifi_username(void) { return s_cfg.wifi_username; }

void sf_config_set_wifi_creds_ex(const char *ssid, int security,
                                 const char *pass, const char *identity,
                                 const char *username)
{
    if (!ssid) return;
    strncpy(s_cfg.wifi_ssid, ssid, sizeof(s_cfg.wifi_ssid) - 1);
    s_cfg.wifi_ssid[sizeof(s_cfg.wifi_ssid) - 1] = '\0';

    if (pass) {
        strncpy(s_cfg.wifi_pass, pass, sizeof(s_cfg.wifi_pass) - 1);
        s_cfg.wifi_pass[sizeof(s_cfg.wifi_pass) - 1] = '\0';
    } else {
        s_cfg.wifi_pass[0] = '\0';
    }
    s_cfg.wifi_security = security;

    if (identity) {
        strncpy(s_cfg.wifi_identity, identity, sizeof(s_cfg.wifi_identity) - 1);
        s_cfg.wifi_identity[sizeof(s_cfg.wifi_identity) - 1] = '\0';
    } else {
        s_cfg.wifi_identity[0] = '\0';
    }
    if (username) {
        strncpy(s_cfg.wifi_username, username, sizeof(s_cfg.wifi_username) - 1);
        s_cfg.wifi_username[sizeof(s_cfg.wifi_username) - 1] = '\0';
    } else {
        s_cfg.wifi_username[0] = '\0';
    }
    s_cfg.wifi_has_creds = true;
    s_cfg.wifi_enabled = true;
}

void sf_config_set_wifi_creds(const char *ssid, const char *pass)
{
    sf_config_set_wifi_creds_ex(ssid, 0, pass, NULL, NULL);
}

void sf_config_clear_wifi_creds(void)
{
    s_cfg.wifi_ssid[0] = '\0';
    s_cfg.wifi_pass[0] = '\0';
    s_cfg.wifi_identity[0] = '\0';
    s_cfg.wifi_username[0] = '\0';
    s_cfg.wifi_security = 0;
    s_cfg.wifi_has_creds = false;
}

bool sf_config_has_wifi_creds(void) { return s_cfg.wifi_has_creds; }

/* ── NETWORKS > Ethernet getters/setters ───────────── */

bool sf_config_get_eth_enabled(void) { return s_cfg.eth_enabled; }
void sf_config_set_eth_enabled(bool en) { s_cfg.eth_enabled = en; }

/* ── THEME getters/setters ─────────────────────────── */

int sf_config_get_theme(void) { return s_cfg.theme_id; }
void sf_config_set_theme(int id) { s_cfg.theme_id = id; }
