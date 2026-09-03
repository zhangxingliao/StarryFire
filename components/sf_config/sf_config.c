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
    wifi_profile_t wifi_profiles[SF_WIFI_MAX_PROFILES];
    int  wifi_profile_count;

    /* NETWORKS > ethernet */
    bool eth_enabled;
} config_data_t;

static config_data_t s_cfg = {
    .theme_id       = 0,
    .brightness     = 100,
    .timezone       = "CST-8",
    .screen_timeout = 30,
    .wifi_enabled   = false,
    .wifi_profile_count = 0,
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

            /* New format: an array of profiles */
            cJSON *profiles = cJSON_GetObjectItem(wifi, "profiles");
            if (cJSON_IsArray(profiles)) {
                int n = cJSON_GetArraySize(profiles);
                for (int i = 0; i < n && s_cfg.wifi_profile_count < SF_WIFI_MAX_PROFILES; i++) {
                    cJSON *p = cJSON_GetArrayItem(profiles, i);
                    if (!p) continue;
                    cJSON *ssid = cJSON_GetObjectItem(p, "ssid");
                    if (!ssid || !cJSON_IsString(ssid) || !ssid->valuestring[0]) continue;
                    wifi_profile_t *dst = &s_cfg.wifi_profiles[s_cfg.wifi_profile_count];
                    strncpy(dst->ssid, ssid->valuestring, sizeof(dst->ssid) - 1);
                    cJSON *pass = cJSON_GetObjectItem(p, "pass");
                    if (pass && cJSON_IsString(pass))
                        strncpy(dst->pass, pass->valuestring, sizeof(dst->pass) - 1);
                    cJSON *sec = cJSON_GetObjectItem(p, "security");
                    if (sec && cJSON_IsNumber(sec))
                        dst->security = sec->valueint;
                    cJSON *idn = cJSON_GetObjectItem(p, "identity");
                    if (idn && cJSON_IsString(idn))
                        strncpy(dst->identity, idn->valuestring, sizeof(dst->identity) - 1);
                    cJSON *usr = cJSON_GetObjectItem(p, "username");
                    if (usr && cJSON_IsString(usr))
                        strncpy(dst->username, usr->valuestring, sizeof(dst->username) - 1);
                    s_cfg.wifi_profile_count++;
                }
            } else {
                /* Legacy format: a single ssid/pass credential */
                cJSON *ssid = cJSON_GetObjectItem(wifi, "ssid");
                if (ssid && cJSON_IsString(ssid) && ssid->valuestring[0]) {
                    wifi_profile_t *dst = &s_cfg.wifi_profiles[0];
                    strncpy(dst->ssid, ssid->valuestring, sizeof(dst->ssid) - 1);
                    cJSON *pass = cJSON_GetObjectItem(wifi, "pass");
                    if (pass && cJSON_IsString(pass))
                        strncpy(dst->pass, pass->valuestring, sizeof(dst->pass) - 1);
                    cJSON *sec = cJSON_GetObjectItem(wifi, "security");
                    if (sec && cJSON_IsNumber(sec))
                        dst->security = sec->valueint;
                    cJSON *idn = cJSON_GetObjectItem(wifi, "identity");
                    if (idn && cJSON_IsString(idn))
                        strncpy(dst->identity, idn->valuestring, sizeof(dst->identity) - 1);
                    cJSON *usr = cJSON_GetObjectItem(wifi, "username");
                    if (usr && cJSON_IsString(usr))
                        strncpy(dst->username, usr->valuestring, sizeof(dst->username) - 1);
                    s_cfg.wifi_profile_count = 1;
                }
            }
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

    ESP_LOGI(TAG, "Config loaded: brightness=%d tz=%s wifi_profiles=%d",
             s_cfg.brightness, s_cfg.timezone, s_cfg.wifi_profile_count);
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
    cJSON *profiles = cJSON_CreateArray();
    for (int i = 0; i < s_cfg.wifi_profile_count; i++) {
        wifi_profile_t *p = &s_cfg.wifi_profiles[i];
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "ssid", p->ssid);
        cJSON_AddStringToObject(obj, "pass", p->pass);
        cJSON_AddNumberToObject(obj, "security", p->security);
        if (p->identity[0])
            cJSON_AddStringToObject(obj, "identity", p->identity);
        if (p->username[0])
            cJSON_AddStringToObject(obj, "username", p->username);
        cJSON_AddItemToArray(profiles, obj);
    }
    cJSON_AddItemToObject(wifi, "profiles", profiles);
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

int sf_config_get_wifi_profile_count(void) { return s_cfg.wifi_profile_count; }

const wifi_profile_t *sf_config_get_wifi_profile(int idx)
{
    if (idx < 0 || idx >= s_cfg.wifi_profile_count) return NULL;
    return &s_cfg.wifi_profiles[idx];
}

bool sf_config_has_wifi_profile(const char *ssid)
{
    if (!ssid) return false;
    for (int i = 0; i < s_cfg.wifi_profile_count; i++) {
        if (strcmp(s_cfg.wifi_profiles[i].ssid, ssid) == 0)
            return true;
    }
    return false;
}

bool sf_config_get_wifi_profile_by_ssid(const char *ssid, wifi_profile_t *out)
{
    if (!ssid || !out) return false;
    for (int i = 0; i < s_cfg.wifi_profile_count; i++) {
        if (strcmp(s_cfg.wifi_profiles[i].ssid, ssid) == 0) {
            *out = s_cfg.wifi_profiles[i];
            return true;
        }
    }
    return false;
}

/* Copy a profile's fields into the destination, bounding all strings. */
static void copy_profile(wifi_profile_t *dst, const char *ssid, int security,
                         const char *pass, const char *identity, const char *username)
{
    strncpy(dst->ssid, ssid, sizeof(dst->ssid) - 1);
    dst->ssid[sizeof(dst->ssid) - 1] = '\0';
    strncpy(dst->pass, pass ? pass : "", sizeof(dst->pass) - 1);
    dst->pass[sizeof(dst->pass) - 1] = '\0';
    dst->security = security;
    strncpy(dst->identity, identity ? identity : "", sizeof(dst->identity) - 1);
    dst->identity[sizeof(dst->identity) - 1] = '\0';
    strncpy(dst->username, username ? username : "", sizeof(dst->username) - 1);
    dst->username[sizeof(dst->username) - 1] = '\0';
}

bool sf_config_add_wifi_profile(const char *ssid, int security,
                                const char *pass, const char *identity,
                                const char *username)
{
    if (!ssid || !ssid[0]) return false;

    /* Update in place if the SSID already exists */
    for (int i = 0; i < s_cfg.wifi_profile_count; i++) {
        if (strcmp(s_cfg.wifi_profiles[i].ssid, ssid) == 0) {
            copy_profile(&s_cfg.wifi_profiles[i], ssid, security, pass, identity, username);
            return true;
        }
    }

    /* New SSID: append, evicting the oldest (index 0) when full */
    if (s_cfg.wifi_profile_count >= SF_WIFI_MAX_PROFILES) {
        for (int i = 1; i < SF_WIFI_MAX_PROFILES; i++)
            s_cfg.wifi_profiles[i - 1] = s_cfg.wifi_profiles[i];
        s_cfg.wifi_profile_count = SF_WIFI_MAX_PROFILES - 1;
    }
    copy_profile(&s_cfg.wifi_profiles[s_cfg.wifi_profile_count], ssid, security, pass, identity, username);
    s_cfg.wifi_profile_count++;
    return true;
}

bool sf_config_remove_wifi_profile(const char *ssid)
{
    if (!ssid) return false;
    for (int i = 0; i < s_cfg.wifi_profile_count; i++) {
        if (strcmp(s_cfg.wifi_profiles[i].ssid, ssid) == 0) {
            for (int j = i; j < s_cfg.wifi_profile_count - 1; j++)
                s_cfg.wifi_profiles[j] = s_cfg.wifi_profiles[j + 1];
            s_cfg.wifi_profile_count--;
            s_cfg.wifi_profiles[s_cfg.wifi_profile_count].ssid[0] = '\0';
            return true;
        }
    }
    return false;
}

/* ── NETWORKS > Ethernet getters/setters ───────────── */

bool sf_config_get_eth_enabled(void) { return s_cfg.eth_enabled; }
void sf_config_set_eth_enabled(bool en) { s_cfg.eth_enabled = en; }

/* ── THEME getters/setters ─────────────────────────── */

int sf_config_get_theme(void) { return s_cfg.theme_id; }
void sf_config_set_theme(int id) { s_cfg.theme_id = id; }
