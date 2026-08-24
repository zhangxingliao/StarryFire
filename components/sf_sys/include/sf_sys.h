#ifndef SF_SYS_H
#define SF_SYS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── EventBus ───────────────────────────────────────
 * Central system event hub. All system-level events (Wi-Fi, IP,
 * custom events, etc.) are broadcast and stored through the EventBus
 * in the (base, id) format.
 *
 * - For native ESP-IDF events (WIFI_EVENT / IP_EVENT, etc.), the
 *   EventBus attaches itself to the ESP-IDF default event loop
 *   automatically; no manual bridging is needed.
 * - For custom system events, use SF_EVENT_BASE + sf_event_id_t.
 *
 * Apps should subscribe to events here only; do not register ESP-IDF
 * event handlers directly.
 * ────────────────────────────────────────────────── */

/* base for custom system events */
#define SF_EVENT_BASE "SF_EVENT"

/* custom system event IDs (used together with SF_EVENT_BASE) */
typedef enum {
    SF_EVENT_BATTERY_LOW   = 0x01,
    SF_EVENT_APP_LAUNCH,
    SF_EVENT_APP_EXIT,
    SF_EVENT_BACK_PRESSED,
    SF_EVENT_HOME_PRESSED,
    SF_EVENT_SCREEN_ON,
    SF_EVENT_SCREEN_OFF,
    SF_EVENT_NOTIFICATION_POST,    /* payload = sf_notification_t* */
    SF_EVENT_APP_INTENT,           /* payload = const sf_intent_t * */
} sf_event_id_t;

/** Event callback: base/id match the ESP-IDF format; event_data may be NULL */
typedef void (*sf_event_cb_t)(esp_event_base_t base, int32_t id,
                               void *event_data, void *user_data);

esp_err_t sf_event_subscribe(esp_event_base_t base, int32_t id,
                              sf_event_cb_t cb, void *user_data);
esp_err_t sf_event_unsubscribe(esp_event_base_t base, int32_t id,
                                sf_event_cb_t cb, void *user_data);
esp_err_t sf_event_publish(esp_event_base_t base, int32_t id,
                            void *event_data);

/* Get the state data of the most recent event (may be NULL) */
void *sf_event_get_state(esp_event_base_t base, int32_t id);

esp_err_t sf_event_bus_init(void);

/* ── Intent ──────────────────────────────────────── */

#define SF_INTENT_ACTION_VIEW     "sf.intent.action.VIEW"
#define SF_INTENT_ACTION_MAIN     "sf.intent.action.MAIN"
#define SF_INTENT_ACTION_SETTINGS "sf.intent.action.SETTINGS"
#define SF_INTENT_CATEGORY_DEFAULT "sf.intent.category.DEFAULT"

typedef struct {
    const char *key;
    enum {
        SF_EXTRA_STRING,
        SF_EXTRA_INT,
    } type;
    union {
        const char *str_val;
        int32_t int_val;
    };
} sf_intent_extra_t;

typedef struct {
    const char *action;
    const char *category;
    const char *target_app_id;
    const sf_intent_extra_t *extras;
    size_t extras_count;
} sf_intent_t;

/* ── App Lifecycle States ────────────────────────── */

typedef enum {
    SF_APP_UNREGISTERED = 0,
    SF_APP_CREATED,
    SF_APP_STARTED,
    SF_APP_RESUMED,
    SF_APP_PAUSED,
    SF_APP_STOPPED,
    SF_APP_DESTROYED,
} sf_app_state_t;

const char *sf_app_state_to_str(sf_app_state_t state);

/* ── App Ops (callbacks) ─────────────────────────── */

typedef struct sf_app_ctx_t sf_app_ctx_t;

typedef struct {
    esp_err_t (*on_create)(sf_app_ctx_t *ctx);
    void (*on_start)(sf_app_ctx_t *ctx);
    void (*on_resume)(sf_app_ctx_t *ctx);
    void (*on_pause)(sf_app_ctx_t *ctx);
    void (*on_stop)(sf_app_ctx_t *ctx);
    void (*on_destroy)(sf_app_ctx_t *ctx);
    bool (*on_back)(sf_app_ctx_t *ctx);
    void (*on_event)(sf_app_ctx_t *ctx, esp_event_base_t base, int32_t id, void *event_data);
} sf_app_ops_t;

/* ── App Intent Filter ───────────────────────────── */

typedef struct {
    const char *action;
    const char *category;
} sf_app_intent_filter_t;

/* ── App Manifest ────────────────────────────────── */

#define SF_APP_FLAG_SHOW_IN_LAUNCHER  (1 << 0)
#define SF_APP_FLAG_PINNED            (1 << 1)
#define SF_APP_FLAG_ALLOW_FULLSCREEN  (1 << 2)

typedef struct {
    const char *id;
    const char *name;
    const char *icon;
    const char *version;
    uint32_t flags;
    const sf_app_ops_t *ops;
    const sf_app_intent_filter_t *intent_filters;
    size_t intent_filters_count;
} sf_app_manifest_t;

#define SF_APP_INTENT_FILTER(action_val, category_val) \
    { .action = action_val, .category = category_val }

/* ── App GUI Interface (injected by the GUI layer) ──────────────
 * Runtime dependency inversion: the AppManager (System) holds window
 * management callbacks implemented by the GUI layer and invokes them
 * synchronously at lifecycle points on the GUI task. The GUI layer in
 * turn calls back into System (sf_app_start / sf_app_pause_current),
 * forming an intentional single-task cooperation loop despite the
 * one-way compile-time include direction. */

typedef struct {
    void (*pre_create)(sf_app_ctx_t *ctx);      /* build the App's UI root into ui_root */
    void (*post_destroy)(sf_app_ctx_t *ctx);    /* tear down the App's UI root */
    void (*on_resume_show)(sf_app_ctx_t *ctx);  /* show the App's UI root */
    void (*on_pause_hide)(sf_app_ctx_t *ctx);   /* hide the App's UI root */
} sf_gui_window_ops_t;

void sf_app_manager_set_gui_ops(const sf_gui_window_ops_t *ops);

/* ── App Runtime Context ─────────────────────────── */

struct sf_app_ctx_t {
    const sf_app_manifest_t *manifest;
    sf_app_state_t state;
    void *user_data;
    void *ui_root;       /**< App-specific UI root container (lv_obj_t*), created/destroyed by the GUI layer via hooks */
    uint64_t lru_tick;
};

/* ── AppManager API ──────────────────────────────── */

esp_err_t sf_app_manager_init(void);
const sf_app_manifest_t *sf_app_register(const sf_app_manifest_t *manifest);
esp_err_t sf_app_unregister(const char *app_id);
esp_err_t sf_app_pause_current(void);

esp_err_t sf_app_start(const char *app_id);
esp_err_t sf_app_start_intent(const sf_intent_t *intent);
esp_err_t sf_app_go_back(void);

sf_app_ctx_t *sf_app_get_current(void);
void sf_app_foreach(bool (*cb)(sf_app_ctx_t *ctx, void *arg), void *arg);

const char *sf_intent_extra_string(const sf_intent_t *intent, const char *key, const char *def);
int32_t sf_intent_extra_int(const sf_intent_t *intent, const char *key, int32_t def);

/* ── System Init ─────────────────────────────────── */

esp_err_t sf_sys_init(void);

#ifdef __cplusplus
}
#endif

#endif
