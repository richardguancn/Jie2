#!/usr/bin/env bash
# 「截2」Debian 打包脚本（M7）
#
# 产出：jie2_<版本>_<架构>.deb
# 内容：/usr/bin/jie2（GUI）、/usr/bin/jie2-probe（诊断工具）、
#      桌面文件、图标。
#
# 依赖声明（Depends）：qt6-base 运行库 + libx11-6 —— 用发行版自带的 Qt
# 而不是自带 Qt 运行时，这是评估报告 3.1 对老国产系统（麒麟/统信）的
# 兼容性要求：自带 Qt 会因 glibc 版本差异跑不起来。
#
# 用法（在构建目录已存在的前提下）：
#   bash scripts/build-deb.sh [build-dir] [output-dir]
set -eu

BUILD_DIR="${1:-build-linux}"
OUT_DIR="${2:-dist}"
WORK="$(pwd)"

# 版本在 project() 的续行里（VERSION 0.1.0），单独一行。
APP_VERSION="$(grep -m1 -E '^[[:space:]]+VERSION [0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt | awk '{print $2}')"
if [ -z "$APP_VERSION" ]; then
    echo "无法从 CMakeLists.txt 解析版本号" >&2
    exit 1
fi

ARCH="$(dpkg --print-architecture)"
PKG="jie2_${APP_VERSION}_${ARCH}"
STAGE="$WORK/$OUT_DIR/$PKG"

echo "== 打包 jie2 ${APP_VERSION} (${ARCH}) =="

if [ ! -x "$BUILD_DIR/src/ui/jie2" ]; then
    echo "找不到构建产物：$BUILD_DIR/src/ui/jie2（先构建）" >&2
    exit 1
fi

rm -rf "$STAGE"
mkdir -p "$STAGE/DEBIAN" \
         "$STAGE/usr/bin" \
         "$STAGE/usr/share/applications" \
         "$STAGE/usr/share/icons/hicolor/256x256/apps"

# 可执行文件
install -m 0755 "$BUILD_DIR/src/ui/jie2" "$STAGE/usr/bin/jie2"
install -m 0755 "$BUILD_DIR/src/tools/jie2-probe" "$STAGE/usr/bin/jie2-probe"
# jie2-line：Wayland layer-shell 顶部条（衣架的 Wayland 实现）。
# 可选：某些构建（无 wayland-client）不产出它；存在才装。
if [ -x "$BUILD_DIR/src/tools/jie2-line" ]; then
    install -m 0755 "$BUILD_DIR/src/tools/jie2-line" "$STAGE/usr/bin/jie2-line"
fi

# 桌面入口与图标
install -m 0644 packaging/jie2.desktop "$STAGE/usr/share/applications/jie2.desktop"
# 图标：与程序内托盘图标同源，由公共脚本生成（deb 与 AppImage 共用）。
# 注意不要把 arch 命名（arm64 vs aarch64）写进路径判断 —— 两个打包脚本
# 的 arch 取值来源不同，早期版本因此互相找不到对方生成的图标。
bash packaging/make-icon.sh "$STAGE/usr/share/icons/hicolor/256x256/apps/jie2.png" 256

# control 文件
INSTALLED_SIZE="$(du -sk "$STAGE/usr" | awk '{print $1}')"
cat > "$STAGE/DEBIAN/control" <<EOF
Package: jie2
Version: ${APP_VERSION}
Section: utils
Priority: optional
Architecture: ${ARCH}
Installed-Size: ${INSTALLED_SIZE}
Depends: libqt6widgets6 | qt6-base, libqt6gui6, libqt6core6, libqt6network6, libx11-6, libc6
Maintainer: jie2
Description: 截2 — 截图 / 标注 / 贴图工具
 跨平台截图工具：区域/全屏截图、标注（矩形/椭圆/箭头/铅笔/马克笔/
 马赛克/模糊/文字/序号）、屏幕贴图、截图衣架、可插拔 OCR。
 本包含 GUI 主程序 jie2 与诊断工具 jie2-probe。
EOF

echo "== 构建 deb =="
dpkg-deb --build --root-owner-group "$STAGE" "$WORK/$OUT_DIR/${PKG}.deb"
echo "== 完成: $OUT_DIR/${PKG}.deb =="
ls -lh "$WORK/$OUT_DIR/${PKG}.deb"
