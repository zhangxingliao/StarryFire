# StarryFire

面向 ESP32 系列芯片的轻量级图形操作系统框架。基于 ESP-IDF 构建，集成 **LVGL** 渲染引擎，为资源受限的嵌入式设备提供类似智能手机的交互体验。

### 核心能力

- **板级抽象**：`boards/<name>/` 目录封装了芯片引脚、SPI/I2C 总线、屏幕参数、分区表等硬件差异，切换硬件平台只需新增一个板级目录，共享代码零改动。
- **应用模型**：提供完整的 App 生命周期管理（on_create → on_resume → on_pause → on_destroy），支持 Intent 跳转和后台 LRU 淘汰，App 之间通过事件总线通信。
- **系统 UI**：内置 Phone Shell——包含桌面 Launcher、状态栏、导航栏、通知中心，支持边缘手势和沉浸式切换，系统 UI 层级高于 App 层级。
- **主题与缩放**：支持深色 / 浅色双主题运行时切换，选择持久化到 Flash；`SF_UI()` 宏以 240px 短边为基准按比例缩放，适配不同尺寸 LCD。
- **系统服务**：事件总线（发布-订阅）、通知服务（环形缓冲 + 定时自动清除）、WiFi 扫描与连接管理、NTP 时间同步、SPIFFS 配置持久化。

> **设计原则**：硬件差异完全收敛到 `boards/<name>/`，所有上层代码与具体硬件解耦。

## 仓库目录结构

```
boards/                板级配置（Kconfig + sdkconfig.defaults + 分区表）
components/
  sf_hal/              硬件抽象层（显示/触摸驱动、板级初始化、GPIO/MCPWM/PWM 封装）
  sf_sys/              系统服务层（AppManager / EventBus / Intent / Notification / WiFi / NTP）
  sf_gui/              GUI 层（Phone Shell / 主题系统 / UI 缩放 / 手势导航）
  sf_config/           配置持久化（SPIFFS JSON）
apps/
  settings/            设置 App（WiFi / 蓝牙 / 语言 / 设备信息）
  monitor/             监控 App（任务 / 内存 / 存储三 Tab）
main/                  入口 app_main + 系统服务初始化
docs/                  项目相关文档（功能介绍、架构设计等）
export.sh              环境初始化脚本（自动探测板子 + 加载 ESP-IDF）
```

## 编译环境

| 依赖 | 版本要求 |
|------|----------|
| ESP-IDF | **v5.4.3** |
| LVGL | **v9.5** |

## 编译 / 烧录 / 日志

项目根目录的 `export.sh` 会自动导出 ESP-IDF 环境并探测当前板子：

```bash
source export.sh                          # 自动探测 boards/ 下唯一板子
source export.sh esp32s3-touch-lcd-2_8    # 指定板子
```

导出环境后，使用 ESP-IDF 标准命令：

```bash
idf.py build                              # 编译
idf.py -p /dev/ttyUSB0 flash              # 烧录
idf.py -p /dev/ttyUSB0 monitor            # 串口日志
idf.py -p /dev/ttyUSB0 flash monitor      # 编译+烧录+日志一步到位
```

> 多板环境时，通过 `export SF_BOARD=<name>` 切换目标板，编译系统会自动加载对应的 `boards/<name>/sdkconfig.defaults` 和分区表。

## 自定义 App

StarryFire 的应用模型基于生命周期回调，注册一个新 App 只需三步：

**1. 定义 App 结构体和回调**

在 `apps/` 下新建目录（如 `apps/myapp/`），实现 `sf_app_t` 的生命周期回调：

```c
#include "sf_app_manager.h"

static void on_create(lv_obj_t *parent) {
    // 创建 UI，parent 是 App 的根容器
}

static void on_resume(void) {
    // 从后台回到前台时调用
}

static void on_pause(void) {
    // 进入后台时调用（App 未销毁）
}

static void on_destroy(void) {
    // 销毁 UI、释放资源
}

static bool on_event(const sf_event_t *ev) {
    // 处理通知点击等系统事件
    return false;  // 返回 true 表示已消费
}

static const sf_app_t my_app = {
    .id        = "myapp",
    .name      = "我的应用",
    .icon      = NULL,
    .on_create = on_create,
    .on_resume = on_resume,
    .on_pause  = on_pause,
    .on_destroy = on_destroy,
    .on_event  = on_event,
};
```

**2. 注册 App**

在 `app_main()` 中（或你的初始化函数里）调用：

```c
sf_app_register(&my_app);        // 注册到 AppManager
sf_app_start("myapp");           // 启动（触发 on_create）
```

**3. 使用 Intent 跳转**

```c
sf_app_start_intent("myapp", "action_name", "{\"key\":\"value\"}");
```

接收方通过 `on_event` 回调中的 `sf_event_t.intent_action` 和 `sf_event_t.intent_data` 获取参数。
