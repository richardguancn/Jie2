#!/usr/bin/env bash
# 「J2」Debian 打包脚本（M7）
#
# 产出：jie2_<版本>_<架构>.deb
# 内容：/usr/bin/jie2（GUI）、/usr/bin/jie2-probe（诊断工具）、
#      桌面文件、图标。
#
# 依赖声明（Depends）：从实际链接的系统库生成 —— 用发行版自带的 Qt
# 而不是自带 Qt 运行时，这是评估报告 3.1 对老国产系统（麒麟/统信）的
# 兼容性要求：自带 Qt 会因 glibc 版本差异跑不起来。
#
# 用法（在构建目录已存在的前提下）：
#   bash scripts/build-deb.sh [build-dir] [output-dir]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-$ROOT/build-linux}"
OUT_DIR="${2:-$ROOT/dist}"
mkdir -p "$OUT_DIR"
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"
cd "$ROOT"
for tool in dpkg dpkg-deb dpkg-shlibdeps readelf desktop-file-validate; do
    command -v "$tool" >/dev/null || { echo "缺少打包工具：$tool" >&2; exit 1; }
done

# 版本在 project() 的续行里（VERSION 0.1.0），单独一行。
APP_VERSION="$(grep -m1 -E '^[[:space:]]+VERSION [0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt | awk '{print $2}')"
if [ -z "$APP_VERSION" ]; then
    echo "无法从 CMakeLists.txt 解析版本号" >&2
    exit 1
fi

ARCH="$(dpkg --print-architecture)"
PKG="jie2_${APP_VERSION}_${ARCH}"
# 临时目录必须留在 Linux 文件系统中：macOS 挂载的输出目录可能不区分
# 大小写，会把 dpkg 的 debian/ 元数据目录与包内的 DEBIAN/ 混为一处。
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
chmod 0755 "$STAGE"

echo "== 打包 jie2 ${APP_VERSION} (${ARCH}) =="

if [ ! -x "$BUILD_DIR/src/ui/jie2" ]; then
    echo "找不到构建产物：$BUILD_DIR/src/ui/jie2（先构建）" >&2
    exit 1
fi

mkdir -p "$STAGE/DEBIAN" \
         "$STAGE/usr/bin" \
         "$STAGE/usr/share/applications" \
         "$STAGE/usr/share/icons/hicolor"

# 可执行文件
install -m 0755 "$BUILD_DIR/src/ui/jie2" "$STAGE/usr/bin/jie2"
install -m 0755 "$BUILD_DIR/src/tools/jie2-probe" "$STAGE/usr/bin/jie2-probe"
# jie2-line：Wayland layer-shell 顶部条（衣架的 Wayland 实现）。
# 可选：某些构建（无 wayland-client）不产出它；存在才装。
if [ -x "$BUILD_DIR/src/tools/jie2-line" ]; then
    install -m 0755 "$BUILD_DIR/src/tools/jie2-line" "$STAGE/usr/bin/jie2-line"
fi

# 拒绝把其他架构的程序贴上本机架构标签。
case "$ARCH" in
    arm64) MACHINE="AArch64" ;;
    amd64) MACHINE="Advanced Micro Devices X86-64" ;;
    *) echo "尚未支持此打包架构：$ARCH" >&2; exit 1 ;;
esac
for binary in "$STAGE"/usr/bin/*; do
    if ! LC_ALL=C readelf -h "$binary" | grep -Fq "$MACHINE"; then
        echo "构建产物架构与 $ARCH 不符：$binary" >&2
        exit 1
    fi
done

# 桌面入口和同源多尺寸图标；安装、升级和卸载后更新缓存。
desktop-file-validate packaging/jie2.desktop
install -m 0644 packaging/jie2.desktop "$STAGE/usr/share/applications/jie2.desktop"
for size in 16 32 48 64 128 256; do
    bash packaging/make-icon.sh "$STAGE/usr/share/icons/hicolor/${size}x${size}/apps/jie2.png" "$size"
done
mkdir -p "$STAGE/usr/share/icons/hicolor/scalable/apps"
install -m 0644 assets/brand/app-icon.svg "$STAGE/usr/share/icons/hicolor/scalable/apps/jie2.svg"
install -m 0755 packaging/debian/postinst "$STAGE/DEBIAN/postinst"
install -m 0755 packaging/debian/postrm "$STAGE/DEBIAN/postrm"

# dpkg-shlibdeps 计算真实依赖和最低版本，包含 libstdc++、XRandR、D-Bus、
# Wayland 等可选组件。缺库时直接失败，禁止用 Qt5/Qt6 二选一掩盖错误。
mkdir -p "$STAGE/debian"
cat > "$STAGE/debian/control" <<EOF
Source: jie2
Section: utils
Priority: optional
Maintainer: jie2

Package: jie2
Architecture: any
Description: Screenshot tool
EOF
QT_DEPS="$(cd "$STAGE" && dpkg-shlibdeps -O usr/bin/*)"
QT_DEPS="${QT_DEPS#shlibs:Depends=}"
if [ -z "$QT_DEPS" ]; then
    echo "未能生成运行依赖" >&2
    exit 1
fi
if readelf -d "$BUILD_DIR/src/ui/jie2" | grep -q 'libQt5Widgets'; then
    # SVG 插件在运行时加载，不会出现在 ELF 的直接依赖中。
    QT_DEPS="$QT_DEPS, libqt5svg5 (>= 5.12), libglib2.0-bin"
fi
if [ -n "${JIE2_DEB_EXTRA_DEPENDS:-}" ]; then
    QT_DEPS="$QT_DEPS, $JIE2_DEB_EXTRA_DEPENDS"
fi
rm -rf "$STAGE/debian"
INSTALLED_SIZE="$(du -sk "$STAGE/usr" | awk '{print $1}')"

cat > "$STAGE/DEBIAN/control" <<EOF
Package: jie2
Version: ${APP_VERSION}
Section: utils
Priority: optional
Architecture: ${ARCH}
Installed-Size: ${INSTALLED_SIZE}
Depends: ${QT_DEPS}
Maintainer: jie2
Suggests: tesseract-ocr, tesseract-ocr-chi-sim, zbar-tools
Description: J2 — 截图 / 标注 / 贴图工具
 跨平台截图工具：区域/全屏截图、标注（矩形/椭圆/箭头/铅笔/马克笔/
 马赛克/模糊/文字/序号）、屏幕贴图、截图衣架、可插拔 OCR。
 本包含 GUI 主程序 jie2 与诊断工具 jie2-probe。
EOF

echo "== 构建 deb =="
dpkg-deb --build --root-owner-group "$STAGE" "$OUT_DIR/${PKG}.deb"
echo "== 完成: $OUT_DIR/${PKG}.deb =="
ls -lh "$OUT_DIR/${PKG}.deb"
