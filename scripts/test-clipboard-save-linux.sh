#!/usr/bin/env bash
# 「J2」Ctrl+Alt+C 保存剪贴板图像验证（专业版对齐项 v2.5）
#
# 场景（真实路径）：
#   1. Xvfb + xclip 放一张 60x40 红图到剪贴板
#   2. 启动主实例，合成热键 Ctrl+Alt+C
#   3. 断言：日志"已保存剪贴板图像"；截图目录出现新 PNG；尺寸 60x40
#   4. 再放一张 20x20 蓝图 → 再按一次 → 断言第二张也保存
#
# 用法（容器内）：bash scripts/test-clipboard-save-linux.sh
set -u

DISPLAY_NUM=:94
export DISPLAY=$DISPLAY_NUM
WORK=/work/build-linux
APP=$WORK/src/ui/jie2
LOG=/tmp/jie2-clip-save.log
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

python3 -c "from PIL import Image; Image.new('RGB',(60,40),(220,30,30)).save('/tmp/cs-red.png')"
python3 -c "from PIL import Image; Image.new('RGB',(20,20),(30,30,220)).save('/tmp/cs-blue.png')"

xclip -selection clipboard -t image/png -i /tmp/cs-red.png
sleep 0.5

"$APP" >"$LOG" 2>&1 &
APP_PID=$!
sleep 3
if ! kill -0 "$APP_PID" 2>/dev/null; then
    echo "FAIL: 应用未启动"; cat "$LOG"; kill $XVFB_PID 2>/dev/null; exit 1
fi

echo "== 1. 第一次：Ctrl+Alt+C（60x40 红图）=="
xdotool key --clearmodifiers ctrl+alt+c
sleep 1.5

echo "== 2. 换 20x20 蓝图，再按一次 =="
xclip -selection clipboard -t image/png -i /tmp/cs-blue.png
sleep 0.5
xdotool key --clearmodifiers ctrl+alt+c
sleep 1.5

kill "$APP_PID" 2>/dev/null
sleep 0.5
kill "$XVFB_PID" 2>/dev/null

echo
echo "== 日志 =="
grep -E "保存剪贴板" "$LOG" | tail -6

echo
echo "== 截图目录 =="
ls -la "$PICS" 2>/dev/null | tail -5

python3 - <<'PY'
import glob, os, re

log = open("/tmp/jie2-clip-save.log", encoding="utf-8").read()
assert re.search(r"热键触发：保存剪贴板图像", log), "FAIL: 缺少热键触发日志"
assert re.search(r"已保存剪贴板图像", log), "FAIL: 缺少保存成功日志"
print("✓ 日志：热键触发 + 保存成功")

files = sorted(glob.glob("/root/Pictures/jie2/jie2-2*.png"))
# 过滤 jie2-copied-*（剪贴板监听"挂图"的产物，是另一功能的预期行为）
assert len(files) == 2, f"FAIL: 期望 2 个文件，实际 {len(files)}: {files}"

from PIL import Image
sizes = [Image.open(f).size for f in files]
print("保存的图片:", list(zip([os.path.basename(f) for f in files], sizes)))
assert (60, 40) in sizes, f"FAIL: 缺少 60x40 的图: {sizes}"
assert (20, 20) in sizes, f"FAIL: 缺少 20x20 的图: {sizes}"
print("✓ 两张图都按原始尺寸保存")

print("PASS: Ctrl+Alt+C 保存剪贴板图像验证通过")
PY
