#!/usr/bin/env bash
# 「J2」macOS 打包脚本（M7）
#
# 产出：dist/jie2-<版本>-macos-<架构>.zip
# 内容：jie2.app（含 GUI 与内嵌的 Qt 依赖路径处理）
#
# 依赖策略说明（与 Linux 侧不同，这里如实写清）：
#   macOS 构建链接的是 Homebrew 的 Qt（qtbase）。为了让 .app 能在
#   **没有装 Homebrew Qt 的机器**上运行，需要把 Qt 框架随包复制进
#   Contents/Frameworks 并改写入库路径（install_name_tool）。
#   本脚本做"可运行的最小打包"：默认产出 zip 并在包内保留构建时的
#   Qt 路径（适合本机/开发机分发）；如需对外分发（无依赖机器），
#   用 --bundle-qt 尝试复制 Qt 框架并重写 rpath。
#
# 用法：
#   bash scripts/build-macos.sh [build-dir] [output-dir] [--bundle-qt]
set -eu

BUILD_DIR="${1:-build-macos}"
OUT_DIR="${2:-dist}"
BUNDLE_QT=0
for arg in "$@"; do
    [ "$arg" = "--bundle-qt" ] && BUNDLE_QT=1
done

WORK="$(pwd)"
APP_SRC="$BUILD_DIR/src/ui/jie2.app"

APP_VERSION="$(grep -m1 -E '^[[:space:]]+VERSION [0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt | awk '{print $2}')"
if [ -z "$APP_VERSION" ]; then
    echo "无法从 CMakeLists.txt 解析版本号" >&2
    exit 1
fi

ARCH="$(uname -m)"
ZIP_PATH="$WORK/$OUT_DIR/jie2-${APP_VERSION}-macos-${ARCH}.zip"

echo "== 打包 macOS: jie2 ${APP_VERSION} (${ARCH}) =="

if [ ! -d "$APP_SRC" ]; then
    echo "找不到应用：$APP_SRC（先构建）" >&2
    exit 1
fi

STAGE="$WORK/$OUT_DIR/jie2-macos-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -R "$APP_SRC" "$STAGE/jie2.app"

if [ "$BUNDLE_QT" -eq 1 ]; then
    echo "== --bundle-qt：复制 Qt 框架并重写入库路径 =="
    #
    # 实测：本项目的 macOS 构建把 Qt 链接为**绝对路径**
    # （/opt/homebrew/opt/qtbase/lib/QtX.framework/...），不是 @rpath。
    # 所以这里按绝对路径匹配并改写，同时把 rpath 也重定向到包内 —— 
    # 两种形态都处理，避免只支持一种而静默什么都不做。
    FRAMEWORKS="$STAGE/jie2.app/Contents/Frameworks"
    mkdir -p "$FRAMEWORKS"
    BIN="$STAGE/jie2.app/Contents/MacOS/jie2"

    QT_PREFIX="/opt/homebrew/opt/qtbase/lib"

    # 递归复制依赖闭包。
    #
    # 实测三件必须同时处理的事（漏任何一个，包在别的机器上才炸）：
    #   ① **传递依赖**：主二进制不直接引用 QtDBus，但 QtGui 引用它；
    #   ② **两种引用形态**：主二进制用绝对路径
    #      （/opt/homebrew/opt/qtbase/lib/QtX.framework/…），
    #      而框架内部用 @rpath/QtX.framework/…（Homebrew 构建时的形态）；
    #   ③ **引用改写要成对**：复制哪个框架，就要把引用它的那一方
    #      改成 @executable_path 相对路径。
    #
    # 用待办队列做闭包，把两种形态都归一化成"源文件路径 + 框架名"。
    queue="$(otool -L "$BIN" | awk '
        /\/Qt[A-Za-z]*\.framework\// {print $1}
        /@rpath\/Qt[A-Za-z]*\.framework\// {print $1}')"
    seen=""
    copied=0
    while [ -n "$queue" ]; do
        next=''
        for ref in $queue; do
            # 归一化：@rpath/X.framework/... → $QT_PREFIX/X.framework/...
            case "$ref" in
                @rpath/*) real="$QT_PREFIX/${ref#@rpath/}" ;;
                *)        real="$ref" ;;
            esac
            framework="${real%%.framework*}.framework"
            fwname="$(basename "$framework")"
            case " $seen " in
                *" $fwname "*) continue ;;
            esac
            seen="$seen $fwname"
            if [ ! -d "$framework" ]; then
                echo "  跳过（未找到框架目录）：$real"
                continue
            fi
            cp -R "$framework" "$FRAMEWORKS/$fwname"
            copied=$((copied + 1))
            inner="${real#"$framework"/}"   # Versions/A/QtX
            target="@executable_path/../Frameworks/$fwname/$inner"

            # ① 主二进制：两种形态都改一遍（不存在的那种静默无操作）
            install_name_tool -change "$ref" "$target" "$BIN" 2>/dev/null || true

            # ② 依赖图：把该框架的 Qt 依赖加入队列，并在**所有已复制框架**
            #    里把对它的引用改掉（引用者可能是任意一层）。
            inner_bin="$FRAMEWORKS/$fwname/$inner"
            if [ -f "$inner_bin" ]; then
                deps="$(otool -L "$inner_bin" | awk '
                    /\/Qt[A-Za-z]*\.framework\// {print $1}
                    /@rpath\/Qt[A-Za-z]*\.framework\// {print $1}')"
                for dep in $deps; do
                    depbase="${dep##*/}"                     # QtX
                    depfw="${dep%%.framework*}"               # 路径到 …/QtX
                    depfw="$(basename "$depfw")"              # QtX
                    case " $seen " in
                        *" $depfw "*) : ;;
                        *) next="$next $dep" ;;
                    esac
                    depinner="${dep#"${dep%%.framework*}.framework"/}"
                    for holder in "$BIN" "$inner_bin"; do
                        install_name_tool -change "$dep" \
                            "@executable_path/../Frameworks/$depfw.framework/$depinner" \
                            "$holder" 2>/dev/null || true
                    done
                    : "$depbase"
                done
            fi
            echo "  已内含：$fwname"
        done
        queue="$next"
    done

    # rpath 重定向：把 Homebrew 的 rpath 换成包内 Frameworks 目录，
    # 供框架自身的依赖解析（如 QtGui → QtCore）。
    if otool -l "$BIN" | grep -q "$QT_PREFIX"; then
        install_name_tool -rpath "$QT_PREFIX" "@executable_path/../Frameworks" \
            "$BIN" 2>/dev/null || true
    fi

    if [ "$copied" -eq 0 ]; then
        echo "  ⚠ 未复制任何 Qt 框架 —— 可能链接形态已变，请检查 otool -L 输出。"
    fi

    # ---- 复制 Qt 插件 ----
    #
    # 必须做：没有平台插件（libqcocoa）时 GUI 起不来，
    # 报 "Could not find the Qt platform plugin cocoa"。
    # 另带 imageformats（JPEG 等格式）与 tls（https 支持）——
    # 它们不是启动必需，但缺了会在运行时才暴露功能缺失。
    PLUGINS_SRC="/opt/homebrew/opt/qtbase/share/qt/plugins"
    PLUGINS_DST="$STAGE/jie2.app/Contents/PlugIns"
    mkdir -p "$PLUGINS_DST"
    for group in platforms imageformats tls; do
        if [ -d "$PLUGINS_SRC/$group" ]; then
            mkdir -p "$PLUGINS_DST/$group"
            cp "$PLUGINS_SRC/$group/"*.dylib "$PLUGINS_DST/$group/" 2>/dev/null || true
            # 插件自身也引用 Qt 框架：改写成包内相对路径。
            for plugin in "$PLUGINS_DST/$group/"*.dylib; do
                [ -f "$plugin" ] || continue
                pdeps="$(otool -L "$plugin" | awk '
                    /\/Qt[A-Za-z]*\.framework\// {print $1}
                    /@rpath\/Qt[A-Za-z]*\.framework\// {print $1}')"
                for dep in $pdeps; do
                    depfw="${dep%%.framework*}"
                    depfw="$(basename "$depfw")"
                    depinner="${dep#"${dep%%.framework*}.framework"/}"
                    # @executable_path 在加载插件时仍指主二进制目录
                    # （Contents/MacOS），所以到 Contents/Frameworks 只需上一级。
                    # 早期版本写成 ../../（多一级），症状是"插件找到了却加载失败"。
                    install_name_tool -change "$dep" \
                        "@executable_path/../Frameworks/$depfw.framework/$depinner" \
                        "$plugin" 2>/dev/null || true
                done
            done
            echo "  插件已内含：$group（$(ls "$PLUGINS_DST/$group" | wc -l | tr -d ' ') 个）"
        fi
    done

    # qt.conf：把 Qt 的插件搜索路径指到包内。
    mkdir -p "$STAGE/jie2.app/Contents/Resources"
    cat > "$STAGE/jie2.app/Contents/Resources/qt.conf" <<'QTCONF'
[Paths]
Plugins = PlugIns
QTCONF

    # 重签名（ad-hoc）。
    #
    # 必须做：install_name_tool 改写了二进制与框架，原来（构建时的 ad-hoc）
    # 签名随之失效 —— macOS 会直接 SIGKILL（实测报 "Killed: 9"，不是 dyld 错，
    # 极易误判成依赖问题）。
    # 顺序：先签嵌套的框架与插件，再签外层 .app（由内向外）。
    echo "== 重签名（ad-hoc）=="
    find "$STAGE/jie2.app/Contents/Frameworks" -name "*.framework" -maxdepth 1 \
        -exec codesign --force --sign - {} \; 2>/dev/null || true
    find "$STAGE/jie2.app/Contents/PlugIns" -name "*.dylib" \
        -exec codesign --force --sign - {} \; 2>/dev/null || true
    codesign --force --sign - "$STAGE/jie2.app" 2>/dev/null || true
    codesign --verify --deep --strict "$STAGE/jie2.app" 2>&1 | sed 's/^/  /' || true
fi

echo "== 打 zip =="
if command -v ditto >/dev/null 2>&1; then
    # ditto 保留 macOS 扩展属性与资源分支（比 zip 更适合 .app）
    ( cd "$STAGE" && ditto -c -k --sequesterRsrc --keepParent jie2.app \
        "$ZIP_PATH" )
else
    ( cd "$STAGE" && zip -qry "$ZIP_PATH" jie2.app )
fi

rm -rf "$STAGE"
echo "== 完成: $ZIP_PATH =="
ls -lh "$ZIP_PATH"

# 最小自检：解压到临时目录并确认可执行文件存在、架构正确。
echo "== 自检 =="
VERIFY_DIR="$(mktemp -d)"
ditto -x -k "$ZIP_PATH" "$VERIFY_DIR"
file "$VERIFY_DIR/jie2.app/Contents/MacOS/jie2" | sed 's/^/  /'
rm -rf "$VERIFY_DIR"
echo "自检通过。"
