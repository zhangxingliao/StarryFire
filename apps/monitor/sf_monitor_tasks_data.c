/**
 * sf_monitor_tasks_data.c — Task monitoring data acquisition
 *
 * Self-contained: holds its own shared snapshot s_tasks_snap and mutex s_tasks_mutex.
 *   - sf_monitor_do_collect_tasks(): called periodically by the integration shell's collection task;
 *     fills the snapshot under the lock. It computes an instantaneous CPU% from the
 *     (runtime delta / time window) between two samples, which is more meaningful than a cumulative value.
 *   - sf_monitor_collect_tasks(): the UI layer reads the snapshot (never touches FreeRTOS).
 *   - sf_monitor_log_tasks(): debug logging.
 *
 * Has no direct dependency on any LVGL / UI types.
 */
#include "sf_monitor_core.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>

static const char *TAG = "sf_monitor_tasks";

/* Task-detail logging switch:
 *   1 = the background collection task prints all task details to the log every period; 0 = off (default).
 *   The UI already displays task info; the periodic log is for debugging only and floods the console,
 *   so it is off by default to reduce serial noise. */
#ifndef SF_MONITOR_LOG_TASKS
#define SF_MONITOR_LOG_TASKS  0
#endif

/* ---------- State mapping ---------- */
static sf_monitor_task_state_t map_state(eTaskState s)
{
    switch (s) {
        case eRunning:   return SF_TASK_STATE_RUNNING;
        case eReady:     return SF_TASK_STATE_READY;
        case eBlocked:   return SF_TASK_STATE_BLOCKED;
        case eSuspended: return SF_TASK_STATE_SUSPENDED;
        case eDeleted:   return SF_TASK_STATE_DELETED;
        default:         return SF_TASK_STATE_INVALID;
    }
}

/* Logging only; avoids -Wunused-function when SF_MONITOR_LOG_TASKS is off */
static const char *state_str(sf_monitor_task_state_t s) __attribute__((unused));
static const char *state_str(sf_monitor_task_state_t s)
{
    switch (s) {
        case SF_TASK_STATE_RUNNING:   return "RUN";
        case SF_TASK_STATE_READY:     return "RDY";
        case SF_TASK_STATE_BLOCKED:   return "BLK";
        case SF_TASK_STATE_SUSPENDED: return "SUS";
        case SF_TASK_STATE_DELETED:   return "DEL";
        default:                      return "???";
    }
}

/* ---------- Shared snapshot + mutex (owned by this module) ---------- */
static sf_monitor_task_snapshot_t s_tasks_snap;
static SemaphoreHandle_t s_tasks_mutex = NULL;

/* Instantaneous-CPU baseline: remembers the previous ulRunTimeCounter and sample time per task name */
typedef struct {
    char name[SF_MONITOR_TASK_NAME_LEN];
    uint32_t runtime;   /* Previous ulRunTimeCounter (microseconds) */
    bool valid;
} prev_rt_t;
static prev_rt_t s_prev[SF_MONITOR_MAX_TASKS];
static int64_t s_prev_time = 0;

static uint32_t prev_lookup(const char *name, bool *found)
{
    for (int i = 0; i < SF_MONITOR_MAX_TASKS; i++) {
        if (s_prev[i].valid &&
            strncmp(s_prev[i].name, name, SF_MONITOR_TASK_NAME_LEN) == 0) {
            *found = true;
            return s_prev[i].runtime;
        }
    }
    *found = false;
    return 0;
}

static void prev_update(const char *name, uint32_t runtime)
{
    int empty = -1;
    for (int i = 0; i < SF_MONITOR_MAX_TASKS; i++) {
        if (s_prev[i].valid &&
            strncmp(s_prev[i].name, name, SF_MONITOR_TASK_NAME_LEN) == 0) {
            s_prev[i].runtime = runtime;
            return;
        }
        if (empty < 0 && !s_prev[i].valid) empty = i;
    }
    if (empty >= 0) {
        strncpy(s_prev[empty].name, name, SF_MONITOR_TASK_NAME_LEN - 1);
        s_prev[empty].name[SF_MONITOR_TASK_NAME_LEN - 1] = '\0';
        s_prev[empty].runtime = runtime;
        s_prev[empty].valid = true;
    }
    /* In the extreme case the table is full with no match, this baseline is dropped (rare) */
}

/* Single collection: writes snap (the caller manages concurrency; this function does not lock) */
static void do_collect(sf_monitor_task_snapshot_t *snap)
{
    static TaskStatus_t stat[SF_MONITOR_MAX_TASKS];
    uint32_t total_run = 0;
    UBaseType_t n = uxTaskGetSystemState(stat, SF_MONITOR_MAX_TASKS, &total_run);
    int64_t now = esp_timer_get_time();   /* microseconds */

    snap->count = 0;
    for (UBaseType_t i = 0; i < n && i < SF_MONITOR_MAX_TASKS; i++) {
        const TaskStatus_t *t = &stat[i];
        sf_monitor_task_info_t *info = &snap->tasks[snap->count];

        info->state            = map_state(t->eCurrentState);
        info->current_priority = (uint32_t)t->uxCurrentPriority;
        info->base_priority    = (uint32_t)t->uxBasePriority;
        info->stack_free_bytes = (uint32_t)t->usStackHighWaterMark * sizeof(StackType_t);
        strncpy(info->name, t->pcTaskName, SF_MONITOR_TASK_NAME_LEN - 1);
        info->name[SF_MONITOR_TASK_NAME_LEN - 1] = '\0';

        /* Instantaneous CPU%: runtime delta in this window / time window (both in microseconds) */
        bool found = false;
        uint32_t prev_rt = prev_lookup(info->name, &found);
        uint32_t delta_rt = t->ulRunTimeCounter - prev_rt;  /* Unsigned subtraction handles wraparound */
        uint32_t cpu = 0;
        if (found && s_prev_time > 0 && now > s_prev_time) {
            uint64_t c = (uint64_t)delta_rt * 100ULL;
            cpu = (uint32_t)(c / (uint64_t)(now - s_prev_time));
            if (cpu > 100) cpu = 100;   /* Multi-core / counter edge cases may slightly exceed 100 */
        }
        info->cpu_percent = cpu;

        prev_update(info->name, t->ulRunTimeCounter);
        snap->count++;
    }
    s_prev_time = now;
}

/* Reset the instantaneous-CPU baseline so the first sample reads cpu=0 (a valid delta only from the next) */
static void reset_baseline(void)
{
    memset(s_prev, 0, sizeof(s_prev));
    s_prev_time = 0;
}

/* ---------- Public interfaces ---------- */

esp_err_t sf_monitor_do_collect_tasks(void)
{
    if (!s_tasks_mutex) s_tasks_mutex = xSemaphoreCreateMutex();
    if (!s_tasks_mutex) return ESP_ERR_NO_MEM;

    xSemaphoreTake(s_tasks_mutex, portMAX_DELAY);
    do_collect(&s_tasks_snap);
    xSemaphoreGive(s_tasks_mutex);
    return ESP_OK;
}

/* Reset the instantaneous-CPU baseline: call it once whenever the collection thread starts (so the
   first frame reads cpu=0 and a valid delta exists only from the next one). Called by the integration
   shell's collection-task entry point. */
void sf_monitor_tasks_reset_baseline(void)
{
    reset_baseline();
}

esp_err_t sf_monitor_collect_tasks(sf_monitor_task_snapshot_t *snap)
{
    if (!snap) return ESP_ERR_INVALID_ARG;
    if (!s_tasks_mutex) s_tasks_mutex = xSemaphoreCreateMutex();
    if (!s_tasks_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_tasks_mutex, portMAX_DELAY);
    *snap = s_tasks_snap;              /* whole-struct copy (includes the embedded arrays) */
    xSemaphoreGive(s_tasks_mutex);
    return ESP_OK;
}

void sf_monitor_log_tasks(void)
{
#if SF_MONITOR_LOG_TASKS
    if (!s_tasks_mutex) { ESP_LOGW(TAG, "not started"); return; }

    /* Walk and print the shared snapshot directly under the lock, avoiding copying the entire
     * snapshot struct (~2.8KB) to the stack, which could overflow the collection-task stack,
     * corrupt the mutex handle, and crash on an assertion. */
    xSemaphoreTake(s_tasks_mutex, portMAX_DELAY);
    ESP_LOGI(TAG, "=== tasks: %u (instant CPU) ===", (unsigned)s_tasks_snap.count);
    for (uint32_t i = 0; i < s_tasks_snap.count; i++) {
        const sf_monitor_task_info_t *t = &s_tasks_snap.tasks[i];
        ESP_LOGI(TAG, "  %-16s state=%-3s prio=%2u base=%2u cpu=%3u%% stackFree=%uB",
                 t->name, state_str(t->state),
                 (unsigned)t->current_priority, (unsigned)t->base_priority,
                 (unsigned)t->cpu_percent, (unsigned)t->stack_free_bytes);
    }
    xSemaphoreGive(s_tasks_mutex);
#endif  /* SF_MONITOR_LOG_TASKS */
}
