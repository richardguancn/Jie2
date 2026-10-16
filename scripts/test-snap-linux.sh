#!/usr/bin/env bash
# 「截2」窗口吸附端到端验证（Xvfb + xdotool + 测试窗口）
#
# 场景：
#   1. 起 Xvfb，创建一个**普通顶层窗口**（scripts/window_target）在已知位置；
#   2. 起 jie2 的交互遮罩；
#   3. 拖拽选区，终点故意落在目标窗口边缘附近（阈值内）；
#   4. 断言日志中的选区右/下边缘与目标窗口边缘**精确相等**（吸附生效）；
#   5. 同时断言吸附候选窗口数 ≥ 1。
#
# 用法（容器内）：bash scripts/test-snap-linux.sh
set -u

DISPLAY_NUM=:99
export DISPLAY=$DISPLAY_NUM
WORK=/work/build-linux
APP=$WORK/src/ui/jie2
LOG=/tmp/jie2-snap.log

pkill -f "Xvfb $DISPLAY_NUM" 2>/dev/null
pkill -f window_target 2>/dev/null
sleep 0.5
Xvfb $DISPLAY_NUM -screen 0 1280x800x24 >/dev/null 2>&1 &
XVFB_PID=$!
sleep 1.5

# 靶窗二进制：构建目录被清理后需要重新编译（脚本自愈，避免依赖前置手工步骤）。
TARGET_BIN="$WORK/scripts/window_target"
if [ ! -x "$TARGET_BIN" ]; then
    mkdir -p "$WORK/scripts"
    cc /work/scripts/window_target.c -o "$TARGET_BIN" -lX11
fi

# 目标窗口：位置 (700,200)，尺寸 300x240 → 右边缘 1000，下边缘 440。
"$TARGET_BIN" 700 200 300 240 60 >/tmp/window_target.log 2>&1 &
TARGET_PID=$!
sleep 1.0
cat /tmp/window_target.log

rm -f "$LOG"
"$APP" --select >"$LOG" 2>&1 &
APP_PID=$!
sleep 2

# 拖拽终点 (996,436)：距目标窗口右边缘 1000 差 4、下边缘 440 差 4，均在 8pt 阈值内。
echo "== 拖拽 (100,100) → (996,436)，期望吸附到 (1000,440) =="
xdotool mousemove --sync 100 100 sleep 0.15
xdotool mousedown 1 sleep 0.2
for i in $(seq 1 12); do
    x=$(( 100 + (996 - 100) * i / 12 ))
    y=$(( 100 + (436 - 100) * i / 12 ))
    xdotool mousemove --sync "$x" "$y" sleep 0.06
done
xdotool mouseup 1 sleep 0.5

sleep 0.5
kill $APP_PID 2>/dev/null
kill $TARGET_PID 2>/dev/null
kill $XVFB_PID 2>/dev/null

echo
echo "== 应用日志 =="
cat "$LOG"

echo
echo "== 断言 =="
python3 - "$LOG" <<'PY'
import re
import sys

log = open(sys.argv[1], encoding="utf-8").read()
m = re.search(r"吸附候选窗口数：(\d+)", log)
assert m and int(m.group(1)) >= 1, "FAIL: 未发现吸附候选窗口"
print(f"吸附候选窗口数: {m.group(1)} ✓")

m = re.search(r"选区结算 (\d+),(\d+) (\d+)x(\d+)", log)
assert m, "FAIL: 未发现选区结算日志"
x, y, w, h = (int(v) for v in m.groups())
right, bottom = x + w, y + h
print(f"选区: ({x},{y}) {w}x{h} → 右边缘={right} 下边缘={bottom}")
# 半开区间语义下：右边缘应吸附到 1000、下边缘吸附到 440。
assert right == 1000, f"FAIL: 右边缘 {right} != 1000（未吸附）"
assert bottom == 440, f"FAIL: 下边缘 {bottom} != 440（未吸附）"
print("PASS: 窗口吸附端到端验证通过（右/下边缘精确吸附到目标窗口边框）")
PY
