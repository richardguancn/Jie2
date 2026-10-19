#!/bin/bash
# 「J2」Windows 自包含打包脚本（MSYS2 / MinGW-w64，UCRT 运行时）
# 作用：把 Qt 运行库与插件收进同一个文件夹并打 zip，
#       目标机器无需安装 Qt（与 macOS 包同一分发思路）。
# 用法：scripts/package-windows.sh [构建目录] [输出目录]
# 前提：已构建过（产物 <构建目录>/src/ui/jie2.exe），且在 MSYS2 的
#       UCRT64/MINGW64 shell 里运行（需要 windeployqt6、objdump）。
# 产出：<输出目录>/jie2-<版本>-windows-x64.zip
#
# 设计取舍：
#   1) 在**副本**上部署，不动构建目录。理由同 package-macos.sh：
#      部署会把 Qt 的 DLL 塞到 exe 旁边，之后重新链接的二进制会重新
#      引用 MSYS2 的 /ucrt64 路径，"这份包到底自不自包含"就无法判断。
#   2) 部署完做**导入表核对**：exe 引用的每个 DLL，要么在包内，
#      要么是 Windows 自带系统 DLL；否则直接失败。这比"能打包"强一层。
set -euo pipefail

BUILD_DIR="${1:-build-windows}"
OUT_DIR="${2:-dist}"

EXE_SRC="$BUILD_DIR/src/ui/jie2.exe"
PROBE_SRC="$BUILD_DIR/src/tools/jie2-probe.exe"
VERSION="$(grep -m1 -E '^[[:space:]]+VERSION [0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt | awk '{print $2}')"
# 产物名里的架构随工具链走：本脚本只处理 64 位（x86_64/x86_64-w64-mingw32）。
ARCH="x64"

if [ ! -f "$EXE_SRC" ]; then
    echo "错误：找不到 $EXE_SRC，请先构建（cmake --build $BUILD_DIR）" >&2
    exit 1
fi

# windeployqt 的来源：优先 $WINDEPLOYQT，否则按 Qt6 常见命名找。
WINDEPLOYQT="${WINDEPLOYQT:-}"
if [ -z "$WINDEPLOYQT" ]; then
    for cand in windeployqt6 windeployqt-qt6 windeployqt; do
        if command -v "$cand" > /dev/null 2>&1; then
            WINDEPLOYQT="$(command -v "$cand")"
            break
        fi
    done
fi
if [ -z "$WINDEPLOYQT" ] || [ ! -x "$WINDEPLOYQT" ]; then
    echo "错误：找不到 windeployqt（当前取值：${WINDEPLOYQT:-空}）。" >&2
    echo "  MSYS2 安装：pacman -S mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-qt6-tools" >&2
    echo "  或用 WINDEPLOYQT=/路径/windeployqt6 指定。" >&2
    exit 1
fi

NAME="jie2-${VERSION}-windows-${ARCH}"
STAGE="$OUT_DIR/jie2-windows-stage"
PKG="$STAGE/$NAME"
# zip 与 stage 都要绝对路径：脚本末尾会 cd 进 stage 再压缩，
# 相对路径会指到别处（macOS 打包脚本踩过同一个坑）。
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"
STAGE="$OUT_DIR/jie2-windows-stage"
PKG="$STAGE/$NAME"
ZIP="$OUT_DIR/$NAME.zip"

echo "==> 0/5 复制到打包暂存区（不动构建目录）"
rm -rf "$STAGE"
mkdir -p "$PKG"
cp "$EXE_SRC" "$PKG/jie2.exe"
if [ -f "$PROBE_SRC" ]; then
    # jie2-probe 不链接 Qt，只依赖核心层与平台层；随包一起发，
    # 便于用户在现场跑能力探测（--displays / --capture 等）。
    cp "$PROBE_SRC" "$PKG/jie2-probe.exe"
else
    echo "  注意：没有 jie2-probe.exe，包内只含主程序"
fi

echo "==> 1/5 收集 Qt 依赖进包（windeployqt）"
# 用 --release：构建是 Release，否则 windeployqt 会按调试版挑 DLL。
# 插件（platforms/qwindows.dll、imageformats/qjpeg.dll、styles）由它自动判定；
# jie2 只用 PNG（brand.qrc 里没有 svg），因此不额外需要 qsvg。
"$WINDEPLOYQT" --release "$PKG/jie2.exe"

# 使用与部署工具相同的 Qt / MinGW 前缀，递归收集全部非系统依赖。
RUNTIME_DIR="$(dirname "$WINDEPLOYQT")"
# OpenSSL 可能由 Qt 在运行时加载，不一定出现在 PE 导入表中。
for lib in "$RUNTIME_DIR"/libssl*.dll "$RUNTIME_DIR"/libcrypto*.dll; do
    [ ! -f "$lib" ] || cp "$lib" "$PKG/"
done

# 本机 Qt6 的 translations 目录若为空（MSYS2 的 qt6 不打 catalogs.json），
# windeployqt 会留下一个空目录并告警。空目录留着会让人误以为缺东西，删掉。
rmdir "$PKG/translations" 2>/dev/null || true

echo "==> 2/5 校验：必需的 Qt 模块与插件是否都在包内"
REQUIRED=(
    "Qt6Core.dll"
    "Qt6Gui.dll"
    "Qt6Widgets.dll"
    "Qt6Network.dll"
    "platforms/qwindows.dll"
)
missing=""
for item in "${REQUIRED[@]}"; do
    if [ ! -e "$PKG/$item" ]; then
        missing="$missing $item"
    fi
done
if [ -n "$missing" ]; then
    echo "失败：包内缺少必需文件：$missing" >&2
    echo "当前包内文件：" >&2
    ( cd "$PKG" && find . -type f | sort | sed 's/^/  /' ) >&2
    exit 1
fi
echo "  必需模块齐全（Qt6 Core/Gui/Widgets/Network + qwindows 平台插件）"

echo "==> 3/5 校验：导入表里的每个 DLL 必须能落到包内或系统 DLL"
OBJDUMP=""
for cand in objdump x86_64-w64-mingw32-objdump; do
    if command -v "$cand" > /dev/null 2>&1; then
        OBJDUMP="$cand"
        break
    fi
done
if [ -z "$OBJDUMP" ]; then
    echo "失败：找不到 objdump，无法核对导入表（MSYS2: pacman -S binutils）" >&2
    exit 1
fi

python scripts/windows-dependencies.py "$PKG" --runtime-dir "$RUNTIME_DIR" --objdump "$OBJDUMP"
python scripts/windows-dependencies.py "$PKG" --objdump "$OBJDUMP" --audit-only

echo "==> 4/5 附上说明文件"
cat > "$PKG/README.txt" <<EOF
J2 ${VERSION}（Windows x64）

运行：双击 jie2.exe。程序启动后常驻托盘，不出主窗口。
卸载：直接删除本文件夹（程序不写注册表、不装服务）。
配置与截图历史位置：%APPDATA%\\jie2\\

自检：jie2-probe.exe 打印本机平台能力
  jie2-probe.exe              能力矩阵
  jie2-probe.exe --displays   显示器枚举
  jie2-probe.exe --capture    抓全屏并输出尺寸
  jie2-probe.exe --windows    可吸附窗口矩形

说明：
- 本包已用 windeployqt 收齐 Qt6 运行库与插件，目标机器不需要装 Qt。
- 尚未在真实 Windows 上做完整人机交互验证（见发布说明「已验证范围」）。
EOF

echo "==> 5/5 打 zip"
rm -f "$ZIP"
( cd "$STAGE" && zip -r -q "$ZIP" "$NAME" )
rm -rf "$STAGE"

echo
echo "完成："
ls -lh "$ZIP"
echo
echo "包内清单（前 40 项）："
unzip -l "$ZIP" | awk 'NR>3 && NR<=43 {print "  " $4}'
echo
echo "包内体积统计："
unzip -l "$ZIP" | tail -2
