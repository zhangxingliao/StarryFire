#pragma once

#include "lvgl.h"
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Theme identifiers
 * ================================================================ */

typedef enum {
    SF_THEME_NEBULA,            /* Dark Nebula — deep purple/indigo (default) */
    SF_THEME_AURORA,            /* Aurora — deep teal/emerald green */
    SF_THEME_COUNT,
} sf_theme_id_t;

extern const char *sf_theme_name(sf_theme_id_t id);

/* ================================================================
 * Theme palette struct
 * ================================================================ */

typedef struct {
    const char *name;

    /* Background colors */
    lv_color_t bg_screen;
    lv_color_t bg_page;
    lv_color_t bg_card;
    lv_color_t bg_launcher;
    lv_color_t bar_bg;

    /* Separators */
    lv_color_t separator;
    lv_color_t sep_primary;

    /* Text colors */
    lv_color_t text_primary;
    lv_color_t text_secondary;
    lv_color_t text_muted;

    /* Accent / functional colors */
    lv_color_t accent;
    lv_color_t active;
    lv_color_t dim;
    lv_color_t arrow;
    lv_color_t bt_dim;

    /* Signal strength */
    lv_color_t rssi_green;
    lv_color_t rssi_amber;
    lv_color_t rssi_red;
} sf_theme_t;

/* ================================================================
 * Semantic color macros — resolved at runtime from the active theme
 * ================================================================
 * For creation-time styling prefer the SF_STYLE_* color presets via
 * sf_theme_get_style() so widgets auto-follow theme switches. SF_COLOR_*
 * remains for draw/refresh-time reads (re-evaluated on each redraw).
 * If sf_theme_get_active() is still NULL (not initialized), the macros
 * null-check and fall back to black.
 * ================================================================ */

#define SF_COLOR_BG_SCREEN      (sf_theme_get_active() ? sf_theme_get_active()->bg_screen : lv_color_black())
#define SF_COLOR_BG_PAGE        (sf_theme_get_active() ? sf_theme_get_active()->bg_page : lv_color_black())
#define SF_COLOR_BG_CARD        (sf_theme_get_active() ? sf_theme_get_active()->bg_card : lv_color_black())
#define SF_COLOR_BG_LAUNCHER    (sf_theme_get_active() ? sf_theme_get_active()->bg_launcher : lv_color_black())
#define SF_COLOR_BAR_BG         (sf_theme_get_active() ? sf_theme_get_active()->bar_bg : lv_color_black())
#define SF_COLOR_SEPARATOR      (sf_theme_get_active() ? sf_theme_get_active()->separator : lv_color_black())
#define SF_COLOR_SEP_PRIMARY    (sf_theme_get_active() ? sf_theme_get_active()->sep_primary : lv_color_black())
#define SF_COLOR_TEXT_PRIMARY   (sf_theme_get_active() ? sf_theme_get_active()->text_primary : lv_color_white())
#define SF_COLOR_TEXT_SECONDARY (sf_theme_get_active() ? sf_theme_get_active()->text_secondary : lv_color_white())
#define SF_COLOR_TEXT_MUTED     (sf_theme_get_active() ? sf_theme_get_active()->text_muted : lv_color_white())
#define SF_COLOR_ACCENT         (sf_theme_get_active() ? sf_theme_get_active()->accent : lv_color_black())
#define SF_COLOR_ACTIVE         (sf_theme_get_active() ? sf_theme_get_active()->active : lv_color_black())
#define SF_COLOR_DIM            (sf_theme_get_active() ? sf_theme_get_active()->dim : lv_color_black())
#define SF_COLOR_ARROW          (sf_theme_get_active() ? sf_theme_get_active()->arrow : lv_color_black())
#define SF_COLOR_BT_DIM         (sf_theme_get_active() ? sf_theme_get_active()->bt_dim : lv_color_black())
#define SF_COLOR_RSSI_GREEN     (sf_theme_get_active() ? sf_theme_get_active()->rssi_green : lv_color_black())
#define SF_COLOR_RSSI_AMBER     (sf_theme_get_active() ? sf_theme_get_active()->rssi_amber : lv_color_black())
#define SF_COLOR_RSSI_RED       (sf_theme_get_active() ? sf_theme_get_active()->rssi_red : lv_color_black())

/* ================================================================
 * Font-scale macros (fixed at compile time; not yet part of the
 * runtime theme)
 *
 * Each font tier's size comes from the board-level CONFIG_SF_FONT_*_SIZE,
 * token-pasted to build the lv_font_montserrat_<size> symbol. To port to a
 * different board, adjust the sizes in boards/<name>/Kconfig and enable the
 * matching CONFIG_LV_FONT_MONTSERRAT_* in its sdkconfig.defaults.
 * ================================================================ */

#define SF_FONT_NAME_(n)   lv_font_montserrat_##n
#define SF_FONT_NAME(n)    SF_FONT_NAME_(n)

LV_FONT_DECLARE(SF_FONT_NAME(CONFIG_SF_FONT_XS_SIZE));
LV_FONT_DECLARE(SF_FONT_NAME(CONFIG_SF_FONT_SM_SIZE));
LV_FONT_DECLARE(SF_FONT_NAME(CONFIG_SF_FONT_MD_SIZE));
LV_FONT_DECLARE(SF_FONT_NAME(CONFIG_SF_FONT_LG_SIZE));
LV_FONT_DECLARE(SF_FONT_NAME(CONFIG_SF_FONT_XL_SIZE));
LV_FONT_DECLARE(SF_FONT_NAME(CONFIG_SF_FONT_XXL_SIZE));

#define SF_FONT_XS_SIZE    CONFIG_SF_FONT_XS_SIZE
#define SF_FONT_SM_SIZE    CONFIG_SF_FONT_SM_SIZE
#define SF_FONT_MD_SIZE    CONFIG_SF_FONT_MD_SIZE
#define SF_FONT_LG_SIZE    CONFIG_SF_FONT_LG_SIZE
#define SF_FONT_XL_SIZE    CONFIG_SF_FONT_XL_SIZE
#define SF_FONT_XXL_SIZE   CONFIG_SF_FONT_XXL_SIZE

#define SF_FONT_XS         (&SF_FONT_NAME(SF_FONT_XS_SIZE))
#define SF_FONT_SM         (&SF_FONT_NAME(SF_FONT_SM_SIZE))
#define SF_FONT_MD         (&SF_FONT_NAME(SF_FONT_MD_SIZE))
#define SF_FONT_LG         (&SF_FONT_NAME(SF_FONT_LG_SIZE))
#define SF_FONT_XL         (&SF_FONT_NAME(SF_FONT_XL_SIZE))
#define SF_FONT_XXL        (&SF_FONT_NAME(SF_FONT_XXL_SIZE))

/* ================================================================
 * Corner radius & spacing constants
 * ================================================================ */

#define SF_RADIUS_NONE   0
#define SF_RADIUS_SM     4
#define SF_RADIUS_MD     8
#define SF_RADIUS_LG     12
#define SF_RADIUS_FULL   LV_RADIUS_CIRCLE

#define SF_PAD_SM    4
#define SF_PAD_MD    8
#define SF_PAD_LG    12

#define SF_GAP_SM    4
#define SF_GAP_MD    8

/* ================================================================
 * UI scaling unit — relative sizing to fit different LCD sizes
 * Baseline is a 240px short side: SF_UI is the identity transform on a
 * 240px short side (zero visual change on this board); other resolutions
 * scale pixel values by the short-side ratio.
 * Usage: lv_obj_set_size(obj, SF_UI(56), SF_UI(56));
 * NOTE: only for scalable sizes (buttons, icons, bar heights, spacing).
 * NOT applicable to 1px separators, borders, font sizes, or constants.
 * ================================================================ */
#define SF_UI_SHORT_SIDE \
    (CONFIG_SF_HAL_DISPLAY_WIDTH < CONFIG_SF_HAL_DISPLAY_HEIGHT ? \
     CONFIG_SF_HAL_DISPLAY_WIDTH : CONFIG_SF_HAL_DISPLAY_HEIGHT)
#define SF_UI(px) (((px) * SF_UI_SHORT_SIDE + 120) / 240)

/* ================================================================
 * Preset style enum
 * ================================================================ */

typedef enum {
    SF_STYLE_SCREEN,
    SF_STYLE_PAGE,
    SF_STYLE_CARD,
    SF_STYLE_TRANSP,

    SF_STYLE_TITLE,
    SF_STYLE_BODY,
    SF_STYLE_LABEL,
    SF_STYLE_SECTION,
    SF_STYLE_ICON,

    SF_STYLE_BTN,
    SF_STYLE_BTN_PRESSED,
    SF_STYLE_BAR_BTN,
    SF_STYLE_BAR_BTN_PRESSED,

    SF_STYLE_STATUS_BAR,
    SF_STYLE_NAV_PILL,
    SF_STYLE_NAV_PILL_BTN,

    /* Color-only companion styles: each sets exactly one color property so that
     * widgets previously styled with lv_obj_set_style_*(..., SF_COLOR_*, ...)
     * can reference a preset and automatically follow theme switches via
     * lv_obj_report_style_change(NULL). */
    SF_STYLE_BG_SCREEN,
    SF_STYLE_BG_PAGE,
    SF_STYLE_BG_CARD,
    SF_STYLE_BG_LAUNCHER,
    SF_STYLE_BG_BAR,
    SF_STYLE_BG_ACCENT,
    SF_STYLE_BG_ACTIVE,
    SF_STYLE_BG_SEPARATOR,
    SF_STYLE_BG_SEP_PRIMARY,
    SF_STYLE_BG_PRESSED,          /* text_primary used as a pressed-state background */

    SF_STYLE_TXT_PRIMARY,
    SF_STYLE_TXT_SECONDARY,
    SF_STYLE_TXT_MUTED,
    SF_STYLE_TXT_ACCENT,
    SF_STYLE_TXT_ACTIVE,
    SF_STYLE_TXT_ARROW,
    SF_STYLE_TXT_BT_DIM,

    SF_STYLE_BORDER_SEP_PRIMARY,
    SF_STYLE_BORDER_ACTIVE,

    SF_STYLE_LINE_SEP,            /* chart grid / divider line color */

    SF_STYLE_COUNT,
} sf_style_id_t;

/* ================================================================
 * Public API
 * ================================================================ */

esp_err_t sf_theme_init(void);

const sf_theme_t *sf_theme_get_active(void);
esp_err_t sf_theme_set_active(sf_theme_id_t id);

const lv_style_t *sf_theme_get_style(sf_style_id_t id);

#ifdef __cplusplus
}
#endif
