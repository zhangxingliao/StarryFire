if(CONFIG_SF_GUI_SHELL_PHONE)
    list(APPEND SF_GUI_SRCS
        "phone/sf_gui_status_bar.c"
        "phone/sf_gui_nav_bar.c"
        "phone/sf_gui_launcher.c"
        "phone/sf_gui_bars.c"
        "phone/sf_gui_phone_shell.c"
        "phone/sf_gui_notification_center.c"
    )
endif()

idf_component_register(
    SRCS
        "sf_gui_shell.c"
        "sf_theme.c"
        ${SF_GUI_SRCS}
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        sf_sys
        sf_config
        lvgl
        esp_lvgl_port
)

# Put LVGL's 256KB memory pool in PSRAM (external RAM) instead of internal DRAM.
# lv_mem_core_builtin.c marks the builtin pool array with LV_ATTRIBUTE_LARGE_RAM_ARRAY, and
# lv_conf_internal.h guards that macro with #ifndef, so the command-line -D added below
# survives and points the array at section ".ext_ram.bss". When
# CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY is set (board sdkconfig.defaults), IDF routes
# that section to PSRAM at link time, so LVGL's own TLSF allocator runs directly on a static
# 256KB PSRAM array — no custom allocator (heap_caps/malloc) involved, DRAM cost ~0.
# LVGL is pulled in as a managed component, so its CMake target is "lvgl__lvgl"
# (not "lvgl"). Resolve the real target via COMPONENT_LIB — same pattern esp_lvgl_port
# itself uses — then attach the pool override so it survives regardless of how LVGL is wired in.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
if(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY)
    target_compile_definitions(${lvgl_lib} PUBLIC
        "LV_ATTRIBUTE_LARGE_RAM_ARRAY=__attribute__((section(\".ext_ram.bss\")))")
endif()
