/**
 * sf_monitor_mem_data.c — Memory-region monitoring data acquisition
 *
 * Self-contained: holds its own shared snapshot s_region_snap and mutex s_region_mutex.
 *   - sf_monitor_do_collect_regions(): called periodically by the integration shell's collection task;
 *     fills the snapshot under the lock.
 *   - sf_monitor_collect_regions(): the UI layer reads the snapshot (never touches heap details).
 *   - sf_monitor_log_regions(): debug logging.
 *
 * Has no direct dependency on any LVGL / UI types.
 */
#include "sf_monitor_core.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>

static const char *TAG = "sf_monitor_mem";

/* Memory-region detail logging switch:
 *   1 = the background collection task prints each memory region's usage to the log every cycle (on by default, handy for early debugging);
 *   0 = off. Independent of the task switch. */
#ifndef SF_MONITOR_LOG_REGIONS
#define SF_MONITOR_LOG_REGIONS  0   /* disable periodic "memory regions" log spam */
#endif

/* ---------- Shared snapshot + mutex (owned by this module) ---------- */
static sf_monitor_region_snapshot_t s_region_snap;
static SemaphoreHandle_t s_region_mutex = NULL;

/* Single collection: writes snap (the caller manages concurrency; this function does not lock) */
static void do_collect(sf_monitor_region_snapshot_t *snap)
{
    /* The heap_caps capability flags for each region; these flags are not mutually exclusive, so regions
     * can overlap (consistent with the official meminfo). IRAM is fixed instruction RAM that does not
     * change over time and has been removed from the monitoring. */
    static const struct {
        const char *name;
        uint32_t    caps;
    } defs[] = {
        { "DRAM",   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT },  /* internal data RAM (main heap) */
        { "SPIRAM", MALLOC_CAP_SPIRAM },                     /* external SPI RAM (present=false if not enabled) */
        { "DMA",    MALLOC_CAP_DMA },                        /* DMA-capable memory */
    };

    snap->count = 0;
    for (size_t i = 0; i < sizeof(defs) / sizeof(defs[0]); i++) {
        if (snap->count >= SF_MONITOR_MAX_REGIONS) break;

        sf_monitor_region_info_t *r = &snap->regions[snap->count];
        strncpy(r->name, defs[i].name, sizeof(r->name) - 1);
        r->name[sizeof(r->name) - 1] = '\0';

        uint32_t total = heap_caps_get_total_size(defs[i].caps);
        r->present = (total > 0);
        r->total        = total;
        r->free         = r->present ? heap_caps_get_free_size(defs[i].caps)        : 0;
        r->largest_free = r->present ? heap_caps_get_largest_free_block(defs[i].caps) : 0;
        r->min_free     = r->present ? heap_caps_get_minimum_free_size(defs[i].caps) : 0;
        snap->count++;
    }
}

/* ---------- Public interfaces ---------- */

esp_err_t sf_monitor_do_collect_regions(void)
{
    if (!s_region_mutex) s_region_mutex = xSemaphoreCreateMutex();
    if (!s_region_mutex) return ESP_ERR_NO_MEM;

    xSemaphoreTake(s_region_mutex, portMAX_DELAY);
    do_collect(&s_region_snap);
    xSemaphoreGive(s_region_mutex);
    return ESP_OK;
}

esp_err_t sf_monitor_collect_regions(sf_monitor_region_snapshot_t *snap)
{
    if (!snap) return ESP_ERR_INVALID_ARG;
    if (!s_region_mutex) s_region_mutex = xSemaphoreCreateMutex();
    if (!s_region_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_region_mutex, portMAX_DELAY);
    *snap = s_region_snap;       /* whole-struct copy (includes the embedded arrays) */
    xSemaphoreGive(s_region_mutex);
    return ESP_OK;
}

void sf_monitor_log_regions(void)
{
#if SF_MONITOR_LOG_REGIONS
    if (!s_region_mutex) { ESP_LOGW(TAG, "not started"); return; }

    /* Walk and print the shared snapshot directly under the lock, avoiding copying the whole snapshot
     * struct to the stack, which could overflow it */
    xSemaphoreTake(s_region_mutex, portMAX_DELAY);
    ESP_LOGI(TAG, "=== memory regions: %u ===", (unsigned)s_region_snap.count);
    for (uint32_t i = 0; i < s_region_snap.count; i++) {
        const sf_monitor_region_info_t *r = &s_region_snap.regions[i];
        if (!r->present) {
            ESP_LOGI(TAG, "  %-7s [absent]", r->name);
            continue;
        }
        uint32_t used     = r->total - r->free;
        uint32_t used_pct = r->total ? (used * 100U) / r->total : 0;
        ESP_LOGI(TAG, "  %-7s total=%u free=%u used=%u (%u%%) largest=%u minFree=%u",
                 r->name, (unsigned)r->total, (unsigned)r->free,
                 (unsigned)used, (unsigned)used_pct,
                 (unsigned)r->largest_free, (unsigned)r->min_free);
    }
    xSemaphoreGive(s_region_mutex);
#endif  /* SF_MONITOR_LOG_REGIONS */
}
