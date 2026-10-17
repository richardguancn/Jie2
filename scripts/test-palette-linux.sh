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
rm -f "/tmp/.X${DISPLAY_NUM#:}-lock" "/tmp/.X11-unix/X${DISPLAY_NUM#:}"
Xvfb $DISPLAY_NUM -screen 0 1280x800x24 >/dev/null 2>&1 &
XVFB_PID=$!
# 等 X socket 就绪：盲等固定时长在高负载下出现过"显示未就绪"竞态
# （xdotool 报 Can't open display → 整项失败）—— 实测教训，不许退回盲等。
for i in $(seq 1 25); do
    [ -S "/tmp/.X11-unix/X${DISPLAY_NUM#:}" ] && break
    sleep 0.2
done
sleep 0.8

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

# Shift+C 切换当前工具颜色，不依赖工具条布局。
xdotool key 1

echo "== 连点颜色按钮 4 次（配置为 3 色循环）=="
for i in 1 2 3 4; do
    xdotool key shift+c
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
