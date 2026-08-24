#include "sf_settings_pages.h"
#include "esp_log.h"
#include "sf_theme.h"

static const char *TAG = "sf_settings_bt";

lv_obj_t *sf_settings_bt_create(lv_obj_t *parent, settings_ctx_t *ctx)
{
    lv_obj_t *page = settings_page_create(parent);

    lv_obj_t *lbl = lv_label_create(page);
    lv_label_set_text(lbl, "Bluetooth settings — coming soon");
    lv_obj_add_style(lbl, sf_theme_get_style(SF_STYLE_TXT_BT_DIM), 0);
    lv_obj_set_style_text_font(lbl, SF_FONT_XS, 0);

    return page;
}
