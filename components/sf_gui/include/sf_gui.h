#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Shell ───────────────────────────────────────── */

esp_err_t sf_gui_init(void);

#if CONFIG_SF_GUI_SHELL_PHONE
esp_err_t sf_gui_phone_shell_init(void);
#endif

/* ── App Root ────────────────────────────────────── */

struct sf_app_ctx_t;

/** Get the App-specific UI root container; an App uses this in on_create to obtain its root */
lv_obj_t *sf_gui_app_get_root(struct sf_app_ctx_t *ctx);

/* ── Status Bar ──────────────────────────────────── */

void sf_gui_status_set_time(const char *time_str);
void sf_gui_status_set_battery(uint8_t percent, bool charging);
/* percent=0~100 means battery present; percent>100 means no battery (AC powered) */

lv_obj_t *sf_gui_status_bar_create(lv_obj_t *parent);

/* ── Nav Bar ─────────────────────────────────────── */

typedef void (*sf_gui_nav_cb_t)(void);

lv_obj_t *sf_gui_nav_bar_create(lv_obj_t *parent);

void sf_gui_nav_set_callbacks(sf_gui_nav_cb_t back,
                              sf_gui_nav_cb_t home,
                              sf_gui_nav_cb_t recents);

/* ── Shell Home ───────────────────────────────────── */

void sf_gui_shell_go_home(void);

#ifdef __cplusplus
}
#endif
