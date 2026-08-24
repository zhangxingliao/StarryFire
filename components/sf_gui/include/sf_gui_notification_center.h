#ifndef SF_GUI_NOTIFICATION_CENTER_H
#define SF_GUI_NOTIFICATION_CENTER_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *sf_gui_notification_center_create(lv_obj_t *parent);
void sf_gui_notification_center_show(void);
void sf_gui_notification_center_hide(void);
bool sf_gui_notification_center_is_visible(void);
void sf_gui_notification_center_toggle(void);

#ifdef __cplusplus
}
#endif

#endif /* SF_GUI_NOTIFICATION_CENTER_H */
