#!/usr/bin/env bash
# 「截2」标注自由选中 / 二次编辑验证（专业版对齐项 v2.8）
#
# 场景（真实路径）：
#   1. 打开遮罩 → 选区 (100,100)-(500,400)
#   2. 画矩形 (200,150)-(350,300)
#   3. Ctrl+点击矩形中心 → 断言"自由选中：标注 1/1（矩形）"
#   4. Delete → 断言"删除选中标注（剩 0 条）"；保存后断言矩形区域无红色
#   5. 画矩形 → Ctrl+点击选中 → 方向键 Right ×20 → 断言移动日志；
#      保存后断言矩形左边缘从相对 x=100 移到 x=120（位移 20px 生效）
#
# 用法（容器内）：bash scripts/test-annotate-select-linux.sh
set -u

DISPLAY_NUM=:92
export DISPLAY=$DISPLAY_NUM
WORK=/work/build-linux
APP=$WORK/src/ui/jie2
LOG=/tmp/jie2-anno-select.log
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
sleep 1

rm -rf /root/.config/jie2 "$PICS"
rm -f /tmp/jie2-single-root "$LOG"

"$APP" --select >"$LOG" 2>&1 &
APP_PID=$!
sleep 2.5
if ! kill -0 "$APP_PID" 2>/dev/null; then
    echo "FAIL: 遮罩未启动"; cat "$LOG"; exit 1
fi

# 工具条按钮坐标（与 selection_overlay.cpp 的 toolbarItems() 一致）
# 工具和颜色通过快捷键切换；工具条按钮的实际点击由界面测试覆盖。


click() { xdotool mousemove --sync "$1" "$2" sleep 0.2 click 1; sleep 0.35; }

drag() {
    xdotool mousemove --sync "$1" "$2" sleep 0.15 mousedown 1 sleep 0.15
    local steps=6
    for i in $(seq 1 $steps); do
        local x=$(( $1 + ($3 - $1) * i / steps ))
        local y=$(( $2 + ($4 - $2) * i / steps ))
        xdotool mousemove --sync "$x" "$y" sleep 0.05
    done
    xdotool mouseup 1; sleep 0.4
}

echo "== 1. 选区 (100,100)-(500,400) =="
drag 100 100 500 400
xdotool key 1

echo "== 2. 画矩形 (200,150)-(350,300) =="
xdotool key 1
drag 200 150 350 300

echo "== 3. Ctrl+点击矩形中心 (275,225) → 选中 =="
xdotool keydown ctrl
xdotool mousemove --sync 275 225 sleep 0.2 click 1
xdotool keyup ctrl
sleep 0.5

echo "== 4. Delete 删除选中 =="
xdotool key Delete
sleep 0.5

echo "== 4.5 保存（第一次输出：应无矩形）=="
xdotool key Return
sleep 1.5

echo "== 5. 再开一次遮罩：画矩形 → 选中 → Right×20 =="
"$APP" --select >>"$LOG" 2>&1 &
APP2_PID=$!
sleep 2.5
drag 100 100 500 400
xdotool key 1
xdotool key 1
drag 200 150 350 300
xdotool keydown ctrl
xdotool mousemove --sync 275 225 sleep 0.2 click 1
xdotool keyup ctrl
sleep 0.5
for i in $(seq 1 20); do xdotool key Right; sleep 0.06; done
sleep 0.5

echo "== 5.5 保存（第二次输出：矩形应右移 20px）=="
xdotool key Return
sleep 1.5

kill "$APP_PID" 2>/dev/null
kill "$APP2_PID" 2>/dev/null
sleep 0.5
kill "$XVFB_PID" 2>/dev/null

echo
echo "== 日志 =="
grep -E "自由选中|删除选中|移动选中" "$LOG" | tail -10

echo
echo "== 截图文件 =="
ls -t "$PICS"/*.png 2>/dev/null | head -3

python3 - <<'PY'
import glob, re
from PIL import Image

log = open("/tmp/jie2-anno-select.log", encoding="utf-8").read()

assert re.search(r"自由选中：标注 1/1（矩形）", log), "FAIL: 缺少选中日志"
print("✓ 自由选中：命中并选中矩形")
assert re.search(r"删除选中标注（剩 0 条）", log), "FAIL: 缺少删除日志"
print("✓ Delete 删除选中标注")
assert re.search(r"移动选中标注（1,0，步长 1px）", log), "FAIL: 缺少移动日志"
print("✓ 方向键移动选中标注")

files = sorted(glob.glob("/root/Pictures/jie2/*.png"))
assert len(files) >= 2, f"FAIL: 期望 ≥2 个输出，实际 {len(files)}"
first, second = files[0], files[1]

im1 = Image.open(first).convert("RGB")
w1, h1 = im1.size
# 第一次输出：矩形被删除 → 无红色像素
red1 = sum(1 for x in range(0, w1) for y in range(0, h1)
           if im1.getpixel((x, y))[0] > 200 and im1.getpixel((x, y))[1] < 100)
print(f"  第一次输出红色像素: {red1}（应为 0）")
assert red1 == 0, "FAIL: 删除后不应有红色像素"
print("✓ 删除真实生效（输出无矩形）")

im2 = Image.open(second).convert("RGB")
w2, h2 = im2.size
# 第二次输出：矩形右移 20 → 相对左边缘应在 x≈120（容差 ±2）
row = 50  # 矩形上边相对 y=50
xs = [x for x in range(0, w2) if im2.getpixel((x, row))[0] > 200
      and im2.getpixel((x, row))[1] < 100]
assert xs, "FAIL: 第二次输出矩形缺失"
left = min(xs)
print(f"  第二次输出矩形左边缘: x={left}（期望 120±2）")
assert abs(left - 120) <= 2, f"FAIL: 位移未生效（左边缘 {left}）"
print("✓ 移动真实生效（左边缘从 100 移到 120）")

print("PASS: 标注自由选中/二次编辑验证通过")
PY
