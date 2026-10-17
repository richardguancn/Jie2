#!/usr/bin/env bash
# 「截2」AppImage 打包（简化自解压格式）
#
# 设计说明（为什么不是标准 type-2 AppImage）：
#   标准 AppImage 需要 appimagetool + 预编译 runtime，运行时依赖 FUSE
#   （libfuse2）—— 而目标场景（麒麟/统信等国产 aarch64 系统）常缺 FUSE。
#   这里采用**自解压单文件**：POSIX shell 头 + tar.gz 尾，零外部工具、
#   零 FUSE 依赖；首次运行解压到临时目录（本应用 ~4MB，开销可忽略）。
#   代价：不是标准格式（不能挂载），换来的是"拷过去就能跑"。
#
# 产物：<out-dir>/jie2-<版本>-<架构>.AppImage（可执行 chmod +x）
#
# 用法：bash scripts/build-appimage.sh [build-dir] [output-dir]
set -eu

BUILD_DIR="${1:-build-linux}"
OUT_DIR="${2:-dist}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# 版本从 CMakeLists.txt 解析，与 deb / rpm / macOS 打包统一来源
# （早期这里是硬编码，版本一改就会产出名不符实的包）。
VERSION="$(grep -m1 -E '^[[:space:]]+VERSION [0-9]+\.[0-9]+\.[0-9]+' "$ROOT/CMakeLists.txt" | awk '{print $2}')"
if [ -z "$VERSION" ]; then
    echo "错误：无法从 CMakeLists.txt 解析版本号" >&2
    exit 1
fi
ARCH="$(uname -m)"

JIE2_BIN="$ROOT/$BUILD_DIR/src/ui/jie2"
PROBE_BIN="$ROOT/$BUILD_DIR/src/tools/jie2-probe"
[ -x "$JIE2_BIN" ] || { echo "错误：找不到 $JIE2_BIN（先构建）" >&2; exit 1; }
[ -x "$PROBE_BIN" ] || { echo "错误：找不到 $PROBE_BIN（先构建）" >&2; exit 1; }

APP_DIR="$OUT_DIR/jie2.AppDir"
OUT_FILE="$OUT_DIR/jie2-$VERSION-$ARCH.AppImage"

# ---- 1) 组装 AppDir ----
rm -rf "$APP_DIR"
mkdir -p "$APP_DIR/usr/bin"
mkdir -p "$APP_DIR/usr/share/applications"
mkdir -p "$APP_DIR/usr/share/icons/hicolor/256x256/apps"

install -m 0755 "$JIE2_BIN" "$APP_DIR/usr/bin/jie2"
install -m 0755 "$PROBE_BIN" "$APP_DIR/usr/bin/jie2-probe"
# jie2-line（Wayland 顶部条）：可选，存在才放入。
if [ -x "$ROOT/$BUILD_DIR/src/tools/jie2-line" ]; then
    install -m 0755 "$ROOT/$BUILD_DIR/src/tools/jie2-line" "$APP_DIR/usr/bin/jie2-line"
fi

cat > "$APP_DIR/jie2.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Version=1.0
Name=截2
Name[en]=Jie2
Comment=截图 / 标注 / 贴图工具
Comment[en]=Screenshot, annotate and pin tool
Exec=jie2
Icon=jie2
Terminal=false
Categories=Utility;Graphics;
Keywords=screenshot;capture;annotate;snip;截图;标注;
StartupNotify=false
EOF
cp "$APP_DIR/jie2.desktop" "$APP_DIR/usr/share/applications/jie2.desktop"

# 图标：脚本生成（与托盘图标同源几何，见 packaging/make-icon.sh）。
bash "$ROOT/packaging/make-icon.sh" "$APP_DIR/usr/share/icons/hicolor/256x256/apps/jie2.png" 256
cp "$APP_DIR/usr/share/icons/hicolor/256x256/apps/jie2.png" "$APP_DIR/jie2.png"

cat > "$APP_DIR/AppRun" <<'EOF'
#!/bin/sh
# 「截2」AppImage 入口
HERE="$(dirname "$(readlink -f "$0")")"
export PATH="$HERE/usr/bin:$PATH"

# 预检：Qt6 运行库来自宿主发行版。缺失时给出明确指引，
# 而不是让二进制自己抛 "error while loading shared libraries"。
missing=""
for lib in libQt6Widgets.so.6 libQt6Gui.so.6 libQt6Core.so.6 libX11.so.6; do
    if ! ldconfig -p 2>/dev/null | grep -q "$lib"; then
        missing="$missing $lib"
    fi
done
if [ -n "$missing" ]; then
    echo "截2 需要宿主提供以下运行库（AppImage 不捆绑 Qt）：" >&2
    echo "  缺失:$missing" >&2
    echo >&2
    echo "Debian/Ubuntu（含麒麟/统信等衍生版）：" >&2
    echo "  sudo apt install libqt6widgets6 libqt6gui6 libqt6core6 libqt6network6 libx11-6" >&2
    echo "Fedora/RHEL：" >&2
    echo "  sudo dnf install qt6-qtbase qt6-qtbase-gui" >&2
    exit 1
fi

exec "$HERE/usr/bin/jie2" "$@"
EOF
chmod +x "$APP_DIR/AppRun"

# ---- 2) 生成自解压头 ----
HEADER="$(mktemp)"
ARCHIVE="$(mktemp)"
trap 'rm -f "$HEADER" "$ARCHIVE"' EXIT

cat > "$HEADER" <<'EOF'
#!/bin/sh
# 「截2」自解压单文件：运行到临时目录后执行（不需要 root / 不需要安装）
set -eu
TMP="$(mktemp -d)"
trap "rm -rf \"$TMP\"" EXIT
ARCHIVE_LINE=$(awk "/^__ARCHIVE_BELOW__/ {print NR + 1; exit 0; }" "$0")
tail -n +$ARCHIVE_LINE "$0" | tar xz -C "$TMP"
exec "$TMP/jie2.AppDir/AppRun" "$@"
exit 0
__ARCHIVE_BELOW__
EOF

# ---- 3) 打包（头 + tar.gz 尾）----
tar czf "$ARCHIVE" -C "$OUT_DIR" jie2.AppDir
cat "$HEADER" "$ARCHIVE" > "$OUT_FILE"
chmod +x "$OUT_FILE"

# ---- 4) 自检：结构、可执行、标记行 ----
[ -x "$OUT_FILE" ] || { echo "自检失败：产物不可执行" >&2; exit 1; }
head -12 "$OUT_FILE" | grep -q '__ARCHIVE_BELOW__' || {
    echo "自检失败：缺少归档标记行" >&2; exit 1; }
SIZE=$(stat -c%s "$OUT_FILE" 2>/dev/null || stat -f%z "$OUT_FILE")
[ "$SIZE" -gt 500000 ] || { echo "自检失败：产物过小（$SIZE 字节）" >&2; exit 1; }

echo "AppImage 打包完成：$OUT_FILE（$SIZE 字节，自解压格式）"
