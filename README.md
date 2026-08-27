# StarryFire

A lightweight graphics-oriented OS framework for ESP32 series chips. Built on ESP-IDF with the **LVGL** rendering engine, it delivers a smartphone-like interactive experience on resource-constrained embedded devices.

### Key Features

- **Board Abstraction**: The `boards/<name>/` directory encapsulates hardware differences — pin assignments, SPI/I2C buses, display parameters, and partition tables. Switching hardware platforms only requires adding a new board directory; shared code remains untouched.
- **App Model**: Full lifecycle management (on_create → on_resume → on_pause → on_destroy) with Intent-based navigation and LRU background eviction. Apps communicate through a publish-subscribe event bus.
- **System UI**: Built-in Phone Shell — desktop Launcher, status bar, navigation bar, and notification center. Supports edge gestures and immersive mode switching. The system UI layer sits above the App layer.
- **Themes & Scaling**: Dark / light dual-theme support with runtime switching; selection persists to Flash. The `SF_UI()` macro scales proportionally from a 240px short-side baseline to fit different LCD sizes.
- **System Services**: Event bus (publish-subscribe), notification service (ring buffer + auto-expiry), WiFi scan and connection management, NTP time sync, SPIFFS config persistence.

> **Design Principle**: All hardware differences are confined to `boards/<name>/`. Upper-layer code is fully decoupled from specific hardware.

## Repository Structure

```
boards/                Board configuration (Kconfig + sdkconfig.defaults + partition table)
components/
  sf_hal/              Hardware abstraction layer (display/touch drivers, board init, GPIO/MCPWM/PWM wrappers)
  sf_sys/              System services (AppManager / EventBus / Intent / Notification / WiFi / NTP)
  sf_gui/              GUI layer (Phone Shell / theme system / UI scaling / gesture navigation)
  sf_config/           Config persistence (SPIFFS JSON)
apps/
  settings/            Settings App (WiFi / Bluetooth / Language / Device Info)
  monitor/             Monitor App (Tasks / Memory / Storage tabs)
main/                  Entry point app_main + system service initialization
docs/                  Project documentation (feature guides, architecture design, etc.)
export.sh              Environment initialization script (auto-detect board + load ESP-IDF)
```

## Build Environment

| Dependency | Version |
|------------|---------|
| ESP-IDF | **v5.4.3** |
| LVGL | **v9.5** |

## Build / Flash / Monitor

The `export.sh` script in the project root auto-detects the board and exports the ESP-IDF environment:

```bash
source export.sh                          # auto-detect the single board under boards/
source export.sh esp32s3-touch-lcd-2_8    # specify a board
```

Then use standard ESP-IDF commands:

```bash
idf.py build                              # build
idf.py -p /dev/ttyUSB0 flash              # flash
idf.py -p /dev/ttyUSB0 monitor            # serial monitor
idf.py -p /dev/ttyUSB0 flash monitor      # flash + monitor in one step
```

> In a multi-board setup, use `export SF_BOARD=<name>` to switch target boards. The build system automatically loads the corresponding `boards/<name>/sdkconfig.defaults` and partition table.

## Custom Apps

StarryFire's app model is based on lifecycle callbacks. Registering a new App takes three steps:

**1. Define App struct and callbacks**

Create a new directory under `apps/` (e.g. `apps/myapp/`) and implement the `sf_app_t` lifecycle callbacks:

```c
#include "sf_app_manager.h"

static void on_create(lv_obj_t *parent) {
    // Create UI; parent is the App's root container
}

static void on_resume(void) {
    // Called when returning from background to foreground
}

static void on_pause(void) {
    // Called when entering background (App not destroyed)
}

static void on_destroy(void) {
    // Destroy UI and release resources
}

static bool on_event(const sf_event_t *ev) {
    // Handle system events like notification clicks
    return false;  // return true if consumed
}

static const sf_app_t my_app = {
    .id        = "myapp",
    .name      = "My App",
    .icon      = NULL,
    .on_create = on_create,
    .on_resume = on_resume,
    .on_pause  = on_pause,
    .on_destroy = on_destroy,
    .on_event  = on_event,
};
```

**2. Register the App**

In `app_main()` or your initialization function:

```c
sf_app_register(&my_app);        // register with AppManager
sf_app_start("myapp");           // start (triggers on_create)
```

**3. Navigate via Intent**

```c
sf_app_start_intent("myapp", "action_name", "{\"key\":\"value\"}");
```

The receiver reads parameters from `sf_event_t.intent_action` and `sf_event_t.intent_data` in the `on_event` callback.
