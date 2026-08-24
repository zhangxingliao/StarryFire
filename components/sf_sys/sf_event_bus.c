#include "sf_sys.h"
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "sf_event_bus";

/* ── Config ────────────────────────────────────────── */

#define MAX_SLOTS  32
#ifndef CONFIG_SF_SYS_EVENT_SUBSCRIBERS_PER_EVENT
#define CONFIG_SF_SYS_EVENT_SUBSCRIBERS_PER_EVENT 8
#endif
#define MAX_SUBS   CONFIG_SF_SYS_EVENT_SUBSCRIBERS_PER_EVENT

/* ── Slots (one slot per (base, id)) ───────────────── */

typedef struct {
    sf_event_cb_t cb;
    void *user_data;
} subscriber_t;

typedef struct {
    esp_event_base_t base;
    int32_t id;
    bool active;
    subscriber_t subs[MAX_SUBS];
    size_t sub_count;
    void *state_buf;       /* copy of the payload of the last event */
    size_t state_len;
} event_slot_t;

static event_slot_t s_slots[MAX_SLOTS];
static size_t s_slot_count;
static SemaphoreHandle_t s_lock;
static bool s_inited;

/* ── Bridging (ESP-IDF → Event Bus) ───────────────── */

static void bridge_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    /* Treat it like a normal publish */
    sf_event_publish(base, id, data);
}

/* ── Internal helpers ─────────────────────────────────────── */

static event_slot_t *find_slot(esp_event_base_t base, int32_t id)
{
    for (size_t i = 0; i < s_slot_count; i++) {
        if (s_slots[i].active &&
            s_slots[i].id == id &&
            strcmp(s_slots[i].base, base) == 0) {
            return &s_slots[i];
        }
    }
    return NULL;
}

static event_slot_t *alloc_slot(esp_event_base_t base, int32_t id)
{
    if (s_slot_count >= MAX_SLOTS) {
        ESP_LOGW(TAG, "slot full (%d)", MAX_SLOTS);
        return NULL;
    }
    event_slot_t *slot = &s_slots[s_slot_count++];
    slot->base = base;
    slot->id = id;
    slot->active = true;
    return slot;
}

/* ── Public API ─────────────────────────────────────── */

esp_err_t sf_event_bus_init(void)
{
    if (s_inited) return ESP_OK;

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    memset(s_slots, 0, sizeof(s_slots));

    /* Make sure the default event loop is created (sf_wifi_init creates it too, but we run before that) */
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "create default loop failed: %s",
                 esp_err_to_name(loop_err));
    }

    /* Register the bridge: capture the known ESP-IDF event bases */
    const esp_event_base_t bridged[] = { WIFI_EVENT, IP_EVENT, NULL };
    for (int i = 0; bridged[i]; i++) {
        esp_err_t err = esp_event_handler_instance_register(
            bridged[i], ESP_EVENT_ANY_ID,
            bridge_handler, NULL, NULL);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "bridge register %s failed: %s",
                     bridged[i], esp_err_to_name(err));
        }
    }

    s_inited = true;
    ESP_LOGI(TAG, "event bus ready (%d slots)", MAX_SLOTS);
    return ESP_OK;
}

esp_err_t sf_event_subscribe(esp_event_base_t base, int32_t id,
                              sf_event_cb_t cb, void *user_data)
{
    if (!cb || !base) return ESP_ERR_INVALID_ARG;
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    event_slot_t *slot = find_slot(base, id);
    if (!slot) {
        slot = alloc_slot(base, id);
        if (!slot) {
            xSemaphoreGive(s_lock);
            return ESP_ERR_NO_MEM;
        }
    }

    if (slot->sub_count >= MAX_SUBS) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }

    slot->subs[slot->sub_count].cb = cb;
    slot->subs[slot->sub_count].user_data = user_data;
    slot->sub_count++;

    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t sf_event_unsubscribe(esp_event_base_t base, int32_t id,
                                sf_event_cb_t cb, void *user_data)
{
    if (!cb || !base) return ESP_ERR_INVALID_ARG;
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    event_slot_t *slot = find_slot(base, id);
    if (!slot) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    for (size_t i = 0; i < slot->sub_count; i++) {
        if (slot->subs[i].cb == cb && slot->subs[i].user_data == user_data) {
            slot->subs[i] = slot->subs[--slot->sub_count];
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
    }

    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t sf_event_publish(esp_event_base_t base, int32_t id,
                            void *event_data)
{
    if (!base) return ESP_ERR_INVALID_ARG;
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    event_slot_t *slot = find_slot(base, id);
    if (!slot) {
        ESP_LOGD(TAG, "publish %s:%" PRId32 " — no subscribers, dropped", base, id);
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    /* Update the state (shallow copy of what event_data points to) */
    if (slot->state_buf) {
        free(slot->state_buf);
        slot->state_buf = NULL;
        slot->state_len = 0;
    }
    /* The payload size is unknown to us, since ESP-IDF events carry no length.
     * Callers must call sf_event_get_state() themselves and know the type.
     * Here we only save a copy of the event_data pointer (a shallow reference —
     * event_data stays valid only until the handler returns),
     * so we store NULL and let callers fetch the state via a service-layer API. */
    /* Note: event_data becomes invalid once the handler returns, so it cannot be stored safely. */

    /* Copy the subscriber list (we cannot hold the lock while iterating) */
    subscriber_t copy[MAX_SUBS];
    size_t count = slot->sub_count;
    memcpy(copy, slot->subs, count * sizeof(subscriber_t));

    xSemaphoreGive(s_lock);

    for (size_t i = 0; i < count; i++) {
        copy[i].cb(base, id, event_data, copy[i].user_data);
    }

    return ESP_OK;
}

void *sf_event_get_state(esp_event_base_t base, int32_t id)
{
    if (!base || !s_inited) return NULL;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    event_slot_t *slot = find_slot(base, id);
    xSemaphoreGive(s_lock);

    if (!slot) return NULL;

    /* Deep state storage is not supported yet, so return NULL.
     * Callers should fetch the latest state through the matching service-layer API
     * (e.g. sf_wifi_get_state()). If automatic state caching is ever needed,
     * a dedicated serializer would have to be registered per (base, id). */
    return NULL;
}
