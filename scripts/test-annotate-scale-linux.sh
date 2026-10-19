#!/usr/bin/env bash
# 「J2」选中标注的拖动移动 / 缩放验证（专业版对齐项 v2.3 / v2.11）
#
# 场景一（Shift+滚轮缩放 + 拖动移动）：
#   画矩形 (200,150)-(280,230) → Ctrl+点击选中 → Shift+滚轮 ×2（×1.1 步进）
#   → 拖动 (240,190)→(290,220) 移动 (50,30) → 保存。
#   断言：日志两次「缩放选中标注 ×1.10」+ 一次「拖动选中标注」；
#   输出图红框精确落在 (242,172)-(338,268)（相对选区 (142,72)-(238,168)）。
# 场景二（角柄缩放的三种基准）：
#   同一矩形 → 选中 → 拖右下角柄（自由，对角基准）→ Ctrl+拖左上角柄（中心
#   基准，×0.5）→ Shift+拖右下角柄（等比，×1.879）→ 保存。
#   断言：三条「拖动缩放」日志的基准分别为 对角 / 中心 / 等比；
#   输出图红框 ≈ (235,174)-(343,249)，纵横比保持 ≈1.45。
#
# 用法（容器内）：bash scripts/test-annotate-scale-linux.sh
set -u

DISPLAY_NUM=:95
export DISPLAY=$DISPLAY_NUM
WORK=/work/build-linux
APP=$WORK/src/ui/jie2
LOG=/tmp/jie2-annotate-scale.log
PICS=/root/Pictures/jie2

pkill -f "Xvfb $DISPLAY_NUM" 2>/dev/null
pkill -f "build-linux/src/ui" 2>/dev/null
sleep 0.5
rm -f "/tmp/.X${DISPLAY_NUM#:}-lock" "/tmp/.X11-unix/X${DISPLAY_NUM#:}"
Xvfb $DISPLAY_NUM -screen 0 1280x800x24 >/dev/null 2>&1 &
XVFB_PID=$!
for i in $(seq 1 25); do
    [ -S "/tmp/.X11-unix/X${DISPLAY_NUM#:}" ] && break
    sleep 0.2
done
sleep 0.8

rm -rf /root/.config/jie2 "$PICS"
rm -f "$LOG"

start_app() {
    "$APP" --select >>"$LOG" 2>&1 &
    APP_PID=$!
    sleep 2.5
    if ! kill -0 "$APP_PID" 2>/dev/null; then
        echo "FAIL: 遮罩未启动"; cat "$LOG"; exit 1
    fi
}

drag() { # x1 y1 x2 y2 —— 普通左键拖拽
    xdotool mousemove --sync "$1" "$2" sleep 0.15 mousedown 1 sleep 0.15
    local steps=8
    for i in $(seq 1 $steps); do
        xdotool mousemove --sync $(( $1 + ($3 - $1) * i / steps )) \
            $(( $2 + ($4 - $2) * i / steps )) sleep 0.06
    done
    xdotool mouseup 1; sleep 0.45
}

drag_with_key() { # 修饰键 x1 y1 x2 y2 —— 按住修饰键的拖拽
    local mod="$1"; shift
    xdotool mousemove --sync "$1" "$2" sleep 0.15
    xdotool keydown "$mod"
    xdotool mousedown 1 sleep 0.15
    local steps=8
    for i in $(seq 1 $steps); do
        xdotool mousemove --sync $(( $1 + ($3 - $1) * i / steps )) \
            $(( $2 + ($4 - $2) * i / steps )) sleep 0.06
    done
    xdotool mouseup 1
    xdotool keyup "$mod"
    sleep 0.45
}

click_ctrl() { # x y —— Ctrl+点击（选中）
    xdotool mousemove --sync "$1" "$2" sleep 0.2
    xdotool keydown ctrl
    xdotool click 1
    xdotool keyup ctrl
    sleep 0.5
}

# ---------------------------------------------------------------------------
echo "========== 场景一：Shift+滚轮缩放 + 拖动移动 =========="
start_app

echo "== 选区 (100,100)-(500,400) =="
drag 100 100 500 400
xdotool key 1

echo "== 画矩形 (200,150)-(280,230) =="
drag 200 150 280 230

echo "== Ctrl+点击 (240,190) 选中 =="
click_ctrl 240 190

echo "== Shift+滚轮 ×2（期望两次 ×1.10）=="
xdotool mousemove --sync 240 190 sleep 0.2
xdotool keydown shift
xdotool click 4
sleep 0.4
xdotool click 4
xdotool keyup shift
sleep 0.5

echo "== 拖动移动 (240,190) → (290,220) =="
drag 240 190 290 220

echo "== 保存 =="
xdotool key Return
sleep 1.2
kill $APP_PID 2>/dev/null
sleep 0.4

echo "---- 场景一日志 ----"
grep -E "自由选中|缩放选中标注|拖动选中标注" "$LOG" || true

expect_log() { # 期望次数 模式
    local want="$1" pat="$2" got
    got=$(grep -c "$pat" "$LOG" || true)
    if [ "$got" != "$want" ]; then
        echo "FAIL: 「$pat」出现 $got 次（期望 $want）"; exit 1
    fi
    echo "✓ 「$pat」次数=$got"
}
expect_log 1 "自由选中：标注 1/1（矩形）"
expect_log 2 "缩放选中标注 ×1.10"
expect_log 1 "拖动选中标注："

# 场景一像素断言：红框应落在相对 (142,72)-(238,168)（位移 50,30 生效）。
LATEST=$(ls -t "$PICS"/*.png | head -1)
python3 - "$LATEST" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
w, h = im.size
assert (w, h) == (400, 300), f"FAIL: 输出尺寸 {w}x{h}"
xs, ys = [], []
for x in range(w):
    for y in range(h):
        r, g, b = im.getpixel((x, y))
        if r > 200 and g < 110 and b < 110:
            xs.append(x); ys.append(y)
assert xs, "FAIL: 输出图没有红框"
x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
print(f"红框范围: ({x0},{y0})-({x1},{y1})（期望 (142,72)-(238,168) ±4）")
assert abs(x0 - 142) <= 4 and abs(x1 - 238) <= 4, "FAIL: 水平位置不符"
assert abs(y0 - 72) <= 4 and abs(y1 - 168) <= 4, "FAIL: 垂直位置不符"
print("✓ 场景一像素断言通过（缩放 96x96 + 位移 (50,30) 全部生效）")
PY

# ---------------------------------------------------------------------------
echo
echo "========== 场景二：角柄缩放的三种基准 =========="
start_app

drag 100 100 500 400
xdotool key 1
drag 200 150 280 230
click_ctrl 240 190

echo "== 拖右下角柄 (284,234)→(324,234)：自由缩放（对角基准）=="
drag 284 234 324 234

echo "== Ctrl+拖左上角柄 (198,146)→(229,168)：中心基准 ×0.5 =="
# 说明：上一步后 bbox=(202,150)-(318,230)，手柄 box=(198,146)-(322,234)。
drag_with_key ctrl 198 146 229 168

echo "== Shift+拖右下角柄 (293,214)→(351,214)：等比 ×1.879 =="
# 说明：上一步后 bbox=(231,170)-(289,210)，手柄 box=(227,166)-(293,214)。
drag_with_key shift 293 214 351 214

echo "== 保存 =="
xdotool key Return
sleep 1.2
kill $APP_PID 2>/dev/null
sleep 0.4

echo "---- 场景二日志 ----"
grep -E "拖动缩放" "$LOG" | tail -4 || true
if [ "$(grep -c "拖动缩放：80x80 → 116x80（基准：对角）" "$LOG" || true)" != "1" ]; then
    echo "FAIL: 对角基准缩放日志不符"; exit 1
fi
if [ "$(grep -c "拖动缩放：116x80 → 58x40（基准：中心）" "$LOG" || true)" != "1" ]; then
    echo "FAIL: 中心基准缩放日志不符"; exit 1
fi
if [ "$(grep -c "拖动缩放：58x40 → 108x75（基准：等比）" "$LOG" || true)" != "1" ]; then
    echo "FAIL: 等比基准缩放日志不符"; exit 1
fi
echo "✓ 三条缩放日志（对角/中心/等比）全部命中"

LATEST=$(ls -t "$PICS"/*.png | head -1)
python3 - "$LATEST" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
w, h = im.size
xs, ys = [], []
for x in range(w):
    for y in range(h):
        r, g, b = im.getpixel((x, y))
        if r > 200 and g < 110 and b < 110:
            xs.append(x); ys.append(y)
assert xs, "FAIL: 输出图没有红框"
x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
bw, bh = x1 - x0, y1 - y0
print(f"最终红框: ({x0},{y0})-({x1},{y1}) 尺寸 {bw}x{bh}（期望 ≈108x75）")
assert abs(bw - 108) <= 6 and abs(bh - 75) <= 6, "FAIL: 最终尺寸不符"
aspect = bw / bh
print(f"纵横比: {aspect:.2f}（期望 ≈1.44 —— 等比缩放保持纵横比）")
assert abs(aspect - 1.44) < 0.12, "FAIL: 纵横比未保持（等比缩放失效？）"
assert abs(x0 - 135) <= 6 and abs(y0 - 74) <= 6, "FAIL: 最终位置不符"
print("✓ 场景二像素断言通过（自由/中心/等比三种缩放全部生效且纵横比保持）")
PY

kill $XVFB_PID 2>/dev/null

echo
echo "PASS: 选中标注拖动/缩放验证通过（v2.3 中心点与等比、v2.11 滚轮缩放）"
