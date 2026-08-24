idf_component_register(
    SRCS
        "sf_app_monitor.c"        # Integration shell: tabview/header/lifecycle/collect scheduling
        "sf_monitor_tasks_ui.c"   # Tasks page UI
        "sf_monitor_tasks_data.c" # Tasks data collection
        "sf_monitor_mem_ui.c"     # Regions page UI
        "sf_monitor_mem_data.c"   # Memory-region data collection
        "sf_monitor_disk_ui.c"    # Storage page UI (placeholder)
        "sf_monitor_disk_data.c"  # Disk data collection (placeholder)
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        sf_sys
        sf_gui
        lvgl
        esp_lvgl_port
        esp_system
        esp_timer
        heap
        spiffs
        freertos
)
