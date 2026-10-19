#!/usr/bin/env bash
# 「J2」F5 刷新截图验证（专业版对齐项 v2.7.2："刷新截图，保持已有标注不变"）
#
# 场景一（实时刷新，几何不变 → 标注保留）：
#   选区 → 画矩形 → F5 刷新 → 断言日志"几何不变，保留标注 1 条"；
#   保存后矩形仍在（红色像素存在）。
# 场景二（历史图 → F5 切回实时屏，几何变化 → 标注清空）：
#   先有一张历史 → 打开遮罩 → 按 `,` 载入历史图 → 画矩形 → F5 →
#   断言日志"几何变化，清空标注"；保存后无红色像素。
#
# 用法（容器内）：bash scripts/test-f5-refresh-linux.sh
set -u

DISPLAY_NUM=:94
export DISPLAY=$DISPLAY_NUM
WORK=/work/build-linux
APP=$WORK/src/ui/jie2
LOG=/tmp/jie2-f5.log
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
# 预置一张 64x64 的历史图（供场景二载入）。
rm -f /tmp/f5-history.png
python3 - <<'PY'
from PIL import Image
Image.new("RGB", (64, 64), (0, 128, 0)).save("/tmp/f5-history.png")
PY
"$APP" --select-rect 0,0,64,64 >/dev/null 2>&1  # 生成一条真实历史
rm -f "$LOG"

drag() { # x1 y1 x2 y2
    xdotool mousemove --sync "$1" "$2" sleep 0.15 mousedown 1 sleep 0.15
    local steps=8
    for i in $(seq 1 $steps); do
        xdotool mousemove --sync $(( $1 + ($3 - $1) * i / steps )) \
            $(( $2 + ($4 - $2) * i / steps )) sleep 0.06
    done
    xdotool mouseup 1; sleep 0.45
}

count_red() { # $1 = png；输出红色像素数
    python3 - "$1" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
n = sum(1 for x in range(im.size[0]) for y in range(im.size[1])
        if im.getpixel((x, y))[0] > 200 and im.getpixel((x, y))[1] < 110
        and im.getpixel((x, y))[2] < 110)
print(n)
PY
}

# ---------------------------------------------------------------------------
echo "========== 场景一：实时刷新（标注保留）=========="
"$APP" --select >"$LOG" 2>&1 &
APP_PID=$!
sleep 2.5

drag 100 100 500 400
drag 200 150 280 230

echo "== 按 F5 刷新 =="
xdotool key F5
sleep 2.0   # 刷新含 hide → 150ms → 抓屏 → 恢复显示

echo "== 保存 =="
xdotool key Return
sleep 1.2
kill $APP_PID 2>/dev/null
sleep 0.4

grep -E "F5 刷新" "$LOG" || true
if [ "$(grep -c "F5 刷新截图完成（几何不变，保留标注 1 条）" "$LOG" || true)" != "1" ]; then
    echo "FAIL: 未出现「几何不变，保留标注 1 条」"; exit 1
fi
echo "✓ 日志确认：F5 后标注保留（1 条）"

LATEST=$(ls -t "$PICS"/*.png | head -1)
RED=$(count_red "$LATEST")
echo "保存输出红色像素: $RED（期望 > 100 —— 矩形确实还在）"
if [ "$RED" -lt 100 ]; then
    echo "FAIL: 刷新后矩形丢失"; exit 1
fi
echo "✓ 像素确认：刷新后矩形仍在输出中"

# ---------------------------------------------------------------------------
echo
echo "========== 场景二：历史图 → F5 切回实时（几何变化，标注清空）=========="
"$APP" --select >>"$LOG" 2>&1 &
APP_PID=$!
sleep 2.5

drag 100 100 500 400
echo "== 按 , 载入历史图 =="
xdotool key comma
sleep 1.0
echo "== 在历史图上重新框选并画矩形 =="
drag 100 100 500 400
drag 200 150 280 230
echo "== 按 F5（应判定几何变化并清空）=="
xdotool key F5
sleep 2.0

echo "== 重新框选并保存 =="
drag 100 100 500 400
xdotool key Return
sleep 1.2
kill $APP_PID 2>/dev/null
kill $XVFB_PID 2>/dev/null

grep -E "F5 刷新|历史" "$LOG" | tail -6 || true
if [ "$(grep -c "F5 刷新截图完成（几何变化，清空标注）" "$LOG" || true)" != "1" ]; then
    echo "FAIL: 未出现「几何变化，清空标注」"; exit 1
fi
echo "✓ 日志确认：历史→实时切换时标注被清空"

LATEST=$(ls -t "$PICS"/*.png | head -1)
RED=$(count_red "$LATEST")
echo "保存输出红色像素: $RED（期望 < 12 —— 矩形已被清空）"
if [ "$RED" -ge 12 ]; then
    echo "FAIL: 标注未被清空"; exit 1
fi
echo "✓ 像素确认：清空生效"

echo
echo "PASS: F5 刷新验证通过（实时刷新保留标注 / 历史切换清空标注）"
