#!/usr/bin/env bash
# 「截2」序号调整验证（专业版对齐项 v2.5.2："第一个数字可大于 1；+/- 调整序号"）
#
# 场景：选区 → 切序号工具（N）→ `+` ×2（下一个序号应为 3）
#       → 放置 → 断言 "number=3"；→ `-` ×5（偏移 -3，下限 1）
#       → 断言 "下一个序号：1"。
#
# 用法（容器内）：bash scripts/test-number-offset-linux.sh
set -u

DISPLAY_NUM=:93
export DISPLAY=$DISPLAY_NUM
WORK=/work/build-linux
APP=$WORK/src/ui/jie2
LOG=/tmp/jie2-number.log

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

rm -rf /root/.config/jie2 /root/Pictures/jie2
rm -f "$LOG"

"$APP" --select >"$LOG" 2>&1 &
APP_PID=$!
sleep 2.5

# 选区
xdotool mousemove --sync 100 100 sleep 0.15 mousedown 1 sleep 0.15
for i in 1 2 3 4; do
    xdotool mousemove --sync $((100 + i * 100)) $((100 + i * 75)) sleep 0.06
done
xdotool mouseup 1 sleep 0.5

echo "== 切序号工具（N）=="
xdotool key n
sleep 0.4

echo "== + ×2（下一个序号：3）=="
xdotool key plus
sleep 0.3
xdotool key plus
sleep 0.4

echo "== 放置序号 =="
xdotool mousemove --sync 220 200 sleep 0.2 click 1
sleep 0.5

echo "== - ×5（下限 1）=="
for i in 1 2 3 4 5; do
    xdotool key minus
    sleep 0.2
done
sleep 0.4

xdotool key Escape
sleep 0.6
kill $APP_PID 2>/dev/null
sleep 0.3
kill $XVFB_PID 2>/dev/null

echo
echo "== 日志（序号相关）=="
grep -E "序号|number" "$LOG" || true

python3 - <<'PY'
import re
log = open("/tmp/jie2-number.log", encoding="utf-8").read()

def count(p):
    return len(re.findall(p, log))

assert count(r"工具：序号") == 1, f"FAIL: 序号工具未切换（{count(r'工具：序号')} 次）"
# 提示序列：+ 后 2 → + 后 3 → 放置 →（- 调整时会出现 3、2、1、1、1）。
values = re.findall(r"下一个序号：(\d+)", log)
assert values[:2] == ["2", "3"], f"FAIL: + ×2 提示序列应为 2,3，实际 {values}"
assert count(r"标注已添加：number=3") == 1, "FAIL: 未放置出 number=3"
assert values[-1] == "1", f"FAIL: - ×5 后应下限到 1，实际 {values[-1]}"
print(f"提示序列: {values}")
print("PASS: 序号调整验证通过（+ 偏移起始 3 / 下限钳制到 1）")
PY
