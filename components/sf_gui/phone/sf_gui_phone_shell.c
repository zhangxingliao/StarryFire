#include "sf_gui.h"
#include "sf_gui_phone.h"
#include "sf_gui_internal.h"
#include "sf_gui_launcher.h"
#include "sf_gui_notification_center.h"
#include "sf_notification.h"
#include "sf_theme.h"
#include "sf_sys.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

static lv_obj_t *s_desktop;
static lv_timer_t *s_idle_timer;
static lv_timer_t *s_gesture_timer;
static lv_point_t s_gesture_start = { -1, -1 };

/* ── App UI hooks (called by AppManager at lifecycle points) ─ */

static void gui_app_pre_create(sf_app_ctx_t *ctx)
{
    lv_obj_t *root = lv_obj_create(lv_scr_act());
    lv_obj_set_width(root, LV_PCT(100));
    lv_obj_set_height(root, LV_PCT(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    ctx->ui_root = root;
}

static void gui_app_post_destroy(sf_app_ctx_t *ctx)
{
    if (ctx->ui_root) {
        lv_obj_del((lv_obj_t *)ctx->ui_root);
        ctx->ui_root = NULL;
    }
}

static void gui_app_on_resume_show(sf_app_ctx_t *ctx)
{
    sf_gui_notification_center_hide();

    lv_obj_t *root = (lv_obj_t *)ctx->ui_root;
    if (root) {
        lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_desktop) {
        lv_obj_add_flag(s_desktop, LV_OBJ_FLAG_HIDDEN);
    }
}

static void gui_app_on_pause_hide(sf_app_ctx_t *ctx)
{
    lv_obj_t *root = (lv_obj_t *)ctx->ui_root;
    if (root) {
        lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ── Home / Back ──────────────────────────────────── */

static void phone_go_home(void)
{
    sf_gui_notification_center_hide();
    sf_app_pause_current();

    if (s_desktop) {
        lv_obj_clear_flag(s_desktop, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ── Immersive mode ───────────────────────────────── */

/*
 * Edge gesture zone: when a press starts inside an edge zone, temporarily
 * disable the scroll capability of the pressed widget's scrollable ancestor,
 * so LVGL does not consume the touch move as scrolling and the indev-level
 * GESTURE event can fire normally. Restored on release.
 */
#define EDGE_ZONE_X  24   /* left edge width (px) — swipe right to go back */
#define EDGE_ZONE_Y  24   /* top/bottom edge height (px) — swipe in/out status & nav bars */

static lv_obj_t *s_disabled_scroll_obj;   /* the object whose scroll was temporarily disabled; restored on release */
static bool s_edge_press;                  /* whether the current press started in an edge zone */

/* Walk up to the first scrollable ancestor (including self), skipping the screen */
static lv_obj_t *find_scrollable_ancestor(lv_obj_t *obj)
{
    lv_obj_t *scr = lv_scr_act();
    while (obj && obj != scr) {
        if (lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLLABLE)) return obj;
        obj = lv_obj_get_parent(obj);
    }
    return NULL;
}

static void immersive_idle_cb(lv_timer_t *t);

static void immersive_start_idle_timer(void)
{
    if (s_idle_timer) {
        lv_timer_reset(s_idle_timer);
    } else {
        s_idle_timer = lv_timer_create(immersive_idle_cb, 5000, NULL);
        lv_timer_set_repeat_count(s_idle_timer, 1);
    }
}

static void immersive_idle_cb(lv_timer_t *t)
{
    (void)t;
    s_idle_timer = NULL;
    if (sf_gui_notification_center_is_visible()) {
        immersive_start_idle_timer();
        return;
    }
    sf_gui_phone_hide_bars();
}

static void immersive_pressed_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_indev_get_point(indev, &s_gesture_start);

    lv_coord_t sh = lv_obj_get_height(lv_scr_act());

    /* Decide whether the press starts in an edge zone */
    s_edge_press = (s_gesture_start.x < EDGE_ZONE_X ||
                    s_gesture_start.y < EDGE_ZONE_Y ||
                    s_gesture_start.y > sh - EDGE_ZONE_Y);

    if (s_edge_press) {
        /* Find the pressed widget's scrollable ancestor and disable scrolling temporarily */
        lv_obj_t *pressed = lv_indev_get_active_obj();
        if (pressed) {
            s_disabled_scroll_obj = find_scrollable_ancestor(pressed);
            if (s_disabled_scroll_obj) {
                lv_obj_clear_flag(s_disabled_scroll_obj, LV_OBJ_FLAG_SCROLLABLE);
            }
        }
    }
}

static void immersive_released_cb(lv_event_t *e)
{
    (void)e;
    /* Restore the temporarily disabled scroll capability */
    if (s_disabled_scroll_obj) {
        if (lv_obj_is_valid(s_disabled_scroll_obj)) {
            lv_obj_add_flag(s_disabled_scroll_obj, LV_OBJ_FLAG_SCROLLABLE);
        }
        s_disabled_scroll_obj = NULL;
    }
    s_edge_press = false;
    s_gesture_start.x = -1;
    s_gesture_start.y = -1;
}

static void immersive_gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;

    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_NONE) return;

    /* Only handle gestures that started in an edge zone */
    if (!s_edge_press) return;

    /* Swipe down from the top edge */
    if (dir == LV_DIR_BOTTOM && s_gesture_start.y < EDGE_ZONE_Y) {
        if (sf_gui_phone_bars_are_visible()) {
            sf_gui_notification_center_show();
        } else {
            sf_gui_phone_show_bars();
        }
        immersive_start_idle_timer();
        goto done;
    }
    // if (dir == LV_DIR_TOP && s_gesture_start.y > sh - EDGE_ZONE_Y) {
    //     sf_gui_phone_show_bars();
    //     immersive_start_idle_timer();
    //     goto done;
    // }

    /* Swipe up from the bottom edge → close the notification center if open */
    if (dir == LV_DIR_TOP && sf_gui_notification_center_is_visible()
        && s_gesture_start.y > lv_obj_get_height(lv_scr_act()) - EDGE_ZONE_Y) {
        sf_gui_notification_center_hide();
        goto done;
    }

    /* Swipe right from the left edge → app-level back (walks subpages back; no-op on home) */
    /* Ignored while the desktop or notification center is visible; going home is
     * only triggered by the HOME button in the bottom nav bar */
    if (dir == LV_DIR_RIGHT && s_gesture_start.x < EDGE_ZONE_X) {
        if (sf_gui_notification_center_is_visible()) goto done;
        if (s_desktop && !lv_obj_has_flag(s_desktop, LV_OBJ_FLAG_HIDDEN)) goto done;

        sf_app_ctx_t *cur = sf_app_get_current();
        if (cur && cur->manifest->ops->on_back) {
            cur->manifest->ops->on_back(cur);
        }
        goto done;
    }

done:
    s_gesture_start.x = -1;
    s_gesture_start.y = -1;
}

static void immersive_check_cb(lv_timer_t *t)
{
    (void)t;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;

    lv_indev_state_t state = lv_indev_get_state(indev);
    if (sf_gui_phone_bars_are_visible() && (state == LV_INDEV_STATE_PRESSED || state == LV_INDEV_STATE_RELEASED)) {
        immersive_start_idle_timer();
    }
}

/* ── Phone shell init ─────────────────────────────── */

esp_err_t sf_gui_phone_shell_init(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);

    lv_obj_add_style(scr, sf_theme_get_style(SF_STYLE_BG_SCREEN), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Desktop: content layer (screen layer), same level as the App root */
    s_desktop = sf_gui_launcher_create(scr);

    /* System UI mounted on the top layer: status/nav bars created last to stay on top */
    lv_obj_t *top = lv_layer_top();
    sf_gui_notification_center_create(top);
    sf_gui_phone_bars_create(top);

    /* Power-on test notification — verifies the UI pipeline */
    sf_notification_post("system", LV_SYMBOL_BELL, "System", "Ready");

    /* Register the GUI window interface — called back by AppManager at lifecycle points */
    static const sf_gui_window_ops_t s_gui_ops = {
        .pre_create      = gui_app_pre_create,
        .post_destroy    = gui_app_post_destroy,
        .on_resume_show  = gui_app_on_resume_show,
        .on_pause_hide   = gui_app_on_pause_hide,
    };
    sf_app_manager_set_gui_ops(&s_gui_ops);

    sf_gui_set_go_home_cb(phone_go_home);

    /* Gesture listener */
    lv_indev_t *indev = NULL;
    while ((indev = lv_indev_get_next(indev)) != NULL) {
        lv_indev_add_event_cb(indev, immersive_pressed_cb, LV_EVENT_PRESSED, NULL);
        lv_indev_add_event_cb(indev, immersive_gesture_cb, LV_EVENT_GESTURE, NULL);
        lv_indev_add_event_cb(indev, immersive_released_cb, LV_EVENT_RELEASED, NULL);
    }

    immersive_start_idle_timer();

    s_gesture_timer = lv_timer_create(immersive_check_cb, 50, NULL);
    lv_timer_set_repeat_count(s_gesture_timer, -1);

    return ESP_OK;
}
