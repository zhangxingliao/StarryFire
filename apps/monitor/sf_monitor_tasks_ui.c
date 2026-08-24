/**
 * sf_monitor_tasks_ui.c — Tasks page UI (fixed header row TASK/ST/CPU/STK + lv_table rendering of [Task | State | CPU% | Stack])
 *
 * Handles only this page's widget building (sf_monitor_tasks_build) and data refresh
 * (sf_monitor_tasks_refresh). Data is taken from sf_monitor_collect_tasks() in sf_monitor_core.h
 * (a shared snapshot; no collection happens in the UI).
 *
 * Calling convention: build / refresh are invoked by the integration shell while it already holds
 * the lvgl_port_lock; this file takes no further locks (same behavior as the original on_create / refresh_cb).
 */
#include "sf_monitor_ui.h"
#include "sf_monitor_core.h"
#include "esp_log.h"
#include "lvgl.h"
#include "sf_theme.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "sf_monitor_tasks_ui";

/* Tasks page (lv_table) layout: 4 columns [Task | State | CPU% | Stack], with a fixed header row TASK/ST/CPU/STK above */
#define TASK_COLS           4
#define TASK_SORT_THRESHOLD 10     /* sort by CPU% descending once the task count exceeds this value */
#define COL_TASK   0
#define COL_STATE  1
#define COL_CPU    2
#define COL_STACK  3
/* Column widths total 240 = screen width, so the table fills the page card horizontally (card has no padding) */
#define W_TASK  80    /* Task-name column: takes the 8px given up by STACK; overlong names are cropped via LV_TABLE_CELL_CTRL_TEXT_CROP */
#define W_STATE  56    /* State column */
#define W_CPU    54    /* CPU column widened by 4px to keep "100%" from wrapping */
#define W_STACK  50    /* Gives up 4px to CPU; the four columns sum to 240 (screen width) */

/* State abbreviations: RUN / BLK / RDY / SUS (running / blocked / ready / suspended) */
static const char *state_abbr(sf_monitor_task_state_t s)
{
    switch (s) {
        case SF_TASK_STATE_RUNNING:   return "RUN";
        case SF_TASK_STATE_BLOCKED:   return "BLK";
        case SF_TASK_STATE_READY:     return "RDY";
        case SF_TASK_STATE_SUSPENDED: return "SUS";
        case SF_TASK_STATE_DELETED:   return "DEL";
        default:                      return "???";
    }
}

/* Remaining-stack bytes -> compact string (>=1K uses K, i.e. KB; <1K drops the unit, showing only the number) */
static void fmt_stack(uint32_t bytes, char *buf, size_t n)
{
    if (bytes >= 1024) {
        int kb = (int)(bytes / 1024);
        snprintf(buf, n, "%dK", kb);
    } else {
        snprintf(buf, n, "%u", (unsigned)bytes);
    }
}

/* Sort by CPU% descending (bubble sort; few tasks, so it is enough) */
static void sort_by_cpu_desc(sf_monitor_task_info_t *a, uint32_t n)
{
    for (uint32_t i = 0; i + 1 < n; i++) {
        for (uint32_t j = 0; j + 1 < n - i; j++) {
            if (a[j].cpu_percent < a[j + 1].cpu_percent) {
                sf_monitor_task_info_t t = a[j];
                a[j] = a[j + 1];
                a[j + 1] = t;
            }
        }
    }
}

/* Build the Tasks page UI: create an lv_table in card, style it, stash key objects in priv */
void sf_monitor_tasks_build(lv_obj_t *card, monitor_priv_t *priv)
{
    /* ── Column header bar (fixed above the scrolling table) ──
     * Concise labels: TASK / ST / CPU / STK — widths mirror the table columns
     * (W_TASK/W_STATE/W_CPU/W_STACK) so the header lines up with the cells. */
    static const char *s_hdr[4] = { "TASK", "ST", "CPU", "STK" };
    uint16_t s_hw[4] = { W_TASK, W_STATE, W_CPU, W_STACK };

    lv_obj_t *hdr = lv_obj_create(card);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_width(hdr, LV_PCT(100));
    lv_obj_set_height(hdr, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(hdr, 0, 0);
    lv_obj_set_style_pad_right(hdr, 0, 0);
    lv_obj_set_style_pad_top(hdr, SF_UI(4), 0);
    lv_obj_set_style_pad_bottom(hdr, SF_UI(4), 0);
    lv_obj_set_style_pad_column(hdr, 2, 0);   /* match table's inter-column gap */
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    for (int c = 0; c < 4; c++) {
        lv_obj_t *l = lv_label_create(hdr);
        lv_label_set_text(l, s_hdr[c]);
        lv_obj_set_width(l, s_hw[c]);
        lv_obj_set_style_pad_left(l, SF_UI(8), 0);    /* match table cell pad_left so labels align with cells */
        lv_obj_add_style(l, sf_theme_get_style(SF_STYLE_TXT_MUTED), 0);
        lv_obj_set_style_text_font(l, SF_FONT_XS, 0);
    }

    /* 1px separator under the header */
    lv_obj_t *hsep = lv_obj_create(card);
    lv_obj_remove_style_all(hsep);
    lv_obj_set_width(hsep, LV_PCT(100));
    lv_obj_set_height(hsep, 1);
    lv_obj_add_style(hsep, sf_theme_get_style(SF_STYLE_BG_SEPARATOR), 0);
    lv_obj_set_style_bg_opa(hsep, LV_OPA_COVER, 0);
    lv_obj_clear_flag(hsep, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *table = lv_table_create(card);
    lv_obj_set_width(table, LV_PCT(100));
    lv_obj_set_flex_grow(table, 1);
    lv_obj_set_style_bg_opa(table, LV_OPA_COVER, 0);
    lv_obj_add_style(table, sf_theme_get_style(SF_STYLE_BG_PAGE), 0);
    lv_obj_add_style(table, sf_theme_get_style(SF_STYLE_TXT_PRIMARY), 0);
    lv_obj_add_style(table, sf_theme_get_style(SF_STYLE_TXT_PRIMARY), LV_PART_ITEMS);  /* Cell text white (the default theme is black) */
    lv_obj_set_style_text_font(table, SF_FONT_XS, 0);
    lv_obj_set_style_pad_row(table, 4, 0);                   /* smaller top/bottom padding for cell text */
    lv_obj_set_style_pad_column(table, 2, 0);                /* right padding for cell text (left/right unified at 2) */
    lv_obj_set_style_pad_left(table, 8, LV_PART_ITEMS);      /* 8px left padding inside cells */
    lv_obj_set_style_radius(table, 0, 0);                    /* table has no rounded corners */
    lv_obj_set_style_border_width(table, 0, 0);             /* clear the default theme's white border */
    lv_obj_set_scrollbar_mode(table, LV_SCROLLBAR_MODE_OFF);  /* no scrollbar shown */
    lv_obj_set_scroll_dir(table, LV_DIR_VER);                /* vertical scrolling only; no horizontal drag */
    /* Cells (LV_PART_ITEMS): dark background to avoid the default white fill, with no borders or dividers */
    lv_obj_set_style_bg_opa(table, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_add_style(table, sf_theme_get_style(SF_STYLE_BG_PAGE), LV_PART_ITEMS);  /* same color as the top title bar */
    /* Remove touch/focus feedback: override the styles of every interaction state
       (pressed/focused/keyboard-focused/edited) so they match the rest state (dark background,
       no borders/dividers) exactly, avoiding a highlight ring when touching a cell.
       Also clear CLICK_FOCUSABLE to prevent focus outlines at the source; vertical scrolling is
       preserved via scroll_dir + SCROLLABLE. */
    lv_obj_clear_flag(table, LV_OBJ_FLAG_CLICK_FOCUSABLE);

    uint32_t cell_states[] = {
        LV_STATE_PRESSED, LV_STATE_FOCUSED, LV_STATE_FOCUS_KEY, LV_STATE_EDITED
    };
    for (int s = 0; s < (int)(sizeof(cell_states) / sizeof(cell_states[0])); s++) {
        uint32_t ps = LV_PART_ITEMS | cell_states[s];
        lv_obj_set_style_bg_opa(table, LV_OPA_COVER, ps);
        lv_obj_add_style(table, sf_theme_get_style(SF_STYLE_BG_PAGE), ps);  /* same color as the top title bar to avoid flashing the old color when touched */
        lv_obj_set_style_recolor_opa(table, 0, ps);            /* key: drop the default theme's 35% black darkening */
        lv_obj_add_style(table, sf_theme_get_style(SF_STYLE_TXT_PRIMARY), ps);
        lv_obj_set_style_outline_width(table, 0, ps);
    }

    /* Main-part interaction states: clear outline + clear recolor (avoid darkening the whole table when pressed) */
    uint32_t main_states[] = {
        LV_STATE_PRESSED, LV_STATE_FOCUSED, LV_STATE_FOCUS_KEY, LV_STATE_EDITED
    };
    for (int s = 0; s < (int)(sizeof(main_states) / sizeof(main_states[0])); s++) {
        lv_obj_set_style_outline_width(table, 0, main_states[s]);
        lv_obj_set_style_recolor_opa(table, 0, main_states[s]);
    }
    lv_table_set_column_count(table, TASK_COLS);
    lv_table_set_column_width(table, COL_TASK,  W_TASK);
    lv_table_set_column_width(table, COL_STATE, W_STATE);
    lv_table_set_column_width(table, COL_CPU,   W_CPU);
    lv_table_set_column_width(table, COL_STACK, W_STACK);
    lv_table_set_row_count(table, 0);   /* no header rows; data is filled directly on refresh */
    priv->task_table = table;
}

/* Refresh the Tasks page: fill the table from the latest task snapshot */
void sf_monitor_tasks_refresh(monitor_priv_t *priv)
{
    if (!priv || !priv->task_table) return;

    sf_monitor_task_snapshot_t snap;
    if (sf_monitor_collect_tasks(&snap) != ESP_OK) return;

    /* When the task count exceeds the threshold, sort by CPU% descending so heavy tasks are easy to spot */
    if (snap.count > TASK_SORT_THRESHOLD) {
        sort_by_cpu_desc(snap.tasks, snap.count);
    }

    lv_table_set_row_count(priv->task_table, snap.count);  /* no header rows, fill directly */
    for (uint32_t i = 0; i < snap.count; i++) {
        const sf_monitor_task_info_t *ti = &snap.tasks[i];
        uint32_t r = i;
        char buf[16];

        lv_table_set_cell_value(priv->task_table, r, COL_TASK,  ti->name);
        lv_table_set_cell_value(priv->task_table, r, COL_STATE, state_abbr(ti->state));

        snprintf(buf, sizeof(buf), "%u%%", (unsigned)ti->cpu_percent);
        lv_table_set_cell_value(priv->task_table, r, COL_CPU, buf);

        fmt_stack(ti->stack_free_bytes, buf, sizeof(buf));
        lv_table_set_cell_value(priv->task_table, r, COL_STACK, buf);

        /* Task-name column is fixed width with overlong text cropped (lv_table has no per-cell scroll animation) */
        lv_table_set_cell_ctrl(priv->task_table, r, COL_TASK,
                               LV_TABLE_CELL_CTRL_TEXT_CROP);
    }
}
