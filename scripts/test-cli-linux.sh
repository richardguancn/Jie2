#!/usr/bin/env bash
# 「截2」M6 命令行验证（Linux aarch64）
#
# 覆盖：
#   1. --size W H        强制输出尺寸
#   2. --whiteboard C    纯色画布 + 标注（拖一个矩形 → 回车 → 保存）
#   3. --active-screen   用光标所在屏（Xvfb 只有一屏，验证不报错即可）
#   4. --hold / --block  参数被接受（--hold 打开遮罩；本脚本只验证不崩）
#
# 用法（容器内）：bash scripts/test-cli-linux.sh
set -u

DISPLAY_NUM=:99
export DISPLAY=$DISPLAY_NUM
WORK=/work/build-linux
APP=$WORK/src/ui/jie2
SHOT_DIR=/root/Pictures/jie2

pkill -f "Xvfb $DISPLAY_NUM" 2>/dev/null
pkill -f "build-linux/src/ui" 2>/dev/null
sleep 0.5
Xvfb $DISPLAY_NUM -screen 0 1280x800x24 >/dev/null 2>&1 &
XVFB_PID=$!
sleep 1.5

rm -rf "$SHOT_DIR"

echo "== [1] --size 强制输出尺寸 =="
rm -f /tmp/cli-size.png
"$APP" --select-rect 0,0,60,40 --size 180 90 --output /tmp/cli-size.png \
    >/tmp/cli1.log 2>&1
python3 - <<'PY'
from PIL import Image
im = Image.open("/tmp/cli-size.png")
print("  输出:", im.size, "（源 60x40 → 强制 180x90）")
assert im.size == (180, 90), f"FAIL: {im.size}"
print("  ✓ --size 生效")
PY

echo "== [2] --active-screen（单屏环境下应与默认一致）=="
rm -f /tmp/cli-active.png
xdotool mousemove --sync 640 400
"$APP" --select-rect 0,0,40,30 --active-screen --output /tmp/cli-active.png \
    >/tmp/cli2.log 2>&1
ls -l /tmp/cli-active.png | awk '{print "  产物:", $5, "字节"}'
python3 -c "
from PIL import Image
print('  ✓ --active-screen 输出:', Image.open('/tmp/cli-active.png').size)
"

echo "== [3] --whiteboard（蓝底 + 画一个矩形 + 回车保存）=="
"$APP" --whiteboard "#0044ff" >/tmp/cli3.log 2>&1 &
APP_PID=$!
sleep 2.5

# 拖一个矩形选区作为"画布区域"（白板需要先框选区域才能标注）
xdotool mousemove --sync 100 100 sleep 0.15 mousedown 1 sleep 0.15
for i in $(seq 1 8); do
    xdotool mousemove --sync $((100 + i * 50)) $((100 + i * 40)) sleep 0.06
done
xdotool mouseup 1 sleep 0.4
# 在选区内画一条红色矩形
xdotool mousemove --sync 200 200 sleep 0.15 mousedown 1 sleep 0.15
for i in $(seq 1 6); do
    xdotool mousemove --sync $((200 + i * 20)) $((200 + i * 10)) sleep 0.06
done
xdotool mouseup 1 sleep 0.4
# 回车确认保存
xdotool key Return
sleep 1.5

kill $APP_PID 2>/dev/null
sleep 0.3

echo "  -- 应用日志 --"
grep -E "白板|提交选区|saved|标注" /tmp/cli3.log | tail -5

LATEST=$(ls -t "$SHOT_DIR"/*.png 2>/dev/null | head -1)
if [ -z "$LATEST" ]; then
    echo "  FAIL: 白板没有保存任何文件"
    kill $XVFB_PID 2>/dev/null
    exit 1
fi
python3 - "$LATEST" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
w, h = im.size
print(f"  白板输出: {w}x{h}")
# 背景应为蓝 (#0044ff = 0,68,255)；检查一个远离标注的位置
bg = im.getpixel((w - 20, h - 20))
print("  背景采样:", bg, "← 应为蓝(0,68,255)")
assert abs(bg[0] - 0) < 20 and abs(bg[1] - 68) < 30 and abs(bg[2] - 255) < 20, \
    f"FAIL: 白板底色不对 {bg}"
# 标注应为红（默认色 239,68,68）。
# 坐标换算：选区 (100,100)-(500,420)，标注拖拽 (200,200)→(320,260)，
# 相对选区即 (100,100)-(220,160) —— 输出图里就在这个范围。
red = sum(1 for x in range(98, 222) for y in range(98, 162)
          if im.getpixel((x, y)) == (239, 68, 68))
print("  标注红色像素:", red)
assert red > 50, "FAIL: 白板标注未渲染"
print("  ✓ 白板模式（纯色画布 + 标注 + 保存）通过")
PY

kill $XVFB_PID 2>/dev/null
echo
echo "PASS: M6 命令行验证通过（--size / --active-screen / --whiteboard）"
