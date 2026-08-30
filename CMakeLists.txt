cmake_minimum_required(VERSION 3.16)

# ── Board selection ─────────────────────────────────────────
# Auto-detection by default: a sole board dir under boards/ is used automatically;
# with multiple boards, pick one via the environmental variable
#   export SF_BOARD=<name>    (e.g. esp32s3-touch-lcd-2_8).
# The board's Kconfig + sdkconfig.defaults are merged into the build via the IDF
# component mechanism (EXTRA_COMPONENT_DIRS) -- switching boards only changes
# boards/<name>/, shared code stays untouched.
set(SF_BOARD "$ENV{SF_BOARD}")
if(NOT SF_BOARD)
    file(GLOB _sf_boards RELATIVE "${CMAKE_CURRENT_LIST_DIR}/boards"
         "${CMAKE_CURRENT_LIST_DIR}/boards/*")
    list(LENGTH _sf_boards _sf_nboards)
    if(_sf_nboards EQUAL 1)
        set(SF_BOARD "${_sf_boards}")
    else()
        message(FATAL_ERROR "Multiple boards found in boards/. Set SF_BOARD=<name>.")
    endif()
endif()

# Existence check: fail fast if SF_BOARD is misspelled, avoiding silently broken
# exclusion logic and confusing errors from sdkconfig.defaults pointing at a missing file.
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/boards/${SF_BOARD}/sdkconfig.defaults")
    message(FATAL_ERROR "Board '${SF_BOARD}' not found: boards/${SF_BOARD}/ missing or "
                        "without sdkconfig.defaults. Export SF_BOARD=<name> (e.g. "
                        "esp32s3-touch-lcd-2_8).")
endif()

# Multi-board conflict protection: EXTRA_COMPONENT_DIRS boards makes IDF register
# every board dir under boards/ as a component (all Kconfig merged). Exclude the
# unselected board dirs here so their Kconfig does not participate in the build
# and pin/driver configs cannot collide. Only the selected board dir is kept for
# component discovery.
# Note: idf_build_set_property must be called after include(project.cmake) (see bottom of file).
set(SF_EXCLUDED_BOARD_DIRS "")
file(GLOB _sf_all_boards RELATIVE "${CMAKE_CURRENT_LIST_DIR}/boards"
     "${CMAKE_CURRENT_LIST_DIR}/boards/*")
foreach(_sf_board IN LISTS _sf_all_boards)
    if(NOT _sf_board STREQUAL SF_BOARD)
        list(APPEND SF_EXCLUDED_BOARD_DIRS "${CMAKE_CURRENT_LIST_DIR}/boards/${_sf_board}")
    endif()
endforeach()
unset(_sf_all_boards)
unset(_sf_board)

# Default config source = root generic config + board-level config (merged, latter overrides former).
#   - root sdkconfig.defaults: OS/App generic (LVGL, WiFi, NTP, FreeRTOS, partition table, ...)
#   - boards/<name>/sdkconfig.defaults: hardware related (display, touch, PSRAM, flash size, ...)
# When switching boards the root stays untouched; only boards/<name>/sdkconfig.defaults changes.
if(NOT DEFINED ENV{SDKCONFIG_DEFAULTS} AND NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS
        "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/boards/${SF_BOARD}/sdkconfig.defaults")
endif()

set(EXTRA_COMPONENT_DIRS apps boards)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Exclude unselected board dirs (must be set after include(project.cmake) and before
# project(), at which point idf_build_set_property is defined and component discovery
# has not yet run).
idf_build_set_property(EXTRA_COMPONENT_EXCLUDE_DIRS "${SF_EXCLUDED_BOARD_DIRS}")

# Firmware version -> PROJECT_VER -> esp_app_desc.version, surfaced as
# "Version" in Settings > Device Info. Bump it here when cutting a release;
# a top-level version.txt (if ever added) would take priority over this value.
project(StarryFire VERSION 1.0.0)
