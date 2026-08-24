# StarryFire

面向 ESP32-S3 的轻量级图形操作系统框架——在 ESP-IDF 之上提供**板级抽象**、**类手机应用模型**（生命周期 / Intent / Launcher）与基于 **LVGL 9** 的标准 GUI 运行时。

> 设计原则：**换板只改 `boards/<name>/`，共享代码零改动。**

## 特性

- **板级裁剪**：`boards/<name>/` 组件（Kconfig + sdkconfig.defaults），`SF_BOARD` 环境变量 / 单板自动探测选择
- **驱动抽象**：`sf_hal` 条件编译（ST7789 显示、CST328 触摸），引脚经 `CONFIG_SF_PIN_*` 收口
- **应用模型**：`sf_app_*` 生命周期（on_create…on_destroy）、Intent 跳转、LRU 后台淘汰
- **系统 UI**：Phone Shell（桌面 + 状态栏/导航栏 + 通知中心 + 边缘手势沉浸式），系统 UI 挂 `lv_layer_top()` 不被 App 覆盖
- **主题**：深色 Dark Nebula / 浅色 Crystal 双主题运行时切换，选择持久化
- **UI 缩放**：`SF_UI()` 以短边 240px 为基准按比例缩放，适配不同尺寸 LCD

## 快速开始

```bash
source $IDF_PATH/export.sh   # ESP-IDF v5.4.3
idf.py build                 # 单板自动探测；多板时 export SF_BOARD=<name>
idf.py -p PORT flash monitor
```

## 文档

| 文档 | 内容 |
|------|------|
| `docs/DESIGN.md` | 总体架构、设计决策、生命周期 / Intent / 分区表 |
| `docs/板级接入文档.md` | 新增一块板的完整流程 |
| `docs/主题系统设计.md` | 调色板、字体、样式、`SF_UI()` 缩放 |
| `docs/手势与导航设计.md` | 边缘手势、沉浸式、层级（top layer） |
| `docs/引脚分配文档.md` | 板级 GPIO 分配与驱动实现状态 |

## 目录结构

```
boards/<name>/         板级组件（Kconfig + sdkconfig.defaults）—— 唯一差异点
components/sf_hal/     驱动层（display / input / board / core）
components/sf_sys/     系统层（AppManager / EventBus / Intent / Notification / WiFi / NTP）
components/sf_config/  配置持久化（SPIFFS JSON）
components/sf_gui/     GUI 层（Phone Shell / 主题 / 缩放）
apps/settings/         设置 App（WiFi / 蓝牙 / 本地化 / 设备信息）
apps/monitor/          监控 App（Tasks/Regions/Storage 三 Tab）
main/                  入口 app_main
```
