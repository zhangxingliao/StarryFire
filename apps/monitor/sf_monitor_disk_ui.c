/**
 * sf_monitor_disk_ui.c — Storage page UI (Windows "This PC" style)
 *
 * One block per volume (vertical flex), top to bottom:
 *   1. Volume name (own row, left-aligned)
 *   2. Horizontal usage bar (18px thick; track = remaining in C_SEP gray, fill = used in C_ACCENT purple)
 *   3. Used row: "Used X% · capacity" (percentage and amount on the same line)
 *   4. Available row: "Available capacity" (the remaining amount on its own line)
 *   5. Total row: "Total capacity"
 * The bottom of the card has one row of color-legend labels: ■ Used (purple)  □ Free (gray).
 * Volumes stack vertically (SF_MONITOR_MAX_DISK); currently there is only SPIFFS.
 *
 * The horizontal bars use the same approach as the Regions page memory bars (already verified, stack-safe):
 * lv_bar with track/indicator two colors.
 *
 * Data comes from sf_monitor_collect_disk() (disk_data.c collects the SPIFFS volume).
 * Calling convention: build / refresh are invoked by the integration shell while it holds the lvgl_port_lock.
 */
#include "sf_monitor_ui.h"
#include "sf_monitor_core.h"
#include "esp_log.h"
#include "lvgl.h"
#include "sf_theme.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "sf_monitor_disk_ui";

/* bytes -> "X.XX GB" / "X.X MB" / "XXX KB" / "XXX B" — mirrors the PC-side capacity formatting */
static void fmt_size(uint64_t bytes, char *buf, size_t n)
{
    if (bytes >= 1024ULL * 1024 * 1024) {
        snprintf(buf, n, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024 * 1024) {
        snprintf(buf, n, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        snprintf(buf, n, "%u KB", (unsigned)(bytes / 1024));
    } else {
        snprintf(buf, n, "%u B", (unsigned)bytes);
    }
}

/* A small legend item of color swatch + text (e.g. ■ Used); the swatch uses the passed-in color */
static lv_obj_t *make_legend_item(lv_obj_t *parent, const lv_style_t *swatch_style, const char *text)
{
    lv_obj_t *item = lv_obj_create(parent);
    lv_obj_remove_style_all(item);
    lv_obj_set_width(item, LV_SIZE_CONTENT);
    lv_obj_set_height(item, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(item, 4, 0);
    lv_obj_set_style_pad_all(item, 0, 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *sq = lv_obj_create(item);
    lv_obj_remove_style_all(sq);
    lv_obj_set_size(sq, SF_UI(12), SF_UI(12));
    lv_obj_add_style(sq, swatch_style, 0);
    lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sq, 2, 0);
    lv_obj_clear_flag(sq, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(item);
    lv_label_set_text(t, text);
    lv_obj_add_style(t, sf_theme_get_style(SF_STYLE_TXT_SECONDARY), 0);
    lv_obj_set_style_text_font(t, SF_FONT_XS, 0);
    return item;
}

/* Build the Storage page UI: one block per volume (hidden by default; shown/hidden on refresh by present) + bottom color legend */
void sf_monitor_disk_build(lv_obj_t *card, monitor_priv_t *priv)
{
    lv_obj_set_style_pad_all(card, SF_UI(12), 0);
    lv_obj_set_style_pad_row(card, SF_UI(16), 0);
    /* Allow vertical scrolling with multiple volumes (a single volume won't trigger it); hide the scrollbar to keep it clean */
    lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < SF_MONITOR_MAX_DISK; i++) {
        /* Per-volume whole container: volume name + bar + used/available/total, stacked vertically */
        lv_obj_t *block = lv_obj_create(card);
        lv_obj_remove_style_all(block);
        lv_obj_set_width(block, LV_PCT(100));
        lv_obj_set_height(block, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(block, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(block, 0, 0);
        lv_obj_set_style_pad_row(block, SF_UI(6), 0);
        lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(block, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_clear_flag(block, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(block, LV_OBJ_FLAG_HIDDEN);   /* hidden until refresh shows present ones */

        /* 1) Volume name (own row, left) */
        lv_obj_t *name = lv_label_create(block);
        lv_label_set_text(name, "-");
        lv_obj_add_style(name, sf_theme_get_style(SF_STYLE_TXT_PRIMARY), 0);
        lv_obj_set_style_text_font(name, SF_FONT_SM, 0);

        /* 2) Horizontal usage bar: track = remaining (sep) / indicator = used (accent) */
        lv_obj_t *bar = lv_bar_create(block);
        lv_obj_set_width(bar, LV_PCT(100));
        lv_obj_set_height(bar, SF_UI(18));
        lv_bar_set_range(bar, 0, 100);
        lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        lv_obj_add_style(bar, sf_theme_get_style(SF_STYLE_BG_SEPARATOR), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 0, 0);
        lv_obj_add_style(bar, sf_theme_get_style(SF_STYLE_BG_ACCENT), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);

        /* 3) Used row: percentage and amount on the same line (secondary text) */
        lv_obj_t *used = lv_label_create(block);
        lv_label_set_text(used, "-");
        lv_obj_add_style(used, sf_theme_get_style(SF_STYLE_TXT_SECONDARY), 0);
        lv_obj_set_style_text_font(used, SF_FONT_XS, 0);

        /* 4) Available row (the remaining amount on its own line) */
        lv_obj_t *avail = lv_label_create(block);
        lv_label_set_text(avail, "-");
        lv_obj_add_style(avail, sf_theme_get_style(SF_STYLE_TXT_SECONDARY), 0);
        lv_obj_set_style_text_font(avail, SF_FONT_XS, 0);

        /* 5) Total row (muted as a caption) */
        lv_obj_t *total = lv_label_create(block);
        lv_label_set_text(total, "-");
        lv_obj_add_style(total, sf_theme_get_style(SF_STYLE_TXT_MUTED), 0);
        lv_obj_set_style_text_font(total, SF_FONT_XS, 0);

        priv->disk_block[i] = block;
        priv->disk_name[i]  = name;
        priv->disk_bar[i]   = bar;
        priv->disk_used[i]  = used;
        priv->disk_avail[i] = avail;
        priv->disk_total[i] = total;
    }

    /* Bottom color legend: Used = purple / Free = gray (once, explaining the bar colors) */
    lv_obj_t *legend = lv_obj_create(card);
    lv_obj_remove_style_all(legend);
    lv_obj_set_width(legend, LV_PCT(100));
    lv_obj_set_height(legend, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(legend, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(legend, SF_UI(16), 0);
    lv_obj_set_style_pad_top(legend, SF_UI(8), 0);
    lv_obj_set_style_bg_opa(legend, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(legend, LV_OBJ_FLAG_SCROLLABLE);
    make_legend_item(legend, sf_theme_get_style(SF_STYLE_BG_ACCENT), "Used");
    make_legend_item(legend, sf_theme_get_style(SF_STYLE_BG_SEPARATOR), "Free");
    priv->disk_legend = legend;
}

/* Refresh the Storage page: update each volume's bar and text from the latest disk snapshot */
void sf_monitor_disk_refresh(monitor_priv_t *priv)
{
    if (!priv) return;

    sf_monitor_disk_snapshot_t snap;
    if (sf_monitor_collect_disk(&snap) != ESP_OK) return;

    for (int i = 0; i < SF_MONITOR_MAX_DISK; i++) {
        lv_obj_t *block = priv->disk_block[i];
        if (!block) continue;

        if (i < (int)snap.count && snap.disks[i].present) {
            const sf_monitor_disk_info_t *d = &snap.disks[i];
            uint64_t total = d->total;
            uint64_t used  = d->used;
            uint64_t free  = total > used ? total - used : 0;
            uint32_t pct   = total ? (uint32_t)(used * 100ULL / total) : 0;

            lv_bar_set_value(priv->disk_bar[i], (int)pct, LV_ANIM_ON);

            char pbuf[8], used_s[16], free_s[16], total_s[16];
            snprintf(pbuf, sizeof(pbuf), "%u%%", (unsigned)pct);
            fmt_size(used,  used_s,  sizeof(used_s));
            fmt_size(free,  free_s,  sizeof(free_s));
            fmt_size(total, total_s, sizeof(total_s));

            lv_label_set_text(priv->disk_name[i], d->name);

            char ubuf[40];
            snprintf(ubuf, sizeof(ubuf), "Used %s (%s)", pbuf, used_s);
            lv_label_set_text(priv->disk_used[i], ubuf);

            char abuf[32];
            snprintf(abuf, sizeof(abuf), "Free %s", free_s);
            lv_label_set_text(priv->disk_avail[i], abuf);

            char tbuf[32];
            snprintf(tbuf, sizeof(tbuf), "Total %s", total_s);
            lv_label_set_text(priv->disk_total[i], tbuf);

            lv_obj_clear_flag(block, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(block, LV_OBJ_FLAG_HIDDEN);
        }
    }
}
