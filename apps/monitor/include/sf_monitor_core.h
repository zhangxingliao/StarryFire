/**
 * sf_monitor_core.h — Public contract for the System Monitor data layer
 *
 * Decoupled from the UI: this header only describes "what the monitoring data looks like" and "what
 * interfaces the data layer exposes". The actual acquisition implementations live in the individual
 * *_data.c files (tasks / mem / disk); each data file is self-contained with a "shared snapshot +
 * mutex + collect fill (do_collect) + read (collect) + log (log)" unit.
 *
 * The UI layer (*_ui.c) only obtains data through the collect_xxx interfaces here and never touches
 * FreeRTOS / heap directly. The integration shell (sf_app_monitor.c) drives periodic collection via
 * the do_collect_xxx interfaces.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Task monitoring ─────────────────────────────────────── */
#define SF_MONITOR_MAX_TASKS        64
#define SF_MONITOR_TASK_NAME_LEN    24   /* Task-name buffer length (including '\0') */

typedef enum {
    SF_TASK_STATE_RUNNING   = 0,
    SF_TASK_STATE_READY,
    SF_TASK_STATE_BLOCKED,
    SF_TASK_STATE_SUSPENDED,
    SF_TASK_STATE_DELETED,
    SF_TASK_STATE_INVALID,
} sf_monitor_task_state_t;

typedef struct {
    char name[SF_MONITOR_TASK_NAME_LEN];  /* Task name (copied in the snapshot; independently valid) */
    sf_monitor_task_state_t state;
    uint32_t current_priority;            /* Current priority */
    uint32_t base_priority;               /* Base priority (the original value before inheritance) */
    uint32_t cpu_percent;                 /* Instantaneous CPU usage % (from a two-sample time window, 0~100; 0 on the first sample) */
    uint32_t stack_free_bytes;            /* Remaining stack bytes (high-water mark; less is more dangerous) */
} sf_monitor_task_info_t;

typedef struct {
    uint32_t count;                              /* Actual number of tasks */
    sf_monitor_task_info_t tasks[SF_MONITOR_MAX_TASKS];
} sf_monitor_task_snapshot_t;

/**
 * Collect once and fill the task snapshot (called periodically by the integration shell's collection task).
 * Internally holds its own mutex and writes into the shared snapshot; returns no data to the caller.
 * Collection must be started by the shell first (see the collection task in sf_app_monitor.c).
 */
esp_err_t sf_monitor_do_collect_tasks(void);

/**
 * Reset the instantaneous-CPU baseline (first frame reads cpu=0; a valid delta exists only next time).
 * Should be called once each time the collection thread starts.
 */
void sf_monitor_tasks_reset_baseline(void);

/**
 * Copy all current tasks from the shared snapshot into snap (maintained by the background collection task).
 * Returns ESP_OK on success; snap must be provided by the caller and non-NULL.
 * cpu_percent is the instantaneous share, based on the runtime delta between two samples / the time window.
 */
esp_err_t sf_monitor_collect_tasks(sf_monitor_task_snapshot_t *snap);

/**
 * Collect and print detailed info for all tasks via ESP_LOGI
 * (name / state / current priority / base priority / CPU% / remaining stack).
 * Logging only; creates no UI objects. Log switch: SF_MONITOR_LOG_TASKS in the matching *_data.c.
 */
void sf_monitor_log_tasks(void);

/* ── Memory-region monitoring ─────────────────────────────────── */
#define SF_MONITOR_MAX_REGIONS  8

/** Memory-region IDs (for a UI that later fetches data by ID; the logs here output by name) */
typedef enum {
    SF_REGION_DRAM   = 0,   /* Internal data RAM (main heap; MALLOC_CAP_INTERNAL|8BIT) */
    SF_REGION_SPIRAM,       /* External SPI RAM (present=false when not enabled) */
    SF_REGION_DMA,          /* DMA-capable memory (MALLOC_CAP_DMA) */
    SF_MONITOR_REGION_COUNT
} sf_monitor_region_id_t;

typedef struct {
    char    name[16];       /* Region label (e.g. "DRAM"/"SPIRAM") */
    bool    present;        /* Whether the region exists (false when SPIRAM is not enabled) */
    uint32_t total;         /* Total capacity in bytes */
    uint32_t free;          /* Free bytes */
    uint32_t largest_free;  /* Largest contiguous free block in bytes (fragmentation gauge) */
    uint32_t min_free;      /* Historical minimum free bytes (long-term leak gauge) */
} sf_monitor_region_info_t;

typedef struct {
    uint32_t count;                                  /* Actual number of regions */
    sf_monitor_region_info_t regions[SF_MONITOR_MAX_REGIONS];
} sf_monitor_region_snapshot_t;

/**
 * Collect once and fill the memory-region snapshot (called periodically by the integration shell's collection task).
 * Internally holds its own mutex and writes into the shared snapshot.
 */
esp_err_t sf_monitor_do_collect_regions(void);

/**
 * Copy the current memory-region info from the shared snapshot into snap (maintained by the background collection task).
 * Returns ESP_OK on success; snap must be provided by the caller and non-NULL.
 * Each region includes total/free/largest_free/min_free plus a present flag (false when SPIRAM is not enabled).
 */
esp_err_t sf_monitor_collect_regions(sf_monitor_region_snapshot_t *snap);

/**
 * Collect and print the usage of each memory region via ESP_LOGI
 * (DRAM / SPIRAM / DMA: total capacity / free / largest contiguous block / historical minimum free).
 * Logging only; creates no UI objects. Log switch: SF_MONITOR_LOG_REGIONS in the matching *_data.c.
 */
void sf_monitor_log_regions(void);

/* ── Disk/storage monitoring ────────────────────────────────── */
#define SF_MONITOR_MAX_DISK  4

typedef struct {
    char    name[16];       /* Mount point (e.g. "/spiffs") */
    bool    present;        /* Whether the volume exists */
    uint32_t total;         /* Total capacity in bytes */
    uint32_t used;          /* Used bytes */
} sf_monitor_disk_info_t;

typedef struct {
    uint32_t count;                              /* Actual number of volumes */
    sf_monitor_disk_info_t disks[SF_MONITOR_MAX_DISK];
} sf_monitor_disk_snapshot_t;

/**
 * Collect once and fill the disk snapshot (called periodically by the integration shell's collection task).
 * Currently collects the SPIFFS volume (partition label "spiffs"); writes the shared snapshot under the lock.
 */
esp_err_t sf_monitor_do_collect_disk(void);

/**
 * Copy the current disk info from the shared snapshot into snap (maintained by the background collection task).
 * Returns ESP_OK on success; snap must be provided by the caller and non-NULL.
 * Each volume includes name/present/total/used; present=false when SPIFFS is not mounted.
 */
esp_err_t sf_monitor_collect_disk(sf_monitor_disk_snapshot_t *snap);

/**
 * Collect and print disk info (per volume: mount point / total capacity / used / usage% / free).
 * Logging only; creates no UI objects. Log switch: SF_MONITOR_LOG_DISK in the matching *_data.c.
 */
void sf_monitor_log_disk(void);

#ifdef __cplusplus
}
#endif
