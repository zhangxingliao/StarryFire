/**
 * sf_ntp.c — StarryFire NTP time service implementation
 */

#include "sf_ntp.h"
#include "sf_config.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "lwip/apps/sntp.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "sf_ntp";

#define NTP_SERVER_PRIMARY   "pool.ntp.org"
#define NTP_SERVER_SECONDARY "cn.pool.ntp.org"

static bool s_initialized = false;

esp_err_t sf_ntp_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    /* Apply the timezone from Settings */
    const char *tz = sf_config_get_timezone();
    if (tz && tz[0] != '\0') {
        setenv("TZ", tz, 1);
        tzset();
        ESP_LOGI(TAG, "timezone applied: %s", tz);
    }

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER_PRIMARY);
    cfg.smooth_sync        = false;
    cfg.server_from_dhcp   = false;
    cfg.renew_servers_after_new_IP = true;
    cfg.num_of_servers     = 2;
    cfg.servers[0]         = NTP_SERVER_PRIMARY;
    cfg.servers[1]         = NTP_SERVER_SECONDARY;

    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sntp init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "SNTP started (%s / %s)", NTP_SERVER_PRIMARY, NTP_SERVER_SECONDARY);
    return ESP_OK;
}

void sf_ntp_set_tz(const char *tz)
{
    if (!tz || tz[0] == '\0') {
        return;
    }
    setenv("TZ", tz, 1);
    tzset();
    ESP_LOGI(TAG, "timezone updated: %s", tz);
}

void sf_ntp_sync_now(void)
{
    /* SNTP already syncs periodically in the background, so no manual
       trigger is needed; this hook is kept so we can call
       esp_netif_sntp_start() explicitly if required later (e.g. right after connecting). */
}

bool sf_ntp_get_local_time(char *buf, size_t buf_len)
{
    if (!buf || buf_len < 6) {
        return false;
    }

    time_t now = time(NULL);
    /* When the system clock has not been calibrated, time(NULL) returns a bogus value near 1970 */
    if (now < (time_t)1700000000) {   /* times before 2023-11-14 are considered uncalibrated */
        return false;
    }

    struct tm tm_info;
    localtime_r(&now, &tm_info);
    snprintf(buf, buf_len, "%02d:%02d", tm_info.tm_hour, tm_info.tm_min);
    return true;
}
