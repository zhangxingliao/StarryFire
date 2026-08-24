#include "sf_gui.h"
#include "sf_theme.h"
#include "sf_sys.h"
#include "esp_lvgl_port.h"

static sf_gui_nav_cb_t s_cb_back;
static sf_gui_nav_cb_t s_cb_home;

static void home_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_cb_home) s_cb_home();
}

static lv_obj_t *create_btn(lv_obj_t *parent, const char *icon, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(btn, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_height(btn, LV_PCT(100));
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_style(btn, sf_theme_get_style(SF_STYLE_BAR_BTN), 0);
    lv_obj_add_style(btn, sf_theme_get_style(SF_STYLE_BAR_BTN_PRESSED), LV_STATE_PRESSED);

    lv_obj_t *ic = lv_label_create(btn);
    lv_label_set_text(ic, icon);
    lv_obj_add_style(ic, sf_theme_get_style(SF_STYLE_TXT_PRIMARY), 0);

    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }

    return btn;
}

static void nav_default_home(void)
{
    sf_gui_shell_go_home();
}

lv_obj_t *sf_gui_nav_bar_create(lv_obj_t *parent)
{
    s_cb_back = NULL;
    s_cb_home = nav_default_home;

    lv_obj_t *nav = lv_obj_create(parent);
    lv_obj_set_width(nav, LV_PCT(100));
    lv_obj_set_height(nav, LV_SIZE_CONTENT);
    lv_obj_add_style(nav, sf_theme_get_style(SF_STYLE_TRANSP), 0);
    lv_obj_set_scrollbar_mode(nav, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *pill = lv_obj_create(nav);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(pill, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_width(pill, LV_PCT(100));
    lv_obj_set_height(pill, SF_UI(30));
    lv_obj_add_style(pill, sf_theme_get_style(SF_STYLE_NAV_PILL), 0);
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *home_btn = create_btn(pill, LV_SYMBOL_HOME, home_click_cb);
    lv_obj_set_flex_grow(home_btn, 0);
    lv_obj_set_width(home_btn, SF_UI(80));

    return nav;
}

void sf_gui_nav_set_callbacks(sf_gui_nav_cb_t back,
                              sf_gui_nav_cb_t home,
                              sf_gui_nav_cb_t recents)
{
    s_cb_back = back;
    s_cb_home = home;
    (void)recents;
}
