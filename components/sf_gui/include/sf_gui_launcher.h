#pragma once

#include "lvgl.h"

typedef void (*sf_gui_launcher_app_cb_t)(const char *app_id);

lv_obj_t *sf_gui_launcher_create(lv_obj_t *parent);
void sf_gui_launcher_refresh(lv_obj_t *grid);
