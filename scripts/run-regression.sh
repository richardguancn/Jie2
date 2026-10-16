#!/usr/bin/env bash
# 「截2」Linux aarch64 全量回归（容器内运行）
#
# 每项独立超时（timeout），任何一项卡死不会拖垮整轮：
#   - 非交互项直接跑；
#   - E2E 项各自起 Xvfb/dbus，跑完自动清理。
#
# 用法（容器内）：bash scripts/run-regression.sh [A|B|all]
set -u
cd "$(dirname "$0")/.." || exit 1

BATCH="${1:-all}"
PASS=0
FAIL=0
FAILED_ITEMS=""

item() { # 名称 超时秒 命令
    local name="$1"
    local timeout_s="$2"
    shift 2
    local out
    out=$(timeout "$timeout_s" bash -c "$*" 2>&1)
    local rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "  ✓ $name"
        PASS=$((PASS + 1))
    else
        echo "  ✗ $name（rc=$rc）"
        echo "$out" | tail -4 | sed 's/^/      /'
        FAIL=$((FAIL + 1))
        FAILED_ITEMS="$FAILED_ITEMS $name"
    fi
}

echo "===== A 批：非交互测试 ====="
# 头文件检查需要逐个编译（Qt 头巨大）：并行 4 路仍要 ~100 秒，
# 超时给足余量（此前 90 秒曾把该项拖成假失败）。
item "头文件自包含" 300 "bash scripts/check-headers.sh | tail -1 | grep -q 通过"
item "内核单测" 60 "./build-linux/tests/jie2_core_tests | grep -q 'all core tests passed'"
item "标注渲染（12 项）" 90 "xvfb-run -a ./build-linux/src/ui/jie2 --test-annotate /tmp/rg1.png 2>/dev/null | grep -q '12 items'"
item "贴图变换" 90 "xvfb-run -a ./build-linux/src/ui/jie2 --test-pin /tmp/rg2 2>/dev/null | grep -q 'saved'"
item "贴图分组" 90 "xvfb-run -a ./build-linux/src/ui/jie2 --test-pin-groups 2>/dev/null | grep -q '== PASS =='"
item "OCR（tesseract）" 90 "xvfb-run -a ./build-linux/src/ui/jie2 --test-ocr tesseract 2>/dev/null | grep -q 'verification passed'"
item "条码（zbar）" 90 "xvfb-run -a ./build-linux/src/ui/jie2 --test-barcode 2>/dev/null | grep -q 'verification passed'"

if [ "$BATCH" = "all" ] || [ "$BATCH" = "B" ]; then
    echo "===== B 批：E2E 测试 ====="
    item "交互链路 E2E" 120 "bash scripts/test-interactive-linux.sh | grep -q '^PASS'"
    item "窗口吸附 E2E" 120 "bash scripts/test-snap-linux.sh | grep -q '^PASS'"
    item "衣架 E2E" 120 "bash scripts/test-clothesline-linux.sh | grep -q '^PASS'"
    item "衣架动画逐帧" 120 "bash scripts/verify-clothesline-animation.sh | grep -q '^PASS'"
    item "M6 CLI" 120 "bash scripts/test-cli-linux.sh | grep -q '^PASS'"
    item "调色板配置" 120 "bash scripts/test-palette-linux.sh | grep -q '^PASS'"
    item "Wayland 热键门户" 120 "bash scripts/test-wayland-portal.sh | grep -q '^PASS'"
    item "Wayland 截图门户" 120 "bash scripts/test-wayland-capture.sh | grep -q '^PASS'"
fi

if [ "$BATCH" = "all" ]; then
    echo "===== C 批：打包 ====="
    item "deb 打包" 120 "bash scripts/build-deb.sh build-linux dist | grep -q '完成'"
    item "AppImage 打包" 120 "bash scripts/build-appimage.sh build-linux dist | grep -q '完成'"
fi

echo
echo "===== 汇总：$PASS 通过 / $FAIL 失败 ====="
if [ "$FAIL" -ne 0 ]; then
    echo "失败项:$FAILED_ITEMS"
    exit 1
fi
exit 0
