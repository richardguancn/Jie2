#!/bin/bash
# jie2 macOS 自包含打包脚本
# 作用：把 Qt 框架与插件收进 jie2.app，使目标机器无需安装 qt / qtbase（与 Snipaste 分发方式一致）
# 用法：scripts/package-macos.sh [build目录]
# 前提：已用 cmake -S . -B build-macos 构建过（产物 build-macos/src/app/jie2.app）
set -euo pipefail

BUILD_DIR="${1:-build-macos}"
APP="$BUILD_DIR/src/ui/jie2.app"
MACDEPLOYQT="$(brew --prefix qtbase)/bin/macdeployqt"

if [ ! -d "$APP" ]; then
    echo "错误：找不到 $APP，请先构建（cmake --build $BUILD_DIR）" >&2
    exit 1
fi

echo "==> 1/3 收集 Qt 依赖进包（macdeployqt）"
"$MACDEPLOYQT" "$APP" -always-overwrite

echo "==> 2/3 ad-hoc 重签名"
codesign --force --deep --sign - "$APP"

echo "==> 3/3 验证：可执行文件不得再引用 /opt/homebrew 的 Qt"
if otool -L "$APP/Contents/MacOS/jie2" | grep -q '/opt/homebrew'; then
    echo "失败：仍存在 Homebrew 依赖：" >&2
    otool -L "$APP/Contents/MacOS/jie2" | grep '/opt/homebrew' >&2
    exit 1
fi
echo "完成：$APP 已自包含（Qt 在 Contents/Frameworks，插件在 Contents/PlugIns）"
