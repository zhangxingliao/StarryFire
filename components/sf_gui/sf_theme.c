#include "sf_theme.h"
#include "sf_config.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "sf_theme";

/* ── Theme palettes (initialized at runtime to avoid non-literal const initializers) ─ */

static sf_theme_t g_themes[SF_THEME_COUNT];

static void init_theme_nebula(void)
{
    sf_theme_t *t = &g_themes[SF_THEME_NEBULA];
    t->name           = "Dark Nebula";
    t->bg_screen      = lv_color_hex(0x0a0a18);
    t->bg_page        = lv_color_hex(0x121220);
    t->bg_card        = lv_color_hex(0x1a1a30);
    t->bg_launcher    = lv_color_hex(0x0f0f20);
    t->bar_bg         = lv_color_hex(0x000000);
    t->separator      = lv_color_hex(0x1e1e3a);
    t->sep_primary    = lv_color_hex(0x28284a);
    t->text_primary   = lv_color_hex(0xf5f5ff);
    t->text_secondary = lv_color_hex(0xc8c8e0);
    t->text_muted     = lv_color_hex(0x7a7aa0);
    t->accent         = lv_color_hex(0x8b5cf6);
    t->active         = lv_color_hex(0x6366f1);
    t->dim            = lv_color_hex(0x2a2a46);
    t->arrow          = lv_color_hex(0x4f4f6b);
    t->bt_dim         = lv_color_hex(0x6b6b8a);
    t->rssi_green     = lv_color_hex(0x44ff44);
    t->rssi_amber     = lv_color_hex(0xffaa00);
    t->rssi_red       = lv_color_hex(0xff4444);
}

static void init_theme_aurora(void)
{
    sf_theme_t *t = &g_themes[SF_THEME_AURORA];
    t->name           = "Crystal";
    t->bg_screen      = lv_color_hex(0xefeff4);
    t->bg_page        = lv_color_hex(0xffffff);
    t->bg_card        = lv_color_hex(0xf2f2f7);
    t->bg_launcher    = lv_color_hex(0xefeff4);
    t->bar_bg         = lv_color_hex(0x000000);
    t->separator      = lv_color_hex(0xc6c6c8);
    t->sep_primary    = lv_color_hex(0xaeaeb2);
    t->text_primary   = lv_color_hex(0x1c1c1e);
    t->text_secondary = lv_color_hex(0x48484a);
    t->text_muted     = lv_color_hex(0x636366);
    t->accent         = lv_color_hex(0x007aff);
    t->active         = lv_color_hex(0x34c759);
    t->dim            = lv_color_hex(0xaeaeb2);
    t->arrow          = lv_color_hex(0x8e8e93);
    t->bt_dim         = lv_color_hex(0x8e8e93);
    t->rssi_green     = lv_color_hex(0x30d158);
    t->rssi_amber     = lv_color_hex(0xff9f0a);
    t->rssi_red       = lv_color_hex(0xff453a);
}

static void init_theme_palettes(void)
{
    init_theme_nebula();
    init_theme_aurora();
}

const char *sf_theme_name(sf_theme_id_t id)
{
    if (id < 0 || id >= SF_THEME_COUNT) return "?";
    return g_themes[id].name;
}

/* ── Currently active theme ─────────────────────────── */

static const sf_theme_t *s_active_theme = NULL;

const sf_theme_t *sf_theme_get_active(void)
{
    return s_active_theme;
}

/* ── Preset lv_style_t storage ──────────────────────── */

static lv_style_t s_styles[SF_STYLE_COUNT];

/* ── Initialize all preset styles from the current theme ─ */

static void init_styles(const sf_theme_t *th)
{
    /* Containers */
    lv_style_init(&s_styles[SF_STYLE_SCREEN]);
    lv_style_set_bg_color(&s_styles[SF_STYLE_SCREEN], th->bg_screen);
    lv_style_set_border_width(&s_styles[SF_STYLE_SCREEN], 0);
    lv_style_set_pad_all(&s_styles[SF_STYLE_SCREEN], 0);
    lv_style_set_radius(&s_styles[SF_STYLE_SCREEN], 0);

    lv_style_init(&s_styles[SF_STYLE_PAGE]);
    lv_style_set_bg_color(&s_styles[SF_STYLE_PAGE], th->bg_page);
    lv_style_set_border_width(&s_styles[SF_STYLE_PAGE], 0);
    lv_style_set_pad_all(&s_styles[SF_STYLE_PAGE], 0);
    lv_style_set_radius(&s_styles[SF_STYLE_PAGE], 0);

    lv_style_init(&s_styles[SF_STYLE_CARD]);
    lv_style_set_bg_color(&s_styles[SF_STYLE_CARD], th->bg_card);
    lv_style_set_radius(&s_styles[SF_STYLE_CARD], SF_RADIUS_MD);
    lv_style_set_border_width(&s_styles[SF_STYLE_CARD], 0);
    lv_style_set_pad_all(&s_styles[SF_STYLE_CARD], SF_PAD_LG);

    lv_style_init(&s_styles[SF_STYLE_TRANSP]);
    lv_style_set_bg_opa(&s_styles[SF_STYLE_TRANSP], LV_OPA_TRANSP);
    lv_style_set_border_width(&s_styles[SF_STYLE_TRANSP], 0);
    lv_style_set_pad_all(&s_styles[SF_STYLE_TRANSP], 0);

    /* Text */
    lv_style_init(&s_styles[SF_STYLE_TITLE]);
    lv_style_set_text_color(&s_styles[SF_STYLE_TITLE], th->text_primary);
    lv_style_set_text_font(&s_styles[SF_STYLE_TITLE], SF_FONT_LG);

    lv_style_init(&s_styles[SF_STYLE_BODY]);
    lv_style_set_text_color(&s_styles[SF_STYLE_BODY], th->text_primary);
    lv_style_set_text_font(&s_styles[SF_STYLE_BODY], SF_FONT_SM);

    lv_style_init(&s_styles[SF_STYLE_LABEL]);
    lv_style_set_text_color(&s_styles[SF_STYLE_LABEL], th->text_secondary);
    lv_style_set_text_font(&s_styles[SF_STYLE_LABEL], SF_FONT_XS);

    lv_style_init(&s_styles[SF_STYLE_SECTION]);
    lv_style_set_text_color(&s_styles[SF_STYLE_SECTION], th->text_muted);
    lv_style_set_text_font(&s_styles[SF_STYLE_SECTION], SF_FONT_SM);

    lv_style_init(&s_styles[SF_STYLE_ICON]);
    lv_style_set_text_color(&s_styles[SF_STYLE_ICON], th->text_primary);
    lv_style_set_text_font(&s_styles[SF_STYLE_ICON], SF_FONT_XL);

    /* Buttons */
    lv_style_init(&s_styles[SF_STYLE_BTN]);
    lv_style_set_bg_color(&s_styles[SF_STYLE_BTN], th->text_primary);
    lv_style_set_bg_opa(&s_styles[SF_STYLE_BTN], LV_OPA_10);
    lv_style_set_radius(&s_styles[SF_STYLE_BTN], SF_RADIUS_LG);
    lv_style_set_border_width(&s_styles[SF_STYLE_BTN], 0);
    lv_style_set_pad_all(&s_styles[SF_STYLE_BTN], 0);

    lv_style_init(&s_styles[SF_STYLE_BTN_PRESSED]);
    lv_style_set_bg_opa(&s_styles[SF_STYLE_BTN_PRESSED], LV_OPA_20);

    lv_style_init(&s_styles[SF_STYLE_BAR_BTN]);
    lv_style_set_bg_opa(&s_styles[SF_STYLE_BAR_BTN], LV_OPA_TRANSP);
    lv_style_set_border_width(&s_styles[SF_STYLE_BAR_BTN], 0);
    lv_style_set_radius(&s_styles[SF_STYLE_BAR_BTN], 0);
    lv_style_set_pad_all(&s_styles[SF_STYLE_BAR_BTN], SF_PAD_SM);

    lv_style_init(&s_styles[SF_STYLE_BAR_BTN_PRESSED]);
    lv_style_set_bg_color(&s_styles[SF_STYLE_BAR_BTN_PRESSED], th->bar_bg);
    lv_style_set_bg_opa(&s_styles[SF_STYLE_BAR_BTN_PRESSED], LV_OPA_10);

    /* Bars */
    lv_style_init(&s_styles[SF_STYLE_STATUS_BAR]);
    lv_style_set_bg_color(&s_styles[SF_STYLE_STATUS_BAR], th->bar_bg);
    lv_style_set_bg_opa(&s_styles[SF_STYLE_STATUS_BAR], LV_OPA_10);
    lv_style_set_border_width(&s_styles[SF_STYLE_STATUS_BAR], 0);
    lv_style_set_pad_all(&s_styles[SF_STYLE_STATUS_BAR], 0);
    lv_style_set_pad_left(&s_styles[SF_STYLE_STATUS_BAR], SF_PAD_MD);
    lv_style_set_pad_right(&s_styles[SF_STYLE_STATUS_BAR], SF_PAD_MD);
    lv_style_set_radius(&s_styles[SF_STYLE_STATUS_BAR], SF_RADIUS_NONE);

    lv_style_init(&s_styles[SF_STYLE_NAV_PILL]);
    lv_style_set_bg_color(&s_styles[SF_STYLE_NAV_PILL], th->bar_bg);
    lv_style_set_bg_opa(&s_styles[SF_STYLE_NAV_PILL], LV_OPA_10);
    lv_style_set_border_width(&s_styles[SF_STYLE_NAV_PILL], 0);
    lv_style_set_pad_all(&s_styles[SF_STYLE_NAV_PILL], 0);
    lv_style_set_radius(&s_styles[SF_STYLE_NAV_PILL], SF_RADIUS_NONE);

    lv_style_init(&s_styles[SF_STYLE_NAV_PILL_BTN]);
    lv_style_set_text_color(&s_styles[SF_STYLE_NAV_PILL_BTN], th->text_primary);

    /* Color-only companion styles (single color property each) */
    lv_style_init(&s_styles[SF_STYLE_BG_SCREEN]);
    lv_style_set_bg_color(&s_styles[SF_STYLE_BG_SCREEN], th->bg_screen);
    lv_style_init(&s_styles[SF_STYLE_BG_PAGE]);
    lv_style_set_bg_color(&s_styles[SF_STYLE_BG_PAGE], th->bg_page);
    lv_style_init(&s_styles[SF_STYLE_BG_CARD]);
    lv_style_set_bg_color(&s_styles[SF_STYLE_BG_CARD], th->bg_card);
    lv_style_init(&s_styles[SF_STYLE_BG_LAUNCHER]);
    lv_style_set_bg_color(&s_styles[SF_STYLE_BG_LAUNCHER], th->bg_launcher);
    lv_style_init(&s_styles[SF_STYLE_BG_BAR]);
    lv_style_set_bg_color(&s_styles[SF_STYLE_BG_BAR], th->bar_bg);
    lv_style_init(&s_styles[SF_STYLE_BG_ACCENT]);
    lv_style_set_bg_color(&s_styles[SF_STYLE_BG_ACCENT], th->accent);
    lv_style_init(&s_styles[SF_STYLE_BG_ACTIVE]);
    lv_style_set_bg_color(&s_styles[SF_STYLE_BG_ACTIVE], th->active);
    lv_style_init(&s_styles[SF_STYLE_BG_SEPARATOR]);
    lv_style_set_bg_color(&s_styles[SF_STYLE_BG_SEPARATOR], th->separator);
    lv_style_init(&s_styles[SF_STYLE_BG_SEP_PRIMARY]);
    lv_style_set_bg_color(&s_styles[SF_STYLE_BG_SEP_PRIMARY], th->sep_primary);
    lv_style_init(&s_styles[SF_STYLE_BG_PRESSED]);
    lv_style_set_bg_color(&s_styles[SF_STYLE_BG_PRESSED], th->text_primary);

    lv_style_init(&s_styles[SF_STYLE_TXT_PRIMARY]);
    lv_style_set_text_color(&s_styles[SF_STYLE_TXT_PRIMARY], th->text_primary);
    lv_style_init(&s_styles[SF_STYLE_TXT_SECONDARY]);
    lv_style_set_text_color(&s_styles[SF_STYLE_TXT_SECONDARY], th->text_secondary);
    lv_style_init(&s_styles[SF_STYLE_TXT_MUTED]);
    lv_style_set_text_color(&s_styles[SF_STYLE_TXT_MUTED], th->text_muted);
    lv_style_init(&s_styles[SF_STYLE_TXT_ACCENT]);
    lv_style_set_text_color(&s_styles[SF_STYLE_TXT_ACCENT], th->accent);
    lv_style_init(&s_styles[SF_STYLE_TXT_ACTIVE]);
    lv_style_set_text_color(&s_styles[SF_STYLE_TXT_ACTIVE], th->active);
    lv_style_init(&s_styles[SF_STYLE_TXT_ARROW]);
    lv_style_set_text_color(&s_styles[SF_STYLE_TXT_ARROW], th->arrow);
    lv_style_init(&s_styles[SF_STYLE_TXT_BT_DIM]);
    lv_style_set_text_color(&s_styles[SF_STYLE_TXT_BT_DIM], th->bt_dim);

    lv_style_init(&s_styles[SF_STYLE_BORDER_SEP_PRIMARY]);
    lv_style_set_border_color(&s_styles[SF_STYLE_BORDER_SEP_PRIMARY], th->sep_primary);
    lv_style_init(&s_styles[SF_STYLE_BORDER_ACTIVE]);
    lv_style_set_border_color(&s_styles[SF_STYLE_BORDER_ACTIVE], th->active);

    lv_style_init(&s_styles[SF_STYLE_LINE_SEP]);
    lv_style_set_line_color(&s_styles[SF_STYLE_LINE_SEP], th->separator);
}

/* ── lv_theme_t callbacks ───────────────────────────── */

static void theme_apply_cb(lv_theme_t *th, lv_obj_t *obj)
{
    (void)th;
    if (lv_obj_check_type(obj, &lv_obj_class)) {
        lv_obj_add_style(obj, &s_styles[SF_STYLE_TRANSP], 0);
    }
}

/* ── Public API ─────────────────────────────────────── */

esp_err_t sf_theme_init(void)
{
    ESP_LOGI(TAG, "initializing theme system");

    lv_disp_t *disp = lv_disp_get_default();
    if (!disp) {
        ESP_LOGW(TAG, "no default display yet, skipping theme init");
        return ESP_OK;
    }

    /* Initialize theme palettes */
    init_theme_palettes();

    /* Read persisted theme setting */
    sf_theme_id_t saved_id = (sf_theme_id_t)sf_config_get_theme();
    if (saved_id < 0 || saved_id >= SF_THEME_COUNT) saved_id = SF_THEME_NEBULA;
    s_active_theme = &g_themes[saved_id];
    ESP_LOGI(TAG, "active theme: %s (id=%d)", s_active_theme->name, (int)saved_id);

    init_styles(s_active_theme);

    lv_theme_t *default_theme = lv_disp_get_theme(disp);
    lv_theme_t *th = lv_theme_create();
    if (!th) {
        ESP_LOGE(TAG, "failed to create lv_theme_t");
        return ESP_FAIL;
    }
    if (default_theme) {
        lv_theme_set_parent(th, default_theme);
    }
    lv_theme_set_apply_cb(th, theme_apply_cb);
    lv_disp_set_theme(disp, th);

    ESP_LOGI(TAG, "theme system ready");
    return ESP_OK;
}

esp_err_t sf_theme_set_active(sf_theme_id_t id)
{
    if (id < 0 || id >= SF_THEME_COUNT) return ESP_ERR_INVALID_ARG;
    if (s_active_theme == &g_themes[id]) return ESP_OK;   /* no change */

    s_active_theme = &g_themes[id];
    ESP_LOGI(TAG, "switching to theme: %s", s_active_theme->name);

    /* Re-initialize all preset styles (reset first to free old LVGL style memory) */
    for (int i = 0; i < SF_STYLE_COUNT; i++) {
        lv_style_reset(&s_styles[i]);
    }
    init_styles(s_active_theme);

    /* Notify LVGL of a global style change: every widget referencing s_styles[] redraws */
    lv_obj_report_style_change(NULL);

    return ESP_OK;
}

const lv_style_t *sf_theme_get_style(sf_style_id_t id)
{
    if (id < 0 || id >= SF_STYLE_COUNT) return NULL;
    return &s_styles[id];
}
