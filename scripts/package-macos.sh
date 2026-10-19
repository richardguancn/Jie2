#!/bin/bash
# jie2 macOS 自包含打包脚本
# 作用：把 Qt 框架与插件收进 jie2.app，使目标机器无需安装 qt / qtbase（与 Snipaste 分发方式一致）
# 用法：scripts/package-macos.sh [build目录] [输出目录]
# 前提：已用 cmake -S . -B build-macos 构建过（产物 build-macos/src/ui/jie2.app）
# 产出：<输出目录>/jie2-<版本>-macos-<架构>.zip
#       <输出目录>/jie2-<版本>-macos-<架构>.dmg（拖拽安装用）
#
# 设计取舍：**在副本上打包，不改构建目录**。
#   早期版本直接对 build-macos/src/ui/jie2.app 跑 macdeployqt，结果构建目录
#   被塞进 Frameworks 与 PlugIns —— 之后重新链接的二进制会重新引用 Homebrew
#   路径，而包内残留的框架让"这份 app 到底自不自包含"变得无法判断。
set -euo pipefail

BUILD_DIR="${1:-build-macos}"
OUT_DIR="${2:-dist}"
APP_SRC="$BUILD_DIR/src/ui/jie2.app"

# macdeployqt 来源：优先取 $MACDEPLOYQT（CI 里 Qt 由 brew 装在别的前缀时用），
# 否则依次尝试 qtbase（拆分后的主模块）与完整 qt 公式。
MACDEPLOYQT="${MACDEPLOYQT:-}"
if [ -z "$MACDEPLOYQT" ] && command -v brew > /dev/null 2>&1; then
    for formula in qtbase qt; do
        CANDIDATE="$(brew --prefix "$formula" 2> /dev/null || true)/bin/macdeployqt"
        if [ -x "$CANDIDATE" ]; then
            MACDEPLOYQT="$CANDIDATE"
            break
        fi
    done
fi
STAGE="$OUT_DIR/jie2-macos-stage"
APP="$STAGE/jie2.app"
VERSION="$(grep -m1 -E '^[[:space:]]+VERSION [0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt | awk '{print $2}')"
ARCH="$(uname -m)"
# 绝对路径：打包要在 $STAGE 里 cd 后执行 ditto，相对路径会被写到暂存区里，
# 随暂存区一起被删掉（真实踩过）。
WORK="$(pwd)"
ZIP="$WORK/$OUT_DIR/jie2-${VERSION}-macos-${ARCH}.zip"
DMG="$WORK/$OUT_DIR/jie2-${VERSION}-macos-${ARCH}.dmg"
VOLNAME="J2 ${VERSION}"

if [ ! -d "$APP_SRC" ]; then
    echo "错误：找不到 $APP_SRC，请先构建（cmake --build $BUILD_DIR）" >&2
    exit 1
fi
if [ ! -x "$MACDEPLOYQT" ]; then
    echo "错误：找不到 macdeployqt（当前取值：${MACDEPLOYQT:-空}）。" >&2
    echo "请先 brew install qtbase，或用 MACDEPLOYQT=/路径/bin/macdeployqt 指定。" >&2
    exit 1
fi

echo "==> 0/6 复制到打包暂存区（不动构建目录）"
rm -rf "$STAGE"
mkdir -p "$STAGE"
ditto "$APP_SRC" "$APP"

echo "==> 1/6 注入应用图标"
# 图标由 packaging/make-icon.sh 生成（与程序内托盘图标同源）。
ICONSET="$STAGE/AppIcon.iconset"
# Contents/Resources 在 Qt 生成的 bundle 里并不存在 —— 不先建，
# iconutil 会以 "Failed to generate ICNS" 失败（真实踩过）。
mkdir -p "$ICONSET" "$APP/Contents/Resources"
for spec in "16 16x16" "32 16x16@2x" "32 32x32" "64 32x32@2x" \
            "128 128x128" "256 128x128@2x" "256 256x256" "512 256x256@2x" \
            "512 512x512" "1024 512x512@2x"; do
    px="${spec%% *}"
    name="${spec##* }"
    bash packaging/make-icon.sh "$ICONSET/icon_${name}.png" "$px" > /dev/null
done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns"
rm -rf "$ICONSET"
# Info.plist 里 CFBundleIconFile 由 Qt 生成为空串：不设它，装到 /Applications
# 会显示成白纸图标。
/usr/libexec/PlistBuddy -c "Set :CFBundleIconFile AppIcon" "$APP/Contents/Info.plist"

echo "==> 2/6 收集 Qt 依赖进包（macdeployqt）"
"$MACDEPLOYQT" "$APP" -always-overwrite

echo "==> 3/6 ad-hoc 重签名（由内向外）"
# macdeployqt 改写了二进制的引用，原签名失效 —— 不重签会被系统 SIGKILL。
find "$APP/Contents/Frameworks" -maxdepth 1 -name "*.framework" \
    -exec codesign --force --sign - {} \; 2>/dev/null || true
find "$APP/Contents/PlugIns" -name "*.dylib" \
    -exec codesign --force --sign - {} \; 2>/dev/null || true
codesign --force --sign - "$APP"

echo "==> 4/6 验证：可执行文件不得再引用 /opt/homebrew"
if otool -L "$APP/Contents/MacOS/jie2" | grep -q '/opt/homebrew'; then
    echo "失败：仍存在 Homebrew 依赖：" >&2
    otool -L "$APP/Contents/MacOS/jie2" | grep '/opt/homebrew' >&2
    exit 1
fi
codesign --verify --strict "$APP" && echo "  签名校验通过"

# 外层应用目录沿用构建目录的旧时间会误导 Finder；按本次打包时间刷新。
touch "$APP"

echo "==> 5/6 打 zip（ditto，保留符号链接与扩展属性）"
rm -f "$ZIP"
( cd "$STAGE" && ditto -c -k --sequesterRsrc --keepParent jie2.app "$ZIP" )

echo "==> 6/6 打 dmg（拖拽安装：jie2.app + Applications 链接）"
DMG_STAGE="$OUT_DIR/jie2-macos-dmg"
# 同名卷若已挂载，hdiutil create 会失败 —— 先卸载（真实踩过）。
if [ -d "/Volumes/$VOLNAME" ]; then
    hdiutil detach "/Volumes/$VOLNAME" > /dev/null 2>&1 || true
fi
rm -rf "$DMG_STAGE"
mkdir -p "$DMG_STAGE"
# ditto 而非 cp -R：保留 bundle 内的符号链接与扩展属性（框架里全是
# Versions/A 的软链，用 cp -R 会展开成实体副本，包体积暴涨）。
ditto "$APP" "$DMG_STAGE/jie2.app"
# 「拖到这里安装」的目标：系统 /Applications 目录的软链。
ln -s /Applications "$DMG_STAGE/Applications"
rm -f "$DMG"
# HFS+ 卷：兼容性最好（APFS 格式的 dmg 在旧系统上打不开）。
# -format UDZO 是压缩只读，发布标准格式。
hdiutil create -volname "$VOLNAME" -srcfolder "$DMG_STAGE" \
    -fs HFS+ -format UDZO -ov "$DMG" > /dev/null
rm -rf "$DMG_STAGE" "$STAGE"
# dmg 自身也 ad-hoc 签名：不签也能用，签了在"校验来源"时少一层警告。
codesign --force --sign - "$DMG" 2>/dev/null || true

echo
echo "完成："
ls -lh "$ZIP" "$DMG"
echo
# 校验 dmg 完整性（CRC 校验）：写完就验，避免交付一个坏镜像。
if hdiutil verify "$DMG" > /dev/null 2>&1; then
    echo "dmg 完整性校验通过"
else
    echo "失败：dmg 完整性校验不通过" >&2
    exit 1
fi
echo "包内 Qt 框架："
unzip -l "$ZIP" | grep -v "__MACOSX" | awk '/Frameworks\/Qt.*\.framework\/Versions\/A\/Qt[A-Za-z]*$/{print $4}' \
    | sed 's|.*Frameworks/||' | sort -u | sed 's/^/  /'
echo "包内插件组："
unzip -l "$ZIP" | grep -v "__MACOSX" | awk '{print $4}' | grep "PlugIns/" \
    | cut -d/ -f4 | sort -u | sed 's/^/  /'
