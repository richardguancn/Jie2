#!/usr/bin/env bash
# 衣架窗口固定，动画只改变内容：检查中间帧、最终位置和反复展开不漂移。
# 用法：bash scripts/verify-clothesline-animation.sh [build目录]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-$ROOT/build-linux}"
TEST="$BUILD_DIR/tests/jie2_ui_alignment_tests"
if [ ! -x "$TEST" ]; then
    echo "找不到界面测试程序：$TEST；请启用测试并安装 Qt Test 后重新构建。" >&2
    exit 1
fi
# 默认离屏验证；真实桌面可显式设置 QT_QPA_PLATFORM=xcb 或 cocoa。
QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}" "$TEST" \
    clotheslineWindowDoesNotDrift revealDwellAndLeaveDelay
