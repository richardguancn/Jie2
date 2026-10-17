#!/bin/bash
# 「截2」Windows 自包含打包脚本（MSYS2 / MinGW-w64，UCRT 运行时）
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
WORK="$(pwd)"
ZIP="$WORK/$OUT_DIR/$NAME.zip"

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

# MinGW 运行时**不会**被 windeployqt 收进来（实测：Qt6 的 windeployqt 只处理
# MSVC 的 --compiler-runtime，对 MinGW 编译的程序不作处理），而 jie2.exe 的
# 导入表里明确依赖 libgcc_s_seh-1.dll / libstdc++-6.dll / libwinpthread-1.dll。
# 不补这三件，包在没有装 MSYS2 的机器上直接起不来 —— 由下面的导入表核对兜住。
#
# 定位方式按可靠度排三层（MSYS2 踩过的坑：gcc -print-file-name 只在
# 库搜索目录里找，返回的是裸文件名，而这几个 DLL 实际装在 <前缀>/bin 下）：
#   1) 与编译器同一个 bin 目录 —— MSYS2/MinGW 的标准布局；
#   2) gcc -print-file-name（交叉工具链布局可能不同）；
#   3) PATH 里逐目录找 —— 兜底。
CC_BIN_DIR="$(dirname "$(command -v "${CXX:-gcc}")")"
echo "  编译器 bin 目录：$CC_BIN_DIR"
for lib in libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll; do
    src=""
    for cand in "$CC_BIN_DIR/$lib" "$(gcc -print-file-name="$lib" 2>/dev/null || true)"; do
        if [ -n "$cand" ] && [ -f "$cand" ] && [ "$cand" != "$lib" ]; then
            src="$cand"
            break
        fi
    done
    if [ -z "$src" ]; then
        IFS_OLD="$IFS"; IFS=':'
        for dir in $PATH; do
            if [ -f "$dir/$lib" ]; then
                src="$dir/$lib"
                break
            fi
        done
        IFS="$IFS_OLD"
    fi
    if [ -n "$src" ]; then
        cp "$src" "$PKG/"
    else
        echo "错误：找不到 MinGW 运行时 $lib（编译器 bin 目录与 PATH 都没有）" >&2
        exit 1
    fi
done
echo "  已补入 MinGW 运行时（libgcc / libstdc++ / libwinpthread）"

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

# Windows 自带、可安全依赖的系统 DLL（小写比较）。
SYSTEM_DLLS="kernel32.dll user32.dll gdi32.dll shell32.dll advapi32.dll
ole32.dll oleaut32.dll comdlg32.dll comctl32.dll ws2_32.dll shcore.dll
dwmapi.dll imm32.dll winmm.dll uxtheme.dll msimg32.dll d3d11.dll dxgi.dll
d2d1.dll dwrite.dll userenv.dll setupapi.dll version.dll netapi32.dll
wtsapi32.dll iphlpapi.dll bcrypt.dll ncrypt.dll secur32.dll mpr.dll
crypt32.dll wldap32.dll psapi.dll powrprof.dll shlwapi.dll winspool.drv
mf.dll mfplat.dll mfreadwrite.dll avrt.dll bluetoothapis.dll"

unresolved=""
for exe in "$PKG/jie2.exe" "$PKG/jie2-probe.exe"; do
    [ -f "$exe" ] || continue
    echo "  -- $(basename "$exe")"
    while IFS= read -r dll; do
        [ -n "$dll" ] || continue
        lower="$(printf '%s' "$dll" | tr '[:upper:]' '[:lower:]')"
        if [ -f "$PKG/$dll" ]; then
            continue                          # 已随包分发
        fi
        case "$lower" in
            api-ms-win-*|ext-ms-*) continue ;;  # UCRT 转发 DLL，Windows 10+ 自带
        esac
        # SYSTEM_DLLS 是多行 + 行内空格分隔，先拆成一行一个再整行匹配。
        if printf '%s' "$SYSTEM_DLLS" | tr -s ' \n' '\n' | grep -qx -- "$lower"; then
            continue
        fi
        unresolved="$unresolved\n  $(basename "$exe") -> $dll"
    done < <("$OBJDUMP" -p "$exe" | awk '/DLL Name/ {print $3}' | sort -u)
done
if [ -n "$unresolved" ]; then
    echo "失败：以下依赖既不在包内，也不是已知系统 DLL（目标机器会缺库）：" >&2
    printf "%b\n" "$unresolved" >&2
    exit 1
fi
echo "  导入表核对通过：非系统依赖全部在包内"

echo "==> 4/5 附上说明文件"
cat > "$PKG/README.txt" <<EOF
截2 ${VERSION}（Windows x64）

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
unzip -l "$ZIP" | awk 'NR>3 {print $4}' | sed '/^[[:space:]]*$/d' | sed 's/^/  /' | head -40
echo
echo "包内体积统计："
unzip -l "$ZIP" | tail -2
