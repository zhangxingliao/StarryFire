/**
 * sf_ntp.h — StarryFire NTP time service
 *
 * Implements network time synchronization based on ESP-IDF esp_netif_sntp.
 * Once the device is online, this module calibrates the system clock
 * (POSIX time), so upper layers (e.g. the status bar) read the real
 * NTP world time, and display it localized using the local timezone
 * (TZ) set in Settings.
 */

#ifndef SF_NTP_H
#define SF_NTP_H

#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the NTP service:
 * - Apply the timezone from Settings to the process TZ environment and call tzset()
 * - Start SNTP (polling mode) to sync time periodically
 * Should be called once Wi-Fi has network access (after sf_wifi_init).
 */
esp_err_t sf_ntp_init(void);

/**
 * Re-apply the local timezone (call when the timezone changes in Settings); takes effect immediately.
 * @param tz POSIX TZ string, e.g. "CST-8"
 */
void sf_ntp_set_tz(const char *tz);

/**
 * Trigger a time sync immediately (optional; SNTP syncs periodically on its own).
 */
void sf_ntp_sync_now(void);

/**
 * Get the local time string "HH:MM" under TZ.
 * @param buf     output buffer, at least 6 bytes
 * @param buf_len buffer length
 * @return true   on success (system clock calibrated by NTP; time_t is not an outlier)
 *         false  if not calibrated yet (caller decides, e.g. shows "--:--"); buf is unchanged
 */
bool sf_ntp_get_local_time(char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* SF_NTP_H */
