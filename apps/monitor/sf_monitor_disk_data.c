/**
 * sf_monitor_disk_data.c — Disk/storage monitoring data acquisition layer
 *
 * Self-contained: holds its own shared snapshot s_disk_snap and mutex s_disk_mutex.
 *   - sf_monitor_do_collect_disk(): called periodically by the integration shell's collection task;
 *     fills the snapshot under the lock.
 *   - sf_monitor_collect_disk(): the UI layer reads the snapshot (never touches SPIFFS details).
 *   - sf_monitor_log_disk(): debug logging (gated by the SF_MONITOR_LOG_DISK macro).
 *
 * Currently collects the SPIFFS volume (partition label "spiffs", mounted at "/spiffs"); to add
 * more volumes later, just append entries in do_collect (up to SF_MONITOR_MAX_DISK).
 *
 * Has no direct dependency on any LVGL / UI types.
 */
#include "sf_monitor_core.h"

#include "esp_log.h"
#include "esp_spiffs.h"          /* esp_spiffs_info() */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>

static const char *TAG = "sf_monitor_disk";

/* Disk/storage volume logging switch:
 *   1 = the background collection task prints the capacity info of each volume (e.g. SPIFFS) each cycle;
 *   0 = off. Independent of the memory/task switches. */
#ifndef SF_MONITOR_LOG_DISK
#define SF_MONITOR_LOG_DISK  0
#endif

/* ---------- Shared snapshot + mutex (owned by this module) ---------- */
static sf_monitor_disk_snapshot_t s_disk_snap;
static SemaphoreHandle_t s_disk_mutex = NULL;

/* Single collection: fill in the mounted volumes (currently only SPIFFS, partition label "spiffs", mounted at "/spiffs") */
static void do_collect(sf_monitor_disk_snapshot_t *snap)
{
    memset(snap, 0, sizeof(*snap));
    snap->count = 0;

    if (snap->count < SF_MONITOR_MAX_DISK) {
        sf_monitor_disk_info_t *d = &snap->disks[snap->count];
        strncpy(d->name, "/spiffs", sizeof(d->name) - 1);
        d->name[sizeof(d->name) - 1] = '\0';

        size_t total = 0, used = 0;
        esp_err_t r = esp_spiffs_info("spiffs", &total, &used);
        d->present = (r == ESP_OK && total > 0);
        d->total   = (uint32_t)total;
        d->used    = (uint32_t)used;
        snap->count++;
    }
}

/* ---------- Public interfaces ---------- */

esp_err_t sf_monitor_do_collect_disk(void)
{
    if (!s_disk_mutex) s_disk_mutex = xSemaphoreCreateMutex();
    if (!s_disk_mutex) return ESP_ERR_NO_MEM;

    xSemaphoreTake(s_disk_mutex, portMAX_DELAY);
    do_collect(&s_disk_snap);
    xSemaphoreGive(s_disk_mutex);
    return ESP_OK;
}

esp_err_t sf_monitor_collect_disk(sf_monitor_disk_snapshot_t *snap)
{
    if (!snap) return ESP_ERR_INVALID_ARG;
    if (!s_disk_mutex) s_disk_mutex = xSemaphoreCreateMutex();
    if (!s_disk_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_disk_mutex, portMAX_DELAY);
    *snap = s_disk_snap;       /* whole-struct copy */
    xSemaphoreGive(s_disk_mutex);
    return ESP_OK;
}

void sf_monitor_log_disk(void)
{
#if SF_MONITOR_LOG_DISK
    if (!s_disk_mutex) { ESP_LOGW(TAG, "not started"); return; }

    /* Walk and print the shared snapshot directly under the lock, avoiding copying the whole snapshot
     * struct to the stack, which could overflow it */
    xSemaphoreTake(s_disk_mutex, portMAX_DELAY);
    ESP_LOGI(TAG, "=== disks: %u ===", (unsigned)s_disk_snap.count);
    for (uint32_t i = 0; i < s_disk_snap.count; i++) {
        const sf_monitor_disk_info_t *d = &s_disk_snap.disks[i];
        if (!d->present) {
            ESP_LOGI(TAG, "  %-9s [absent]", d->name);
            continue;
        }
        uint32_t free     = d->total - d->used;
        uint32_t used_pct = d->total ? (d->used * 100U) / d->total : 0;
        ESP_LOGI(TAG, "  %-9s total=%u used=%u (%u%%) free=%u",
                 d->name, (unsigned)d->total, (unsigned)d->used,
                 (unsigned)used_pct, (unsigned)free);
    }
    xSemaphoreGive(s_disk_mutex);
#endif  /* SF_MONITOR_LOG_DISK */
}
