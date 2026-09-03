/**
 * sf_config.h - StarryFire configuration storage service
 *
* Persists device configuration as JSON using the SPIFFS filesystem.
*
* JSON structure:
 * {
 *   "LOCALE": {
 *     "brightness": 80,
 *     "timezone": "CST-8",
 *     "screen_timeout": 30
 *   },
 *   "NETWORKS": {
 *     "wifi": {
 *       "enabled": true,
 *       "profiles": [
 *         { "ssid": "...", "pass": "...", "security": 4,
 *           "identity": "...", "username": "..." }
 *       ]
 *     },
 *     "ethernet": {
 *       "enabled": false
 *     }
 *   }
 * }
 */
#ifndef SF_CONFIG_H
#define SF_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Initialization ────────────────────────────────────────── */

/**
 * Initializes SPIFFS and loads the config.
 * Creates default config if no config file exists.
 * Must be called before sf_wifi_init().
 */
esp_err_t sf_config_init(void);

/**
 * Saves the current config to SPIFFS.
 */
esp_err_t sf_config_save(void);

/* ── Theme config ──────────────────────────────────────── */

/** Theme ID (0 = Dark Nebula, 1 = Aurora, etc.), default 0 */
int  sf_config_get_theme(void);
void sf_config_set_theme(int id);

/* ── LOCALE config ──────────────────────────────────── */

/** Brightness (0-100), default 100 */
int  sf_config_get_brightness(void);
void sf_config_set_brightness(int val);

/** Timezone string (POSIX TZ format, e.g. "CST-8"), default "CST-8" */
const char *sf_config_get_timezone(void);
void        sf_config_set_timezone(const char *tz);

/** Screen timeout (seconds, 0=never), default 30 */
int  sf_config_get_screen_timeout(void);
void sf_config_set_screen_timeout(int sec);

/* ── NETWORKS config ────────────────────────────────── */

/* ── Wi-Fi (multiple saved profiles) ── */

/** Maximum number of saved Wi-Fi profiles */
#define SF_WIFI_MAX_PROFILES 5

/** A saved Wi-Fi network profile (credentials + security). */
typedef struct {
    char ssid[33];
    char pass[65];
    int  security;     /* sf_wifi_security_t; 0 = Open (legacy/default) */
    char identity[65]; /* EAP identity (Enterprise only) */
    char username[65]; /* EAP username (Enterprise only) */
} wifi_profile_t;

bool sf_config_get_wifi_enabled(void);
void sf_config_set_wifi_enabled(bool en);

/** Number of saved Wi-Fi profiles */
int sf_config_get_wifi_profile_count(void);

/** Get a saved profile by index (0..count-1). Returns NULL if out of range. Read-only. */
const wifi_profile_t *sf_config_get_wifi_profile(int idx);

/** Whether a saved profile with the given SSID exists */
bool sf_config_has_wifi_profile(const char *ssid);

/** Add or update a saved profile (matched by SSID). Enforces SF_WIFI_MAX_PROFILES
 *  (evicts the oldest entry when full and the SSID is new). Returns true if stored. */
bool sf_config_add_wifi_profile(const char *ssid, int security,
                                const char *pass, const char *identity,
                                const char *username);

/** Remove the saved profile with the given SSID. Returns true if removed. */
bool sf_config_remove_wifi_profile(const char *ssid);

/* ── Ethernet (reserved) ── */

bool sf_config_get_eth_enabled(void);
void sf_config_set_eth_enabled(bool en);

#ifdef __cplusplus
}
#endif

#endif /* SF_CONFIG_H */
