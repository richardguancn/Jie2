#!/usr/bin/env bash
# 「截2」调色板配置验证（M2 增补，专业版对齐项）
#
# 场景：把配置里的 annotation.palette_size 设为 3，
# 打开选择遮罩 → 连点颜色按钮 4 次，断言颜色循环是 3 色循环
# （第 4 次回到第 1 个颜色）。
#
# 用法（容器内）：bash scripts/test-palette-linux.sh
set -u

DISPLAY_NUM=:99
export DISPLAY=$DISPLAY_NUM
WORK=/work/build-linux
APP=$WORK/src/ui/jie2
LOG=/tmp/jie2-palette.log
CFG=/root/.config/jie2/jie2.conf

pkill -f "Xvfb $DISPLAY_NUM" 2>/dev/null
pkill -f "build-linux/src/ui" 2>/dev/null
sleep 0.5
Xvfb $DISPLAY_NUM -screen 0 1280x800x24 >/dev/null 2>&1 &
XVFB_PID=$!
sleep 1.5

mkdir -p /root/.config/jie2
cat > "$CFG" <<EOF
annotation.palette_size=3
EOF
rm -f "$LOG"

"$APP" --select >"$LOG" 2>&1 &
APP_PID=$!
sleep 2.5

# 选区
xdotool mousemove --sync 100 100 sleep 0.15 mousedown 1 sleep 0.15
for i in 1 2 3 4 5 6; do
    xdotool mousemove --sync $((100 + i * 55)) $((100 + i * 40)) sleep 0.05
done
xdotool mouseup 1 sleep 0.5

# 颜色按钮的 x 坐标：工具条顺序
# rect52 ellipse52 arrow52 pencil52 marker64 mosaic64 blur52 eraser52
# magnifier64 text52 number52 undo52 redo52 color40 save76 cancel60
# 起点 x=100，间距 6；color 是第 15 个按钮（index 14）
COLOR_X=$(python3 - <<'PY'
# 必须与 selection_overlay.cpp 的 toolbarItems() **逐项一致**（16 项）。
# 这里早期多写了一个 52（17 项），导致坐标整体右偏一格多，
# 点击落到"保存"附近而不是"颜色" —— 表现为"颜色按钮点了没反应"。
# 顺序：rect ellipse arrow pencil marker mosaic blur eraser
#       magnifier text number undo redo color save cancel
widths = [52,52,52,52,64,64,52,52,64,52,52,52,52,40,76,60]
names  = ["rect","ellipse","arrow","pencil","marker","mosaic","blur","eraser",
          "magnifier","text","number","undo","redo","color","save","cancel"]
x = 100
pos = {}
for w, n in zip(widths, names):
    pos[n] = x + w // 2
    x += w + 6
print(pos["color"])
PY
)
# 工具条 y：选区底部(100+6*40=340) + 10（间距）+ 15（条高一半） = 365。
# 早期版本写死 425 —— 落在工具条之外，点到了画布上（把选区清掉），
# 表现为"颜色按钮点了没反应"。
COLOR_Y=$((100 + 6 * 40 + 10 + 15))
echo "颜色按钮 ($COLOR_X, $COLOR_Y)"

echo "== 连点颜色按钮 4 次（配置为 3 色循环）=="
for i in 1 2 3 4; do
    xdotool mousemove --sync "$COLOR_X" "$COLOR_Y" sleep 0.2 click 1
    sleep 0.4
done

# Esc 取消遮罩
xdotool key Escape
sleep 0.8
kill $APP_PID 2>/dev/null
sleep 0.3
kill $XVFB_PID 2>/dev/null

echo
echo "== 应用日志（颜色切换）=="
grep "颜色：" "$LOG" | tail -6

python3 - <<'PY'
import re

log = open("/tmp/jie2-palette.log", encoding="utf-8").read()
hits = re.findall(r"颜色：(#[0-9A-F]{6})", log)
print("颜色序列:", hits)
if len(hits) < 4:
    print("FAIL: 少于 4 次颜色切换（点击未生效？）")
    raise SystemExit(1)
# 3 色循环：第 4 次应等于第 1 次
if hits[3] != hits[0]:
    print(f"FAIL: 第 4 次 {hits[3]} != 第 1 次 {hits[0]}（未按 3 色循环）")
    raise SystemExit(1)
# 前 3 次应互不相同
if len(set(hits[:3])) != 3:
    print(f"FAIL: 前 3 次颜色有重复 {hits[:3]}")
    raise SystemExit(1)
print("PASS: 调色板数量可配置生效（3 色循环，第 4 次回到第 1 色）")
PY
