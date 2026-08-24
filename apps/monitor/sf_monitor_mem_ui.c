/**
 * sf_monitor_mem_ui.c — Regions page UI (lv_chart line + lv_bar usage bars)
 *
 * Top: a line chart of DRAM usage % over time (0..100, up to MEM_CHART_POINTS
 *      points, no dots, cleared on app enter / tab switch to show only live data).
 * Bottom: per-region usage bars (DRAM / SPIRAM / DMA), bottom-aligned.
 * Data from sf_monitor_collect_regions.
 *
 * Calling convention: build / refresh are invoked by the integration shell within
 * an already lvgl_port_lock-held context; this file does NOT lock again.
 */
#include "sf_monitor_ui.h"
#include "sf_monitor_core.h"
#include "esp_log.h"
#include "lvgl.h"
#include "sf_theme.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "sf_monitor_mem_ui";

#define MEM_CHART_POINTS   100   /* max points kept in the line chart buffer */
#define MEM_CHART_ENABLED  1    /* 1 = draw DRAM-usage line chart; taskLVGL stack raised to 16KB so the chart draw is safe */

/* bytes -> compact size string (>=1M -> M, >=1K -> K, else B) */
static void fmt_mem(uint32_t bytes, char *buf, size_t n)
{
    if (bytes >= 1024 * 1024) {
        snprintf(buf, n, "%.1fM", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        snprintf(buf, n, "%uK", (unsigned)(bytes / 1024));
    } else {
        snprintf(buf, n, "%uB", (unsigned)bytes);
    }
}

/* Clear chart history so only live data is shown. Called on app enter / tab switch. */
void sf_monitor_mem_reset_chart(monitor_priv_t *priv)
{
    if (!priv || !priv->region_chart || !priv->region_ser) return;
    lv_chart_set_all_values(priv->region_chart, priv->region_ser, LV_CHART_POINT_NONE);
    if (priv->region_readout) lv_label_set_text(priv->region_readout, "--%");
}

/* Build Regions page UI: expanded line chart (top) + usage bars (bottom). */
void sf_monitor_mem_build(lv_obj_t *card, monitor_priv_t *priv)
{
    lv_obj_set_style_pad_row(card, SF_UI(10), 0);   /* gap between chart section and bars section */

#if MEM_CHART_ENABLED
    /* ── Chart section (expanded) ── */
    lv_obj_t *chart_box = lv_obj_create(card);
    lv_obj_remove_style_all(chart_box);
    lv_obj_set_width(chart_box, LV_PCT(100));
    lv_obj_set_flex_grow(chart_box, 1);              /* take all spare height -> chart expanded */
    /* Low min floor: the card (~226px tall) must hold both the chart and the bars
       list (~130px). A high min_height here pushes the bottom (DMA) bar off-screen.
       Keep a small floor so the chart never vanishes; it still grows to fill. */
    lv_obj_set_style_min_height(chart_box, 40, 0);
    lv_obj_add_style(chart_box, sf_theme_get_style(SF_STYLE_BG_PAGE), 0);
    lv_obj_set_style_bg_opa(chart_box, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(chart_box, SF_UI(10), 0);
    lv_obj_set_flex_flow(chart_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(chart_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(chart_box, LV_OBJ_FLAG_SCROLLABLE);

    /* title row: name (left) + live readout (right) */
    lv_obj_t *title_row = lv_obj_create(chart_box);
    lv_obj_remove_style_all(title_row);
    lv_obj_set_width(title_row, LV_PCT(100));
    lv_obj_set_height(title_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(title_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(title_row, 0, 0);
    lv_obj_clear_flag(title_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *chart_title = lv_label_create(title_row);
    lv_label_set_text(chart_title, "DRAM Usage");
    lv_obj_add_style(chart_title, sf_theme_get_style(SF_STYLE_TXT_MUTED), 0);
    lv_obj_set_style_text_font(chart_title, SF_FONT_XS, 0);

    lv_obj_t *readout = lv_label_create(title_row);
    lv_label_set_text(readout, "--%");
    lv_obj_add_style(readout, sf_theme_get_style(SF_STYLE_TXT_ACCENT), 0);
    lv_obj_set_style_text_font(readout, SF_FONT_LG, 0);
    priv->region_readout = readout;

    /* chart */
    lv_obj_t *chart = lv_chart_create(chart_box);
    lv_obj_set_width(chart, LV_PCT(100));
    lv_obj_set_flex_grow(chart, 1);
    lv_obj_set_style_radius(chart, 0, 0);
    lv_obj_set_style_bg_opa(chart, LV_OPA_COVER, 0);
    lv_obj_add_style(chart, sf_theme_get_style(SF_STYLE_BG_PAGE), 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_scrollbar_mode(chart, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_left(chart, SF_UI(6), 0);
    lv_obj_set_style_pad_right(chart, SF_UI(6), 0);
    lv_obj_set_style_pad_top(chart, SF_UI(6), 0);
    lv_obj_set_style_pad_bottom(chart, SF_UI(6), 0);
    /* No clip_corner: radius!=0 forces LVGL to allocate off-screen layers and
       exhaust the 64KB LV_MEM (same root cause as the earlier Wi-Fi scan crash). */
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, MEM_CHART_POINTS);
    lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(chart, 5, 0);   /* 5 horizontal grid lines */
    /* grid lines use the LV_PART_MAIN line style */
    lv_obj_add_style(chart, sf_theme_get_style(SF_STYLE_LINE_SEP), LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 1, LV_PART_MAIN);
    lv_obj_set_style_line_opa(chart, LV_OPA_40, LV_PART_MAIN);
    /* the line itself uses the LV_PART_ITEMS line style */
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    /* rounded caps look nicer (PC/phone-style); requires enough taskLVGL stack */
    lv_obj_set_style_line_rounded(chart, true, LV_PART_ITEMS);
    /* no point bullets: indicator size 0 -> bullet_w/h = 0 -> points skipped */
    lv_obj_set_style_width(chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(chart, 0, LV_PART_INDICATOR);

    priv->region_chart = chart;
    priv->region_ser = lv_chart_add_series(chart, C_ACCENT, LV_CHART_AXIS_PRIMARY_Y);
    sf_monitor_mem_reset_chart(priv);   /* start empty (no history) */
#else
    /* chart disabled — shield the whole chart section (title + live readout +
       lv_chart) so LVGL never draws the line chart (root cause of the taskLVGL
       stack overflow). The usage bars below are still built and refreshed.
       Set the pointers NULL so refresh() skips the chart draw paths. */
    priv->region_chart   = NULL;
    priv->region_ser     = NULL;
    priv->region_readout = NULL;
    lv_obj_set_style_pad_row(card, 0, 0);   /* single section now -> drop the inter-section gap */
#endif

    /* ── Bars section ── */
    lv_obj_t *list = lv_obj_create(card);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_height(list, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_hor(list, SF_UI(12), 0);
    lv_obj_set_style_pad_top(list, SF_UI(8), 0);     /* top margin above the first bar */
    lv_obj_set_style_pad_bottom(list, SF_UI(14), 0); /* bottom margin below the last (DMA) bar so it isn't clipped */
    lv_obj_set_style_pad_row(list, SF_UI(12), 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
#if MEM_CHART_ENABLED
    /* chart above takes the spare height; bars sit at the bottom */
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
#else
    /* no chart above: let the bars fill the card and sit at the bottom */
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
#endif
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    for (int r = 0; r < SF_MONITOR_MAX_REGIONS; r++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);   /* hidden until refresh shows present ones */

        /* head: region name (left) + used/total (right) */
        lv_obj_t *head = lv_obj_create(row);
        lv_obj_remove_style_all(head);
        lv_obj_set_width(head, LV_PCT(100));
        lv_obj_set_height(head, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
        lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(head, 0, 0);
        lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *name = lv_label_create(head);
        lv_label_set_text(name, "-");
        lv_obj_add_style(name, sf_theme_get_style(SF_STYLE_TXT_PRIMARY), 0);
        lv_obj_set_style_text_font(name, SF_FONT_XS, 0);

        lv_obj_t *val = lv_label_create(head);
        lv_label_set_text(val, "-");
        lv_obj_add_style(val, sf_theme_get_style(SF_STYLE_TXT_SECONDARY), 0);
        lv_obj_set_style_text_font(val, SF_FONT_XS, 0);

        /* full-width usage bar below the head */
        lv_obj_t *bar = lv_bar_create(row);
        lv_obj_set_width(bar, LV_PCT(100));
        lv_obj_set_height(bar, SF_UI(10));
        lv_obj_set_style_pad_top(head, 0, 0);
        lv_obj_set_style_pad_bottom(name, SF_UI(6), 0);   /* gap between head and bar */
        lv_bar_set_range(bar, 0, 100);
        lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        lv_obj_add_style(bar, sf_theme_get_style(SF_STYLE_BG_SEPARATOR), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 0, 0);
        lv_obj_add_style(bar, sf_theme_get_style(SF_STYLE_BG_ACCENT), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);

        priv->region_rows[r] = row;
        priv->region_bar[r]  = bar;
        priv->region_txt[r]  = name;
        priv->region_val[r]  = val;
    }
}

/* Refresh Regions page: advance the DRAM usage line (if chart enabled) + update the usage bars. */
void sf_monitor_mem_refresh(monitor_priv_t *priv)
{
    if (!priv) return;
    sf_monitor_region_snapshot_t snap;
    if (sf_monitor_collect_regions(&snap) != ESP_OK) return;

    /* DRAM usage % drives both the line chart and the live readout */
    int dram_pct = 0;
    bool have_dram = false;
    for (uint32_t i = 0; i < snap.count; i++) {
        if (strcmp(snap.regions[i].name, "DRAM") == 0 && snap.regions[i].present) {
            uint32_t total = snap.regions[i].total;
            uint32_t used  = total > snap.regions[i].free ? total - snap.regions[i].free : 0;
            dram_pct   = total ? (int)(used * 100U / total) : 0;
            have_dram  = true;
            break;
        }
    }
    if (have_dram) {
        if (priv->region_chart && priv->region_ser) {
            lv_chart_set_series_color(priv->region_chart, priv->region_ser, C_ACCENT);
            lv_chart_set_next_value(priv->region_chart, priv->region_ser, dram_pct);
        }
        if (priv->region_readout) {
            char rbuf[8];
            snprintf(rbuf, sizeof(rbuf), "%d%%", dram_pct);
            lv_label_set_text(priv->region_readout, rbuf);
        }
    }

    /* bars: show present regions, hide absent ones (independent of the chart) */
    char buf[64], used_s[12], total_s[12];
    for (int i = 0; i < SF_MONITOR_MAX_REGIONS; i++) {
        lv_obj_t *row = priv->region_rows[i];
        if (!row) continue;
        if (i < (int)snap.count && snap.regions[i].present) {
            uint32_t total = snap.regions[i].total;
            uint32_t free  = snap.regions[i].free;
            uint32_t used  = total > free ? total - free : 0;
            uint32_t pct   = total ? used * 100U / total : 0;
            lv_bar_set_value(priv->region_bar[i], (int)pct, LV_ANIM_ON);
            lv_label_set_text(priv->region_txt[i], snap.regions[i].name);
            fmt_mem(used,  used_s,  sizeof(used_s));
            fmt_mem(total, total_s, sizeof(total_s));
            snprintf(buf, sizeof(buf), "%s / %s", used_s, total_s);
            lv_label_set_text(priv->region_val[i], buf);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        }
    }
}
