#include "sf_gui.h"
#include "sf_gui_phone.h"
#include "esp_lvgl_port.h"

static lv_obj_t *s_status_bar;
static lv_obj_t *s_nav_bar;
static bool s_bars_visible;

static void bar_opa_cb(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void bar_hide_ready_cb(lv_anim_t *a)
{
    lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN);
}

void sf_gui_phone_bars_create(lv_obj_t *parent)
{
    s_status_bar = sf_gui_status_bar_create(parent);
    lv_obj_clear_flag(s_status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_align(s_status_bar, LV_ALIGN_TOP_MID);

    s_nav_bar = sf_gui_nav_bar_create(parent);
    lv_obj_clear_flag(s_nav_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_align(s_nav_bar, LV_ALIGN_BOTTOM_MID);

    s_bars_visible = true;
}

void sf_gui_phone_hide_bars(void)
{
    if (!s_bars_visible) return;
    s_bars_visible = false;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_time(&a, 300);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);

    lv_anim_set_var(&a, s_status_bar);
    lv_anim_set_exec_cb(&a, bar_opa_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_ready_cb(&a, bar_hide_ready_cb);
    lv_anim_start(&a);

    lv_anim_set_var(&a, s_nav_bar);
    lv_anim_set_exec_cb(&a, bar_opa_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_ready_cb(&a, bar_hide_ready_cb);
    lv_anim_start(&a);
}

void sf_gui_phone_show_bars(void)
{
    if (s_bars_visible) return;
    s_bars_visible = true;

    lv_obj_clear_flag(s_status_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_nav_bar, LV_OBJ_FLAG_HIDDEN);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_time(&a, 300);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);

    lv_anim_set_var(&a, s_status_bar);
    lv_anim_set_exec_cb(&a, bar_opa_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_start(&a);

    lv_anim_set_var(&a, s_nav_bar);
    lv_anim_set_exec_cb(&a, bar_opa_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_start(&a);
}

bool sf_gui_phone_bars_are_visible(void)
{
    return s_bars_visible;
}

lv_obj_t *sf_gui_phone_get_status_bar(void)
{
    return s_status_bar;
}

lv_obj_t *sf_gui_phone_get_nav_bar(void)
{
    return s_nav_bar;
}
