#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

esp_err_t sf_gui_phone_shell_init(void);
void sf_gui_phone_bars_create(lv_obj_t *parent);
lv_obj_t *sf_gui_phone_get_status_bar(void);
lv_obj_t *sf_gui_phone_get_nav_bar(void);
void sf_gui_phone_hide_bars(void);
void sf_gui_phone_show_bars(void);
bool sf_gui_phone_bars_are_visible(void);
