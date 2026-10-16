#!/usr/bin/env bash
# 「截2」Linux aarch64 验证脚本
#
# 在 Linux ARM64 容器内构建并验证三层结构：
#   - 构建 core / platform / tools / tests（+ Qt UI 层，若依赖可装）
#   - 跑单元测试
#   - 跑平台探测（应报告 X11 会话与外设能力）
#   - 在 Xvfb 虚拟显示下实测 XGrabKey 热键注册
#
# 用法（在宿主机执行）：
#   docker run --rm -v "$PWD":/work -w /work ubuntu:24.04 \
#       bash scripts/verify-linux-aarch64.sh

set -uo pipefail

echo "=== [1/6] 环境 ==="
uname -m
grep -E '^(PRETTY_NAME|VERSION_CODENAME)=' /etc/os-release || true
nproc

echo
echo "=== [2/6] 安装依赖 ==="
apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    build-essential cmake \
    qt6-base-dev \
    libx11-dev \
    xvfb \
    2>&1 | tail -3
echo "依赖安装完成"

echo
echo "=== [3/6] 配置 ==="
rm -rf build-linux
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=RelWithDebInfo 2>&1 | tail -12

echo
echo "=== [4/6] 构建 ==="
cmake --build build-linux -j"$(nproc)" 2>&1 | tail -15

echo
echo "=== [5/6] 单元测试 ==="
(cd build-linux && ctest --output-on-failure)

echo
echo "=== [6/6] 平台探测与热键实测 ==="
echo "--- 纯探测（无 X 显示，预期 session=unknown 或按 DISPLAY 判定）---"
./build-linux/src/tools/jie2-probe || true

echo
echo "--- Xvfb 虚拟 X11 下的热键实测 ---"
xvfb-run -a ./build-linux/src/tools/jie2-probe --test-hotkey || true

echo
echo "=== 验证结束 ==="
