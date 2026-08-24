#include "sf_notification.h"
#include "sf_sys.h"
#include "sf_wifi.h"
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "sf_notif";

/* ── Ring buffer storage ───────────────────────────────── */

static sf_notification_t s_notifs[SF_NOTIF_MAX_COUNT];
static int s_head;   /* index of the newest notification */
static int s_count;  /* number of valid notifications */
static uint32_t s_next_id;
static SemaphoreHandle_t s_lock;

/* ── Internal helpers ─────────────────────────────────────── */

static int idx_of(int i)
{
    /* s_head is the newest write position; index 0 = newest */
    int pos = s_head - i;
    if (pos < 0) pos += SF_NOTIF_MAX_COUNT;
    return pos;
}

/* ── Event Bus callbacks — auto-generated notifications ──── */

static void on_event(esp_event_base_t base, int32_t id, void *data, void *ctx)
{
    ESP_LOGI(TAG, "on_event base=%s id=%" PRId32, base, id);
    if (strcmp(base, WIFI_EVENT) == 0 && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, ">> posting Wi-Fi Disconnected");
        sf_notification_post_action("system", "\xEF\x87\xAB",
                                     "Wi-Fi", "Disconnected", "sf.intent.action.SETTINGS:wifi");
    } else if (strcmp(base, IP_EVENT) == 0 && id == IP_EVENT_STA_GOT_IP) {
        const char *ssid = sf_wifi_get_ssid();
        if (ssid && ssid[0]) {
            char body[128];
            snprintf(body, sizeof(body), "Connected to %s", ssid);
            sf_notification_post_action("system", "\xEF\x87\xAB",
                                         "Wi-Fi", body, NULL);
        } else {
            sf_notification_post_action("system", "\xEF\x87\xAB",
                                         "Wi-Fi", "Connected", NULL);
        }
    } else if (strcmp(base, SF_EVENT_BASE) == 0 && id == SF_EVENT_BATTERY_LOW) {
        sf_notification_post("system", "\xEF\x89\x84",
                              "Low Battery", "Battery is running low");
    }
}

/* ── Public API ─────────────────────────────────────── */

esp_err_t sf_notification_init(void)
{
    memset(s_notifs, 0, sizeof(s_notifs));
    s_head = 0;
    s_count = 0;
    s_next_id = 1;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    /* Subscribe to system events to auto-generate notifications */
    sf_event_subscribe(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL);
    sf_event_subscribe(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    sf_event_subscribe(SF_EVENT_BASE, SF_EVENT_BATTERY_LOW, on_event, NULL);

    ESP_LOGI(TAG, "notification service ready");
    return ESP_OK;
}

esp_err_t sf_notification_post(const char *app_id, const char *icon,
                                const char *title, const char *body)
{
    if (!title || !title[0]) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* If full, drop the oldest one */
    if (s_count >= SF_NOTIF_MAX_COUNT) {
        s_count = SF_NOTIF_MAX_COUNT - 1;
        /* Head pointer stays put: overwrite the oldest slot (s_head - SF_NOTIF_MAX_COUNT + 1) */
    }

    /* Write the new notification at the head */
    s_head = (s_head + 1) % SF_NOTIF_MAX_COUNT;

    sf_notification_t *n = &s_notifs[s_head];
    memset(n, 0, sizeof(*n));
    n->id = s_next_id++;
    n->timestamp_ms = esp_log_timestamp();  /* ms since boot */
    if (app_id) strncpy(n->app_id, app_id, sizeof(n->app_id) - 1);
    if (icon)   strncpy(n->icon, icon, sizeof(n->icon) - 1);
    strncpy(n->title, title, sizeof(n->title) - 1);
    if (body)   strncpy(n->body, body, sizeof(n->body) - 1);
    n->read = false;

    if (s_count < SF_NOTIF_MAX_COUNT) s_count++;

    ESP_LOGI(TAG, "post notif #%" PRIu32 ": %s", n->id, n->title);

    /* Snapshot the notification before releasing the lock, so the UI callback
     * reads consistent data without holding s_lock (avoids reentrancy if the
     * callback ever calls sf_notification_post / dismiss). */
    sf_notification_t copy = *n;

    xSemaphoreGive(s_lock);

    /* Notify the UI layer — runs without s_lock held */
    sf_event_publish(SF_EVENT_BASE, SF_EVENT_NOTIFICATION_POST, &copy);
    return ESP_OK;
}

esp_err_t sf_notification_post_action(const char *app_id, const char *icon,
                                       const char *title, const char *body,
                                       const char *action)
{
    if (!title || !title[0]) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* Ring buffer write (same logic as sf_notification_post, inlined to keep
     * the lock held so we can atomically write the action too). */
    if (s_count >= SF_NOTIF_MAX_COUNT) {
        s_count = SF_NOTIF_MAX_COUNT - 1;
    }

    s_head = (s_head + 1) % SF_NOTIF_MAX_COUNT;

    sf_notification_t *n = &s_notifs[s_head];
    memset(n, 0, sizeof(*n));
    n->id = s_next_id++;
    n->timestamp_ms = esp_log_timestamp();
    if (app_id) strncpy(n->app_id, app_id, sizeof(n->app_id) - 1);
    if (icon)   strncpy(n->icon, icon, sizeof(n->icon) - 1);
    strncpy(n->title, title, sizeof(n->title) - 1);
    if (body)   strncpy(n->body, body, sizeof(n->body) - 1);
    if (action && action[0]) {
        strncpy(n->action, action, sizeof(n->action) - 1);
    }
    n->read = false;

    if (s_count < SF_NOTIF_MAX_COUNT) s_count++;

    ESP_LOGI(TAG, "post notif #%" PRIu32 ": %s", n->id, n->title);

    sf_notification_t copy = *n;
    xSemaphoreGive(s_lock);

    sf_event_publish(SF_EVENT_BASE, SF_EVENT_NOTIFICATION_POST, &copy);
    return ESP_OK;
}

esp_err_t sf_notification_dismiss(uint32_t id)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        sf_notification_t *n = &s_notifs[idx_of(i)];
        if (n->id == id) {
            /* Overwrite with the last one */
            int last = idx_of(s_count - 1);
            if (i < s_count - 1) {
                *n = s_notifs[last];
            }
            memset(&s_notifs[last], 0, sizeof(s_notifs[last]));
            s_count--;
            ESP_LOGI(TAG, "dismiss notif #%" PRIu32, id);
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_FOUND;
}

void sf_notification_clear_all(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_notifs, 0, sizeof(s_notifs));
    s_head = 0;
    s_count = 0;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "clear all notifs");
}

int sf_notification_get_count(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int count = s_count;
    xSemaphoreGive(s_lock);
    return count;
}

const sf_notification_t *sf_notification_get(int index)
{
    /* Returns a pointer into the static array; callers should use it promptly, outside the lock */
    if (index < 0 || index >= s_count) return NULL;
    return &s_notifs[idx_of(index)];
}

int sf_notification_get_unread_count(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int unread = 0;
    for (int i = 0; i < s_count; i++) {
        if (!s_notifs[idx_of(i)].read) unread++;
    }
    xSemaphoreGive(s_lock);
    return unread;
}

esp_err_t sf_notification_mark_read(uint32_t id)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        sf_notification_t *n = &s_notifs[idx_of(i)];
        if (n->id == id) {
            n->read = true;
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_FOUND;
}
