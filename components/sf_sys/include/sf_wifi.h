/**
 * sf_wifi.h — StarryFire Wi-Fi service
 *
 * Wraps ESP-IDF Wi-Fi STA mode and provides:
 * - JSON config persistence (via sf_config, with auto-reconnect)
 * - Asynchronous scanning
 * - Connect / disconnect
 * - Connection-state changes published through the EventBus
 *
 * All callbacks run in the ESP-IDF event loop task, so upper layers
 * must call lvgl_port_lock() when updating LVGL.
 */
#ifndef SF_WIFI_H
#define SF_WIFI_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Wi-Fi states ───────────────────────────────────── */

typedef enum {
    SF_WIFI_STATE_DISABLED = 0,
    SF_WIFI_STATE_DISCONNECTED,
    SF_WIFI_STATE_CONNECTING,
    SF_WIFI_STATE_CONNECTED,
    SF_WIFI_STATE_SCANNING,
    SF_WIFI_STATE_ERROR,
} sf_wifi_state_t;

/* ── Scan results ─────────────────────────────────────── */

#define SF_WIFI_MAX_SCAN_RESULTS  15
#define SF_WIFI_SSID_MAX_LEN      33
#define SF_WIFI_PASS_MAX_LEN      64

typedef struct {
    char     ssid[SF_WIFI_SSID_MAX_LEN];
    int8_t   rssi;           /* signal strength in dBm */
    uint8_t  authmode;       /* 0 = Open, anything else = secured */
    uint8_t  channel;
    bool     is_connected;   /* whether this network is currently connected */
    bool     is_saved;       /* whether this is a saved network (has credentials in NVS) */
} sf_wifi_scan_result_t;

typedef struct {
    sf_wifi_scan_result_t results[SF_WIFI_MAX_SCAN_RESULTS];
    int count;
} sf_wifi_scan_list_t;

/* ── API ──────────────────────────────────────────── */

/**
 * Initialize the Wi-Fi service (STA mode).
 * If saved credentials exist in NVS, it tries to connect automatically.
 */
esp_err_t sf_wifi_init(void);

/**
 * Enable / disable Wi-Fi.
 * Disabling disconnects and stops the Wi-Fi radio.
 */
esp_err_t sf_wifi_set_enabled(bool enabled);
bool      sf_wifi_is_enabled(void);

/** Get the current state */
sf_wifi_state_t sf_wifi_get_state(void);

/** Get the SSID of the current connection (empty string if not connected) */
const char *sf_wifi_get_ssid(void);

/** Get the SSID saved in NVS (empty string if none is saved) */
const char *sf_wifi_get_saved_ssid(void);

/** Get the current IP address as a string (empty string if not connected) */
const char *sf_wifi_get_ip_str(void);

/** Get the RSSI of the current connection (0 if not connected) */
int8_t sf_wifi_get_rssi(void);

/**
 * Connect to the specified network.
 * On success the credentials are saved to NVS and reused for auto-reconnect on boot.
 * @param ssid  SSID
 * @param pass  password (empty string or NULL for an open network)
 */
esp_err_t sf_wifi_connect(const char *ssid, const char *pass);

/** Disconnect from the current connection (keeps NVS credentials) */
esp_err_t sf_wifi_disconnect(void);

/** Forget the saved network (removes NVS credentials and disconnects) */
esp_err_t sf_wifi_forget(void);

/**
 * Start an asynchronous scan.
 * Once the scan completes, the results can be fetched with sf_wifi_get_scan_results().
 * Listen for the WIFI_EVENT_SCAN_DONE event to be notified of completion.
 */
esp_err_t sf_wifi_start_scan(void);

/**
 * Copy the most recent scan results into `out` (snapshot).
 * Safe to call from any task: a scan completing concurrently on the event
 * task will not tear the copy.
 */
esp_err_t sf_wifi_get_scan_results(sf_wifi_scan_list_t *out);

#ifdef __cplusplus
}
#endif

#endif /* SF_WIFI_H */
