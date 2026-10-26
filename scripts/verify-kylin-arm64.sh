#!/usr/bin/env bash
# 在已有依赖的 Linux ARM64 环境验证 Qt5 成品；任一必需检查失败立即退出。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-$ROOT/build-kylin-arm64}"
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"
APP="$BUILD_DIR/src/ui/jie2"
PROBE="$BUILD_DIR/src/tools/jie2-probe"
if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != aarch64 ]; then
    echo "此检查必须在 Linux ARM64 上运行。" >&2
    exit 1
fi
for binary in "$APP" "$PROBE" "$BUILD_DIR/src/tools/jie2-line"; do
    test -x "$binary"
    LC_ALL=C readelf -h "$binary" | grep -q 'AArch64'
    dependencies="$(ldd "$binary")"
    if [[ "$dependencies" == *'not found'* ]]; then
        echo "运行依赖缺失：$binary" >&2
        echo "$dependencies" >&2
        exit 1
    fi
done
dynamic="$(readelf -d "$APP")"
[[ "$dynamic" == *libQt5Widgets* && "$dynamic" != *libQt6* ]]

# 阻止误用新系统编译产生旧麒麟无法加载的二进制。
python3 - "$APP" "$PROBE" "$BUILD_DIR/src/tools/jie2-line" <<'PY'
import re
import subprocess
import sys
limits = {'GLIBC': (2, 31), 'GLIBCXX': (3, 4, 28), 'CXXABI': (1, 3, 12)}
for binary in sys.argv[1:]:
    symbols = subprocess.check_output(['readelf', '--version-info', binary], text=True)
    for name, limit in limits.items():
        versions = [tuple(map(int, v.split('.'))) for v in re.findall(r'\b' + name + r'_([0-9.]+)', symbols)]
        if versions:
            actual = max(versions)
            print(f'{binary}: {name}_{".".join(map(str, actual))}')
            if actual > limit:
                sys.exit(f'超出麒麟 V10 SP1 构建基线：{name}_{actual} > {limit}')
PY

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
# 使用临时配置，避免验证过程污染当前用户的数据。
export XDG_CONFIG_HOME="$WORK/config" XDG_DATA_HOME="$WORK/data"
export XDG_RUNTIME_DIR="$WORK/runtime" QT_QPA_PLATFORM=xcb XDG_SESSION_TYPE=x11
unset WAYLAND_DISPLAY
mkdir -m 700 "$XDG_RUNTIME_DIR"
xvfb-run -a -s '-screen 0 1280x800x24' bash -euo pipefail -c '
    app="$1"; probe="$2"; work="$3"
    "$app" --self-check
    "$probe" --test-hotkey
    "$probe" --wait-activation-ms 5000 > "$work/hotkey.log" &
    hotkey_pid=$!
    for attempt in $(seq 1 10); do
        sleep 0.2
        xdotool key ctrl+alt+t
        kill -0 "$hotkey_pid" 2>/dev/null || break
    done
    wait "$hotkey_pid"
    cat "$work/hotkey.log"
    "$probe" --capture
    "$app" --select-rect 20,30,160,90 --output "$work/capture.png"
    "$app" --test-pin "$work/pin"
    python3 - "$work" <<"PY"
from pathlib import Path
import sys
from PIL import Image
root = Path(sys.argv[1])
assert Image.open(root / "capture.png").size == (160, 90)
original = Image.open(root / "pin-orig.png").convert("RGB")
assert original.size == (100, 60)
assert original.getpixel((10, 10)) == (255, 0, 0)
assert Image.open(root / "pin-rot.png").size == (60, 100)
assert Image.open(root / "pin-flip.png").convert("RGB").getpixel((10, 10)) == (0, 255, 0)
assert Image.open(root / "pin-inv.png").convert("RGB").getpixel((10, 10)) == (0, 255, 255)
assert Image.open(root / "pin-saved.png").size == (100, 60)
print("截图与贴图导出检查通过")
PY
' _ "$APP" "$PROBE" "$WORK"
echo "Linux ARM64 / Qt5 兼容性检查通过；麒麟 UKUI 桌面仍需实机验收。"
