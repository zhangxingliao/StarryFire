#pragma once

#include "lvgl.h"

typedef struct settings_ctx settings_ctx_t;

struct settings_ctx {
    lv_obj_t *window;
    lv_obj_t *main_page;
    lv_obj_t *cur_page;
};

void settings_show_main(settings_ctx_t *ctx);
lv_obj_t *settings_page_create(lv_obj_t *parent);

/* ── Shared UI utility functions ─────────────────────────────── */

/** Create a full-width separator line (1px) */
lv_obj_t *settings_create_separator(lv_obj_t *parent);

/** Create a category card (title + rounded container); returns the card container */
lv_obj_t *settings_create_category(lv_obj_t *parent, const char *title);

/** Create an in-card separator line (indented to align after the icon) */
lv_obj_t *settings_create_item_sep(lv_obj_t *parent);

lv_obj_t *sf_settings_wifi_create(lv_obj_t *parent, settings_ctx_t *ctx);
lv_obj_t *sf_settings_bt_create(lv_obj_t *parent, settings_ctx_t *ctx);
lv_obj_t *sf_settings_local_create(lv_obj_t *parent, settings_ctx_t *ctx);
lv_obj_t *sf_settings_device_create(lv_obj_t *parent, settings_ctx_t *ctx);

/** Dismiss the open Wi-Fi password sheet, if any. Returns true if a sheet was
 *  closed. Used by the app-level back handler so the system back gesture returns
 *  to the Wi-Fi list instead of jumping to the home page. */
bool sf_settings_wifi_dismiss_sheet(void);
