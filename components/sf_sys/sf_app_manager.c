#include "sf_sys.h"
#include <string.h>
#include <stdint.h>
#include "esp_log.h"

static const char *TAG = "sf_app_mgr";

/* ── State machine helpers ────────────────────────── */

struct transition {
    sf_app_state_t from;
    sf_app_state_t to;
};

#define TRANS(_from, _to)   { .from = (_from), .to = (_to) }

static const struct transition s_valid_transitions[] = {
    TRANS(SF_APP_UNREGISTERED, SF_APP_CREATED),
    TRANS(SF_APP_CREATED,      SF_APP_STARTED),
    TRANS(SF_APP_STARTED,      SF_APP_RESUMED),
    TRANS(SF_APP_RESUMED,      SF_APP_PAUSED),
    TRANS(SF_APP_PAUSED,       SF_APP_RESUMED),
    TRANS(SF_APP_PAUSED,       SF_APP_STOPPED),
    TRANS(SF_APP_STOPPED,      SF_APP_DESTROYED),
    TRANS(SF_APP_CREATED,      SF_APP_UNREGISTERED),
    TRANS(SF_APP_RESUMED,      SF_APP_DESTROYED),
    TRANS(SF_APP_STARTED,      SF_APP_DESTROYED),
    TRANS(SF_APP_PAUSED,       SF_APP_DESTROYED),
};

static bool is_valid_transition(sf_app_state_t from, sf_app_state_t to)
{
    for (size_t i = 0; i < sizeof(s_valid_transitions) / sizeof(s_valid_transitions[0]); i++) {
        if (s_valid_transitions[i].from == from && s_valid_transitions[i].to == to) return true;
    }
    return false;
}

/* ── App table ─────────────────────────────────────── */

static sf_app_ctx_t s_apps[CONFIG_SF_SYS_MAX_REGISTERED_APPS];
static size_t s_app_count;

static size_t s_history[CONFIG_SF_SYS_MAX_REGISTERED_APPS];
static size_t s_history_depth;

static int s_current_idx;

/* ── GUI window interface (injected by the GUI layer) ─── */

static const sf_gui_window_ops_t *s_gui_ops;

void sf_app_manager_set_gui_ops(const sf_gui_window_ops_t *ops)
{
    s_gui_ops = ops;
}

/* ── LRU tracking for paused Apps ──────────────────── */

static uint64_t s_tick_base;

static void lru_touch(sf_app_ctx_t *ctx)
{
    ctx->lru_tick = s_tick_base++;
}

static sf_app_ctx_t *lru_find_victim(void)
{
    sf_app_ctx_t *victim = NULL;
    uint64_t oldest = UINT64_MAX;

    for (size_t i = 0; i < s_app_count; i++) {
        if (s_apps[i].state == SF_APP_PAUSED && s_apps[i].lru_tick < oldest) {
            oldest = s_apps[i].lru_tick;
            victim = &s_apps[i];
        }
    }
    return victim;
}

/* ── Internal helpers ──────────────────────────────── */

static sf_app_ctx_t *ctx_by_id(const char *id)
{
    for (size_t i = 0; i < s_app_count; i++) {
        if (strcmp(s_apps[i].manifest->id, id) == 0) return &s_apps[i];
    }
    return NULL;
}

static int ctx_index(sf_app_ctx_t *ctx)
{
    if (!ctx) return -1;
    ptrdiff_t diff = ctx - s_apps;
    return (diff >= 0 && (size_t)diff < s_app_count) ? (int)diff : -1;
}

static void push_history(int idx)
{
    if (s_history_depth > 0 && s_history[s_history_depth - 1] == (size_t)idx) return;
    if (s_history_depth < CONFIG_SF_SYS_MAX_REGISTERED_APPS) {
        s_history[s_history_depth++] = (size_t)idx;
    }
}

static int pop_history(void)
{
    if (s_history_depth == 0) return -1;
    return (int)s_history[--s_history_depth];
}

/* Peek the top entry without removing it (used by sf_app_go_back) */
static int peek_history(void)
{
    if (s_history_depth == 0) return -1;
    return (int)s_history[s_history_depth - 1];
}

/* Keep the history stack consistent after sf_app_unregister shuffles slots:
 * drop any entry that pointed at the unregistered app and remap the surviving
 * app that moved into the freed slot (no-op when there was no shuffle). */
static void remap_history(int freed, int moved_from)
{
    for (size_t i = 0; i < s_history_depth; ) {
        int v = (int)s_history[i];
        if (v == freed) {
            memmove(&s_history[i], &s_history[i + 1],
                    (s_history_depth - i - 1) * sizeof(s_history[0]));
            s_history_depth--;
        } else {
            if (v == moved_from) s_history[i] = (size_t)freed;
            i++;
        }
    }
}

static void do_transition(sf_app_ctx_t *ctx, sf_app_state_t to)
{
    sf_app_state_t from = ctx->state;
    if (!is_valid_transition(from, to)) {
        ESP_LOGE(TAG, "invalid transition %s -> %s for app %s",
                 sf_app_state_to_str(from), sf_app_state_to_str(to),
                 ctx->manifest ? ctx->manifest->id : "?");
        return;
    }

    ctx->state = to;
    ESP_LOGI(TAG, "[%s] %s -> %s",
             ctx->manifest->id,
             sf_app_state_to_str(from),
             sf_app_state_to_str(to));

    switch (to) {
    case SF_APP_CREATED:
        if (from == SF_APP_UNREGISTERED) {
            if (s_gui_ops && s_gui_ops->pre_create) s_gui_ops->pre_create(ctx);
            if (ctx->manifest->ops->on_create) {
                esp_err_t ret = ctx->manifest->ops->on_create(ctx);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "on_create failed for %s: %s",
                             ctx->manifest->id, esp_err_to_name(ret));
                    if (s_gui_ops && s_gui_ops->post_destroy) s_gui_ops->post_destroy(ctx);
                    ctx->state = SF_APP_UNREGISTERED;
                    return;
                }
            }
        }
        break;
    case SF_APP_STARTED:
        if (ctx->manifest->ops->on_start) ctx->manifest->ops->on_start(ctx);
        break;
    case SF_APP_RESUMED:
        s_current_idx = ctx_index(ctx);
        if (s_gui_ops && s_gui_ops->on_resume_show) s_gui_ops->on_resume_show(ctx);
        if (ctx->manifest->ops->on_resume) ctx->manifest->ops->on_resume(ctx);
        sf_event_publish(SF_EVENT_BASE, SF_EVENT_APP_LAUNCH, ctx->manifest->id);
        break;
    case SF_APP_PAUSED:
        if (ctx->manifest->ops->on_pause) ctx->manifest->ops->on_pause(ctx);
        if (s_gui_ops && s_gui_ops->on_pause_hide) s_gui_ops->on_pause_hide(ctx);
        lru_touch(ctx);
        break;
    case SF_APP_STOPPED:
        if (ctx->manifest->ops->on_stop) ctx->manifest->ops->on_stop(ctx);
        break;
    case SF_APP_DESTROYED:
        if (from != SF_APP_STOPPED) {
            if (ctx->manifest->ops->on_stop) ctx->manifest->ops->on_stop(ctx);
        }
        if (ctx->manifest->ops->on_destroy) ctx->manifest->ops->on_destroy(ctx);
        if (s_gui_ops && s_gui_ops->post_destroy) s_gui_ops->post_destroy(ctx);
        ctx->state = SF_APP_UNREGISTERED;
        break;
    default:
        break;
    }
}

/* ── LRU eviction ──────────────────────────────────── */

static void evict_if_needed(sf_app_ctx_t *exclude)
{
    size_t paused = 0;
    for (size_t i = 0; i < s_app_count; i++) {
        if (s_apps[i].state == SF_APP_PAUSED && &s_apps[i] != exclude) paused++;
    }

    while (paused >= (size_t)CONFIG_SF_SYS_MAX_PAUSED_APPS && paused > 0) {
        sf_app_ctx_t *victim = lru_find_victim();
        if (!victim || victim == exclude) break;
        ESP_LOGI(TAG, "evicting PAUSED app %s (LRU)", victim->manifest->id);
        do_transition(victim, SF_APP_STOPPED);
        do_transition(victim, SF_APP_DESTROYED);
        paused--;
    }
}

/* ── Public API ────────────────────────────────────── */

esp_err_t sf_app_manager_init(void)
{
    memset(s_apps, 0, sizeof(s_apps));
    s_app_count = 0;
    s_history_depth = 0;
    s_current_idx = -1;
    s_tick_base = 1;

    ESP_LOGI(TAG, "app manager init: ready (apps registered explicitly)");
    return ESP_OK;
}

const sf_app_manifest_t *sf_app_register(const sf_app_manifest_t *manifest)
{
    if (!manifest) return NULL;

    if (s_app_count >= CONFIG_SF_SYS_MAX_REGISTERED_APPS) {
        ESP_LOGE(TAG, "app table full");
        return NULL;
    }

    sf_app_ctx_t *ctx = &s_apps[s_app_count++];
    ctx->manifest = manifest;
    ctx->state = SF_APP_UNREGISTERED;
    ctx->user_data = NULL;
    ctx->ui_root = NULL;

    ESP_LOGI(TAG, "registered app %s (will create on first start)", manifest->id);
    return manifest;
}

esp_err_t sf_app_unregister(const char *app_id)
{
    sf_app_ctx_t *ctx = ctx_by_id(app_id);
    if (!ctx) return ESP_ERR_NOT_FOUND;

    if (ctx->state != SF_APP_UNREGISTERED) {
        do_transition(ctx, SF_APP_DESTROYED);
    }

    int idx = ctx_index(ctx);
    if (idx >= 0) {
        int last = (int)--s_app_count;
        if (idx != last) {
            s_apps[idx] = s_apps[last];
        }
        remap_history(idx, last);
        if (s_current_idx == last) {
            s_current_idx = idx;
        } else if (s_current_idx == idx) {
            s_current_idx = -1;
        }
    }

    return ESP_OK;
}

/* ── Intent resolution helpers ─────────────────────── */

typedef struct {
    const sf_intent_t *intent;
    const sf_app_manifest_t *result;
} resolve_ctx_t;

static bool resolve_by_intent(sf_app_ctx_t *ctx, void *arg)
{
    resolve_ctx_t *rctx = (resolve_ctx_t *)arg;
    for (size_t i = 0; i < ctx->manifest->intent_filters_count; i++) {
        const sf_app_intent_filter_t *f = &ctx->manifest->intent_filters[i];
        if (f->action && rctx->intent->action &&
            strcmp(f->action, rctx->intent->action) == 0) {
            rctx->result = ctx->manifest;
            return false;
        }
    }
    return true;
}

/* ── Lifecycle operations ──────────────────────────── */

/* Bring an app to the foreground (RESUMED) without touching the history
 * stack. Shared by sf_app_start (which additionally records navigation) and
 * sf_app_go_back (which manages the stack itself). */
static esp_err_t resume_foreground(sf_app_ctx_t *ctx)
{
    if (!ctx) return ESP_ERR_NOT_FOUND;

    /* Already foreground — no state change; history stays untouched too */
    if (ctx->state == SF_APP_RESUMED) {
        return ESP_OK;
    }

    /* SF_APP_STOPPED must first be DESTROYED and then re-CREATED */
    if (ctx->state == SF_APP_STOPPED) {
        do_transition(ctx, SF_APP_DESTROYED);
    }

    /* First start or restart after destruction: UNREGISTERED → CREATED */
    if (ctx->state == SF_APP_UNREGISTERED) {
        do_transition(ctx, SF_APP_CREATED);
    }

    /* Pause the current foreground app */
    sf_app_ctx_t *cur = sf_app_get_current();
    if (cur && cur != ctx) {
        do_transition(cur, SF_APP_PAUSED);
        evict_if_needed(ctx);
    }

    /* CREATED → STARTED → RESUMED */
    if (ctx->state == SF_APP_CREATED) {
        do_transition(ctx, SF_APP_STARTED);
    }
    if (ctx->state == SF_APP_STARTED || ctx->state == SF_APP_PAUSED) {
        do_transition(ctx, SF_APP_RESUMED);
    }

    return ESP_OK;
}

esp_err_t sf_app_start(const char *app_id)
{
    sf_app_ctx_t *ctx = ctx_by_id(app_id);
    if (!ctx) {
        ESP_LOGE(TAG, "app %s not registered", app_id);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret = resume_foreground(ctx);
    if (ret != ESP_OK) return ret;

    /* sf_app_start is the public "launch" entry point (launcher tap, intent,
     * notification): record navigation in the history stack so that
     * sf_app_go_back can walk back. Resuming the app already on top is a
     * no-op (push_history dedups), so the stack never gets duplicates. */
    push_history(ctx_index(ctx));
    return ESP_OK;
}

esp_err_t sf_app_start_intent(const sf_intent_t *intent)
{
    if (!intent) return ESP_ERR_INVALID_ARG;

    const sf_app_manifest_t *m = NULL;

    if (intent->target_app_id) {
        sf_app_ctx_t *ctx = ctx_by_id(intent->target_app_id);
        if (ctx) m = ctx->manifest;
    }

    if (!m) {
        resolve_ctx_t rctx = { .intent = intent, .result = NULL };
        sf_app_foreach(resolve_by_intent, &rctx);
        m = rctx.result;
    }

    if (!m) {
        ESP_LOGE(TAG, "no app matches intent action=%s", intent->action);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret = sf_app_start(m->id);
    if (ret == ESP_OK) {
        /* Deliver the intent directly to the resolved target app. The target
         * has just finished on_create/on_start/on_resume, so its user_data is
         * valid; delivery runs in the caller's task (same task that built the
         * UI), so app code may touch LVGL safely. */
        sf_app_ctx_t *ctx = ctx_by_id(m->id);
        if (ctx && ctx->manifest->ops->on_event) {
            ctx->manifest->ops->on_event(ctx, SF_EVENT_BASE, SF_EVENT_APP_INTENT,
                                         (void *)intent);
        }
    }
    return ret;
}

esp_err_t sf_app_go_back(void)
{
    /* The history stack mirrors navigation order and always has the current
     * foreground app on top (sf_app_start pushes the app it resumes).
     * Pop the current app's entry, then resume the app now on top through a
     * dedicated pop-and-resume path that does NOT call sf_app_start (calling
     * it would re-push the target and, if the target was already RESUMED, the
     * early return would leave the stack corrupted). */
    if (!sf_app_get_current()) {
        return ESP_ERR_NOT_FOUND;
    }

    int cur = pop_history();
    if (cur < 0 || (size_t)cur >= s_app_count) return ESP_ERR_NOT_FOUND;

    /* Nothing below the current app: restore the stack and report no target */
    int prev = peek_history();
    if (prev < 0 || (size_t)prev >= s_app_count) {
        push_history(cur);
        return ESP_ERR_NOT_FOUND;
    }

    return resume_foreground(&s_apps[prev]);
}

sf_app_ctx_t *sf_app_get_current(void)
{
    if (s_current_idx < 0 || (size_t)s_current_idx >= s_app_count) return NULL;
    if (s_apps[s_current_idx].state != SF_APP_RESUMED) return NULL;
    return &s_apps[s_current_idx];
}

esp_err_t sf_app_pause_current(void)
{
    sf_app_ctx_t *cur = sf_app_get_current();
    if (!cur) return ESP_ERR_NOT_FOUND;
    if (cur->state != SF_APP_RESUMED) return ESP_ERR_INVALID_STATE;
    do_transition(cur, SF_APP_PAUSED);
    return ESP_OK;
}

void sf_app_foreach(bool (*cb)(sf_app_ctx_t *ctx, void *arg), void *arg)
{
    for (size_t i = 0; i < s_app_count; i++) {
        if (!cb(&s_apps[i], arg)) break;
    }
}

