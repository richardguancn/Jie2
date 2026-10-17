#!/usr/bin/env bash
# 「截2」形状记忆颜色验证（专业版对齐项 v2.4）
#
# 场景：调色板 4 色，验证"每个工具独立记忆颜色"：
#   1. 矩形工具点颜色 2 次 → 黄、绿
#   2. 切椭圆工具点颜色 1 次 → 黄（椭圆自己的第 1 次，不受矩形影响）
#   3. 切回矩形工具点颜色 1 次 → 蓝（矩形自己的第 3 次，从绿继续）
#   断言：日志里三条颜色记录的工具名与序列全部符合预期。
#
# 用法（容器内）：bash scripts/test-color-memory-linux.sh
set -u

DISPLAY_NUM=:99
export DISPLAY=$DISPLAY_NUM
WORK=/work/build-linux
APP=$WORK/src/ui/jie2
LOG=/tmp/jie2-color-memory.log
CFG=/root/.config/jie2/jie2.conf

pkill -f "Xvfb $DISPLAY_NUM" 2>/dev/null
pkill -f "build-linux/src/ui" 2>/dev/null
sleep 0.5
Xvfb $DISPLAY_NUM -screen 0 1280x800x24 >/dev/null 2>&1 &
XVFB_PID=$!
sleep 1.5

mkdir -p /root/.config/jie2
cat > "$CFG" <<EOF
annotation.palette_size=4
EOF
rm -f "$LOG"

"$APP" --select >"$LOG" 2>&1 &
APP_PID=$!
sleep 2.5

# 工具条按钮坐标（与 selection_overlay.cpp 的 toolbarItems() 一致）
# 工具和颜色通过快捷键切换；工具条按钮的实际点击由界面测试覆盖。



echo "== 选区 =="
xdotool mousemove --sync 100 100 sleep 0.15 mousedown 1 sleep 0.15
for i in $(seq 1 6); do
    xdotool mousemove --sync $((100 + i * 55)) $((100 + i * 40)) sleep 0.06
done
xdotool mouseup 1 sleep 0.5

click() { xdotool mousemove --sync "$1" "$2" sleep 0.2 click 1; sleep 0.4; }

echo "== 1. 矩形工具：点颜色 2 次 =="
xdotool key 1          # 选矩形（确保是矩形）
xdotool key shift+c
xdotool key shift+c

echo "== 2. 切椭圆：点颜色 1 次 =="
xdotool key 2
xdotool key shift+c

echo "== 3. 切回矩形：点颜色 1 次 =="
xdotool key 1
xdotool key shift+c

# Esc 取消
xdotool key Escape
sleep 0.8
kill $APP_PID 2>/dev/null
sleep 0.3
kill $XVFB_PID 2>/dev/null

echo
echo "== 颜色切换日志 =="
grep "颜色：" "$LOG" | tail -6

python3 - <<'PY'
import re

log = open("/tmp/jie2-color-memory.log", encoding="utf-8").read()
# 日志格式：[jie2] 颜色：#XXXXXX（工具=矩形，调色板 4 色，该工具第 N 次切换）
hits = re.findall(r"颜色：(#[0-9A-F]{6})（工具=([^，]+)，调色板 4 色，该工具第 (\d+) 次切换）", log)
print("解析到:", hits)
if len(hits) < 4:
    print("FAIL: 少于 4 次颜色切换")
    raise SystemExit(1)

(hex1, tool1, n1) = hits[0]
(hex2, tool2, n2) = hits[1]
(hex3, tool3, n3) = hits[2]
(hex4, tool4, n4) = hits[3]

# 1. 矩形第 1、2 次：黄、绿
assert tool1 == "矩形" and n1 == "1" and hex1 == "#FACC15", f"FAIL: {hits[0]}"
assert tool2 == "矩形" and n2 == "2" and hex2 == "#22C55E", f"FAIL: {hits[1]}"
print("✓ 矩形：第 1 次黄、第 2 次绿")

# 2. 椭圆第 1 次：黄（不受矩形影响）
assert tool3 == "椭圆" and n3 == "1" and hex3 == "#FACC15", f"FAIL: {hits[2]}"
print("✓ 椭圆：独立从第 1 次开始（黄）—— 形状各记颜色")

# 3. 矩形第 3 次：蓝（从自己的绿继续）
assert tool4 == "矩形" and n4 == "3" and hex4 == "#3B82F6", f"FAIL: {hits[3]}"
print("✓ 切回矩形：从自己的第 3 次继续（蓝）")

print("PASS: 形状记忆颜色验证通过")
PY
