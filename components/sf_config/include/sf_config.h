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
 *       "ssid": "...",
 *       "pass": "..."
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

/* ── Wi-Fi ── */

bool sf_config_get_wifi_enabled(void);
void sf_config_set_wifi_enabled(bool en);

/** Return the saved SSID (empty string if none saved) */
const char *sf_config_get_wifi_ssid(void);
const char *sf_config_get_wifi_pass(void);

/** Wi-Fi security type (sf_wifi_security_t). 0 = Open (legacy config default). */
int  sf_config_get_wifi_security(void);
/** EAP identity / username for Enterprise; empty string if none saved */
const char *sf_config_get_wifi_identity(void);
const char *sf_config_get_wifi_username(void);

/** Save full Wi-Fi credentials (incl. security type and Enterprise EAP fields), mark enabled=true */
void sf_config_set_wifi_creds_ex(const char *ssid, int security,
                                 const char *pass, const char *identity,
                                 const char *username);

/** Save Wi-Fi credentials (ssid/pass) and mark enabled=true. Security defaults to Open. */
void sf_config_set_wifi_creds(const char *ssid, const char *pass);

/** Clear saved Wi-Fi credentials */
void sf_config_clear_wifi_creds(void);

/** Whether saved Wi-Fi credentials exist */
bool sf_config_has_wifi_creds(void);

/* ── Ethernet (reserved) ── */

bool sf_config_get_eth_enabled(void);
void sf_config_set_eth_enabled(bool en);

#ifdef __cplusplus
}
#endif

#endif /* SF_CONFIG_H */
