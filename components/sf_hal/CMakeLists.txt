set(SF_HAL_SRCS
    "core/sf_hal_core.c"
    "display/sf_hal_display.c"
    "input/sf_hal_input.c"
    "board/sf_hal_board.c"
)

set(SF_HAL_INCLUDE_DIRS
    "include"
    "display"
    "input"
    "board"
)

# Only build the driver selected in Kconfig -- switching boards only changes sdkconfig.defaults, never shared code
if(CONFIG_SF_HAL_DISPLAY_ST7789)
    list(APPEND SF_HAL_SRCS "display/backends/st7789/st7789.c")
    list(APPEND SF_HAL_INCLUDE_DIRS "display/backends/st7789")
endif()

if(CONFIG_SF_HAL_INPUT_TOUCH_CST328)
    list(APPEND SF_HAL_SRCS "input/cst328/cst328.c")
    list(APPEND SF_HAL_INCLUDE_DIRS "input/cst328")
endif()

idf_component_register(
    SRCS ${SF_HAL_SRCS}
    INCLUDE_DIRS ${SF_HAL_INCLUDE_DIRS}
    PRIV_REQUIRES
        lvgl
        esp_lvgl_port
        esp_lcd
        esp_timer
        driver
)
