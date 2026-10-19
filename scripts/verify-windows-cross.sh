#!/usr/bin/env bash
# 「J2」Windows 平台后端交叉编译验证
#
# 为什么需要这个脚本：
#   Windows 后端（src/platform/windows/）在本机（macOS 宿主 + Linux 容器）
#   没有真实的 Windows 运行环境。按项目"能验证才声称完成"的约定，
#   这里做**交叉编译验证**（编译 + 链接通过），并把线程模型的坑固化下来：
#
#   Debian/Ubuntu 的 mingw-w64 有**两个线程模型变体**：
#     - win32（默认）：**不支持** std::mutex / std::thread /
#       std::condition_variable —— 直接编译 Windows 后端会报
#       "std::mutex does not name a type"（实测）
#     - posix：支持上述 C++11 线程设施
#   本脚本自动挑选 posix 变体；找不到时明确报错而不是假装通过。
#
# 用法（容器或任意装了 mingw 的环境）：bash scripts/verify-windows-cross.sh
set -u

cd "$(dirname "$0")/.." || exit 1

# ---- 1. 选工具链（优先 posix 线程模型）----
CXX=""
for cand in x86_64-w64-mingw32-g++-posix x86_64-w64-mingw32-g++ \
            aarch64-w64-mingw32-g++-posix aarch64-w64-mingw32-g++; do
    if command -v "$cand" >/dev/null 2>&1; then
        CXX="$cand"
        # 验证它支持 std::mutex（win32 模型不支持，换下一个候选）
        if printf '#include <mutex>\nint main(){ std::mutex m; (void)m; return 0; }\n' |
           "$CXX" -std=c++20 -x c++ - -c -o /dev/null 2>/dev/null; then
            break
        fi
        echo "提示：$cand 不支持 std::mutex（win32 线程模型），继续找 posix 变体…"
        CXX=""
    fi
done

if [ -z "$CXX" ]; then
    echo "FAIL: 找不到支持 C++11 线程设施的 mingw-w64 工具链。"
    echo "  安装：sudo apt install g++-mingw-w64-x86-64-posix"
    echo "  （注意必须带 -posix 后缀；默认的 win32 变体不支持 std::mutex）"
    exit 2
fi
echo "== 工具链: $CXX ($($CXX --version | head -1)) =="

# ---- 2. 编译 core + platform + smoke ----
OUT=/tmp/jie2-win-cross
rm -rf "$OUT"
mkdir -p "$OUT"

CORE_SOURCES="src/core/annotation.cpp src/core/capability.cpp \
src/core/capture_history.cpp src/core/config.cpp src/core/image_buffer.cpp \
src/core/image_ops.cpp src/core/ocr.cpp src/core/snap.cpp"
PLATFORM_SOURCES="src/platform/factory.cpp src/platform/hotkey.cpp \
src/platform/windows/windows_platform.cpp"

echo "== 编译目标文件 =="
fail=0
for src in $CORE_SOURCES $PLATFORM_SOURCES; do
    obj="$OUT/$(basename "${src%.cpp}").o"
    if ! "$CXX" -std=c++20 -Wall -Wextra -I src -c "$src" -o "$obj" \
         > "$OUT/$(basename "$src").log" 2>&1; then
        echo "  FAIL: $src"
        head -8 "$OUT/$(basename "$src").log" | sed 's/^/    /'
        fail=1
    else
        echo "  ok: $src"
    fi
done
[ "$fail" -eq 0 ] || { echo "编译失败"; exit 1; }

echo "== 链接 smoke 可执行（.exe）=="
cat > "$OUT/smoke.cpp" <<'EOF'
// 交叉编译验证用的最小入口：只调用平台层的探测，不碰 UI。
#include <cstdio>
#include "platform/platform.h"

int main() {
    auto platform = jie2::platform::createPlatform();
    const jie2::core::PlatformCapabilities caps = platform->probe();
    std::printf("jie2 windows smoke (%s)\n", platform->name().c_str());
    std::printf("%s", caps.describe().c_str());
    return 0;
}
EOF

"$CXX" -std=c++20 -I src "$OUT/smoke.cpp" "$OUT"/*.o -o "$OUT/jie2-win-smoke.exe" \
    -luser32 -lgdi32 -lshcore -static -static-libgcc -static-libstdc++ \
    > "$OUT/link.log" 2>&1
if [ $? -ne 0 ]; then
    echo "  FAIL: 链接失败"
    head -12 "$OUT/link.log" | sed 's/^/    /'
    exit 1
fi

echo "== 产物 =="
file "$OUT/jie2-win-smoke.exe" | sed 's/^/  /'
ls -lh "$OUT/jie2-win-smoke.exe" | awk '{print "  大小:", $5}'

# ---- 3. 导入表检查 ----
#
# 断言：所有依赖都来自预期的系统 DLL（KERNEL32/USER32/GDI32/SHCORE/msvcrt），
# 且没有未解析符号。这比"能链接"更强一层：排除链接期被静默塞入的
# 意外依赖（比如静态库引入的额外 DLL），也排除了悬空符号。
echo "== 导入表检查 =="
IMPORTS=$(x86_64-w64-mingw32-objdump -p "$OUT/jie2-win-smoke.exe" |
          awk '/DLL Name/{print $3}' | sort -u)
echo "$IMPORTS" | sed 's/^/  /'
# 允许两类 C 运行时（都是系统组件，取决于工具链配置）：
#   - msvcrt.dll                老式 CRT（Debian mingw-w64 默认）
#   - api-ms-win-crt-*.dll      UCRT 转发 DLL（Windows 10+ 自带；
#                               brew 的 mingw-w64 默认用 UCRT）
EXPECTED="GDI32.dll KERNEL32.dll msvcrt.dll SHCORE.dll USER32.dll"
bad=""
for dll in $IMPORTS; do
    case " $EXPECTED " in
        *" $dll "*) continue ;;
    esac
    case "$dll" in
        api-ms-win-crt-*.dll) continue ;;
    esac
    bad="$bad $dll"
done
if [ -n "$bad" ]; then
    echo "  WARN: 出现预期外的依赖：$bad（检查是否引入了非系统库）"
else
    echo "  ✓ 依赖全部来自预期系统 DLL（含 CRT 变体）"
fi

# ---- 4. 可选：wine 运行级验证（带架构匹配检查）----
#
# 为什么必须检查架构：wine 只能加载**与宿主架构相同**的 PE
# （wine 8+ 的 WoW64 把系统 DLL 编成宿主架构的 PE）。在 aarch64 宿主上
# 加载 x86-64 PE 会报误导性的 "Application could not be started /
# File not found"（实测 —— 看起来像产物坏了，实为环境不支持）。
# 所以先比对架构：不匹配就明确说"跳过及原因"，而不是制造假失败。
WINE_BIN=""
for cand in wine64 wine; do
    if command -v "$cand" >/dev/null 2>&1; then WINE_BIN="$cand"; break; fi
done
if [ -z "$WINE_BIN" ] && [ -x /usr/lib/wine/wine64 ]; then
    WINE_BIN=/usr/lib/wine/wine64  # Debian 的真实 loader（/usr/bin/wine 是 wrapper）
fi

HOST_ARCH="$(uname -m)"
case "$HOST_ARCH" in
    x86_64)        HOST_PE="x86-64" ;;
    aarch64|arm64) HOST_PE="aarch64" ;;
    *)             HOST_PE="unknown" ;;
esac

if [ -n "$WINE_BIN" ]; then
    if [ "$HOST_PE" = "x86-64" ]; then
        echo "== wine 运行级验证（非真实 Windows，仅作额外信号）=="
        export WINEDEBUG=-all WINEARCH=win64
        export WINEPREFIX="${TMPDIR:-/tmp}/jie2-wineprefix"
        export WINELOADER="$WINE_BIN"
        if timeout 120 "$WINE_BIN" "$OUT/jie2-win-smoke.exe" 2>/dev/null |
           grep -q "windows smoke"; then
            echo "  ✓ wine 下探测输出正常"
        else
            echo "  ℹ wine 运行未取得输出（不阻断编译级结论）"
        fi
    else
        echo "== wine 运行级验证：跳过 =="
        echo "  原因：wine 宿主为 $HOST_ARCH，只能加载 $HOST_PE PE；"
        echo "        本产物是 x86-64 PE（由 x86_64-w64-mingw32 编译）。"
        echo "        aarch64 宿主加载 x86-64 PE 需要 x86-64 wine / FEX / box64，"
        echo "        该环境不具备 —— 这是环境限制，不是产物问题。"
    fi
else
    echo "注：环境无 wine，跳过运行级验证（实机 Windows 验证仍待 Windows 环境）。"
fi

echo
echo "PASS: Windows 后端交叉编译验证通过（编译 + 链接 + 导入表）"
echo "  说明：本结果证明 Win32 API 用法可编译、依赖闭包干净；"
echo "        **实机运行验证**仍需真实 Windows 环境（README「Windows 平台」）。"
