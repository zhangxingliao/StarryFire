#!/usr/bin/env bash
# StarryFire 开发环境导出脚本（bash / zsh 兼容）
#
# 用法:
#   source export.sh                # 沿用已导出的 SF_BOARD；否则自动探测唯一板子
#   source export.sh <board>        # 指定/切换板子（如 esp32s3-touch-lcd-2_8）
#
# 导出的环境变量:
#   ESP_IDF_PATH   ESP-IDF 安装路径
#   SF_BOARD       当前编译的板子（与根 CMakeLists.txt 的板级选择一致）

# bash 用 BASH_SOURCE[0]，zsh 用 $0（source 时指向被加载的文件）
_SRC="${BASH_SOURCE[0]}"
[ -z "$_SRC" ] && _SRC="$0"
PROJECT_DIR="$(cd "$(dirname "$_SRC")" && pwd)"
ESP_IDF_PATH="$HOME/libs/esp/esp-idf-v5.4.3"

# ---------- 板子选择 ----------
# 优先级: 命令行参数 > 已导出的 SF_BOARD > 自动探测（boards/ 下唯一板）
if [ $# -ge 1 ]; then
    SF_BOARD="$1"
fi

if [ -z "${SF_BOARD:-}" ]; then
    _found=0
    _candidate=""
    for _d in "$PROJECT_DIR"/boards/*/; do
        if [ -d "$_d" ]; then
            _found=$((_found + 1))
            _candidate="$(basename "$_d")"
        fi
    done
    if [ "$_found" -eq 1 ]; then
        SF_BOARD="$_candidate"
    elif [ "$_found" -eq 0 ]; then
        echo "Error: no board found in boards/" >&2
        return 1 2>/dev/null || exit 1
    else
        echo "Error: ${_found} boards found in boards/, auto-detection needs exactly one." >&2
        echo "       use: source export.sh <board>" >&2
        return 1 2>/dev/null || exit 1
    fi
fi

if [ ! -f "$PROJECT_DIR/boards/$SF_BOARD/sdkconfig.defaults" ]; then
    echo "Error: board '$SF_BOARD' not found: boards/$SF_BOARD missing or without sdkconfig.defaults" >&2
    return 1 2>/dev/null || exit 1
fi

export SF_BOARD
export ESP_IDF_PATH

echo "SF_BOARD=$SF_BOARD"
echo "ESP_IDF_PATH=$ESP_IDF_PATH"

# ---------- 加载 ESP-IDF 环境 ----------
pushd "$ESP_IDF_PATH" >/dev/null
    . ./export.sh
popd >/dev/null
