#include "sf_sys.h"
#include "sf_wifi.h"
#include "sf_ntp.h"
#include "sf_notification.h"
#include "sf_config.h"
#include "esp_log.h"

static const char *TAG = "sf_sys";

const char *sf_app_state_to_str(sf_app_state_t state)
{
    switch (state) {
    case SF_APP_UNREGISTERED: return "UNREGISTERED";
    case SF_APP_CREATED:      return "CREATED";
    case SF_APP_STARTED:      return "STARTED";
    case SF_APP_RESUMED:      return "RESUMED";
    case SF_APP_PAUSED:       return "PAUSED";
    case SF_APP_STOPPED:      return "STOPPED";
    case SF_APP_DESTROYED:    return "DESTROYED";
    default:                  return "?";
    }
}

extern esp_err_t sf_event_bus_init(void);

static void post_connected_notification(void)
{
    const char *ssid = sf_wifi_get_ssid();
    if (ssid && ssid[0]) {
        char body[128];
        snprintf(body, sizeof(body), "Connected to %s", ssid);
        sf_notification_post("system", "\xEF\x87\xAB", "Wi-Fi", body);
    } else {
        sf_notification_post("system", "\xEF\x87\xAB", "Wi-Fi", "Connected");
    }
}

esp_err_t sf_sys_init(void)
{
    ESP_LOGI(TAG, "StarryFire System Services initializing...");

    ESP_ERROR_CHECK(sf_event_bus_init());
    ESP_ERROR_CHECK(sf_app_manager_init());
    sf_config_init();

    /* The notification module must be initialized before Wi-Fi starts connecting,
       otherwise the slot does not exist yet when WIFI_EVENT/IP_EVENT are bridged over, and events are lost */
    sf_notification_init();
    sf_wifi_init();
    sf_ntp_init();

    /* If already connected at boot (e.g. a fast reconnect), post a notification */
    if (sf_wifi_get_state() == SF_WIFI_STATE_CONNECTED) {
        post_connected_notification();
    }

    ESP_LOGI(TAG, "System Services ready");

    return ESP_OK;
}
