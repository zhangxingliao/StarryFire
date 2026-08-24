/**
 * sf_monitor_ui.h — System Monitor UI shared header
 *
 * Factors out what the three tab-page (Tasks / Regions / Storage) UI files share:
 *   - design constants (colors, header height)
 *   - page-tab macros (TAB_* / TAB_COUNT)
 *   - the private context monitor_priv_t (widget pointers per page)
 *   - the build / refresh declarations for each page
 *
 * The integration shell (sf_app_monitor.c) and each *_ui.c include this header.
 * Data-layer types come from sf_monitor_core.h (already included here).
 */
#pragma once

#include "lvgl.h"
#include "sf_theme.h"
#include "sf_monitor_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Design constants (mapped to the system theme palette) ── */
#define C_BG      SF_COLOR_BG_PAGE
#define C_CARD    SF_COLOR_BG_CARD
#define C_SEP     SF_COLOR_SEPARATOR
#define C_TITLE   SF_COLOR_TEXT_MUTED
#define C_TEXT    SF_COLOR_TEXT_PRIMARY
#define C_TEXT2   SF_COLOR_TEXT_SECONDARY
#define C_ACCENT  SF_COLOR_ACCENT

#define HEADER_H  30   /* Custom header height (px); 44 reduced to ~30, more compact */

/* ── Page-tab definitions ── */
#define TAB_TASKS           0      /* Tasks page */
#define TAB_REGIONS         1      /* Regions page */
#define TAB_STORAGE         2      /* Storage page */
#define TAB_COUNT           3

/* Tab metadata (defined by the integration shell; UI files reference name/placeholder) */
typedef struct {
    const char *name;
    const char *placeholder;
} tab_def_t;

/* ── Private context (owned by the shell; each page UI accesses its widgets through it) ── */
typedef struct {
    lv_obj_t *tv;
    lv_obj_t *title;        /* Header title label (shows only the current page name) */
    lv_obj_t *dots[TAB_COUNT];  /* Bottom page indicator dots */

    /* Tasks page widgets */
    lv_obj_t *task_table;   /* The Tasks page's lv_table */
    int cur_tab;            /* Index of the active page (decides whether the UI refreshes) */
    lv_timer_t *ui_timer;   /* UI refresh timer (fires only when refreshing the visible page) */

    /* Regions page widgets */
    lv_obj_t *region_chart;                              /* DRAM usage-trend line chart */
    lv_chart_series_t *region_ser;                       /* Chart series (DRAM usage) */
    lv_obj_t *region_readout;                            /* Large real-time usage number at the chart's top right */
    lv_obj_t *region_rows[SF_MONITOR_MAX_REGIONS];       /* Per-region row containers */
    lv_obj_t *region_bar[SF_MONITOR_MAX_REGIONS];        /* Per-region usage bars */
    lv_obj_t *region_txt[SF_MONITOR_MAX_REGIONS];        /* Per-region names (left) */
    lv_obj_t *region_val[SF_MONITOR_MAX_REGIONS];        /* Per-region used/total (right) */

    /* Storage page widgets (Windows "This PC" style: per volume one block = name + horizontal bar + used/available/total) */
    lv_obj_t *disk_block[SF_MONITOR_MAX_DISK];           /* Per-volume whole container (for showing/hiding as one) */
    lv_obj_t *disk_name[SF_MONITOR_MAX_DISK];            /* Volume name (own row, left) */
    lv_obj_t *disk_bar[SF_MONITOR_MAX_DISK];             /* Per-volume usage bar (track=remaining C_SEP / fill=used C_ACCENT) */
    lv_obj_t *disk_used[SF_MONITOR_MAX_DISK];            /* Used row: percentage with amount on the same line */
    lv_obj_t *disk_avail[SF_MONITOR_MAX_DISK];           /* Available row (remaining amount on its own line) */
    lv_obj_t *disk_total[SF_MONITOR_MAX_DISK];           /* Total row */
    lv_obj_t *disk_legend;                               /* Bottom color legend (once: ■ used / □ available) */
} monitor_priv_t;

/* ── Build / refresh interfaces for each page ──
 * build: create this page's widgets inside the card container, stashing key object pointers back in priv.
 * refresh: update this page's widgets from the latest snapshot (the shell calls it only when the page is visible). */

/* Tasks page (lv_table) */
void sf_monitor_tasks_build(lv_obj_t *card, monitor_priv_t *priv);
void sf_monitor_tasks_refresh(monitor_priv_t *priv);

/* Regions page (lv_chart line + lv_bar usage bars) */
void sf_monitor_mem_build(lv_obj_t *card, monitor_priv_t *priv);
void sf_monitor_mem_refresh(monitor_priv_t *priv);
/* Clear the line-chart history (called when entering the app / switching to the Regions page; show realtime data only) */
void sf_monitor_mem_reset_chart(monitor_priv_t *priv);

/* Storage page (Windows style: lv_bar horizontal usage bars) */
void sf_monitor_disk_build(lv_obj_t *card, monitor_priv_t *priv);
void sf_monitor_disk_refresh(monitor_priv_t *priv);

#ifdef __cplusplus
}
#endif
