#!/usr/bin/env bash
# 「截2」衣架（M3，tendedero 合并）端到端验证
#
# 场景：
#   1. 先截两张图（填充历史）；
#   2. 启动托盘常驻模式（衣架随之创建）；
#   3. 断言日志 "衣架：2 项"；
#   4. 指针移到屏幕顶边 → 断言"衣架展开"；
#   5. 截图检查顶部条带真的画出来了（像素颜色）；
#   6. 点击第一张卡片 → 断言"已复制"且剪贴板里有图像；
#   7. 指针移开 → 断言"衣架收起"。
#
# 用法（容器内）：bash scripts/test-clothesline-linux.sh
set -u

DISPLAY_NUM=:99
export DISPLAY=$DISPLAY_NUM
WORK=/work/build-linux
APP=$WORK/src/ui/jie2
PROBE=$WORK/src/tools/jie2-probe
LOG=/tmp/jie2-clothesline.log
CFG_DIR=/root/.config/jie2

pkill -f "Xvfb $DISPLAY_NUM" 2>/dev/null
sleep 0.5
Xvfb $DISPLAY_NUM -screen 0 1280x800x24 >/dev/null 2>&1 &
XVFB_PID=$!
sleep 1.5

# 清空环境，保证确定性。
rm -rf "$CFG_DIR" /root/Pictures/jie2
rm -f "$LOG"

echo "== 1. 填充历史（两次截图）=="
"$APP" --select-rect 0,0,64,64 >/dev/null 2>&1
"$APP" --select-rect 0,0,96,48 >/dev/null 2>&1
cat "$CFG_DIR/history.txt"

echo "== 2. 启动托盘常驻模式 =="
"$APP" >"$LOG" 2>&1 &
APP_PID=$!
sleep 2.5

echo "== 3. 指针移到屏幕顶边 (640,1) =="
xdotool mousemove --sync 640 1
sleep 1.0

echo "== 4. 截取屏幕（验证条带真的画出来了）=="
"$PROBE" --capture-save /tmp/strip-check.ppm >/dev/null 2>&1

echo "== 5. 点击第一张卡片 =="
# 卡片 1 中心：x = 16 + 150/2 = 91，y = 22 + 104/2 = 74
xdotool mousemove --sync 91 74 sleep 0.3 click 1
sleep 0.8

echo "== 6. 读取剪贴板 =="
xclip -selection clipboard -t image/png -o > /tmp/clip.png 2>/dev/null
CLIP_RC=$?

echo "== 7. 指针移开 → 收起 =="
xdotool mousemove --sync 640 500
sleep 1.0

sleep 0.3
kill $APP_PID 2>/dev/null
sleep 0.3
kill $XVFB_PID 2>/dev/null

echo
echo "== 应用日志 =="
cat "$LOG"
echo

python3 - <<'PY'
import re
import sys

log = open("/tmp/jie2-clothesline.log", encoding="utf-8").read()

def need(pattern, name):
    m = re.search(pattern, log)
    assert m, f"FAIL: 日志缺少「{name}」"
    print(f"✓ {name}: {m.group(0)}")
    return m

m = need(r"衣架：(\d+) 项", "衣架初始化")

assert int(m.group(1)) == 2, f"FAIL: 期望 2 项，实际 {m.group(1)}"

need(r"衣架展开", "顶边触发展开")
need(r"衣架：已复制 .* 到剪贴板", "点击卡片复制")
need(r"衣架收起", "指针离开收起")

# 检查条带像素：条带底色 (26,26,30) 在顶部区域应大量存在。
def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # P6\n<w> <h>\n255\n
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    pixels = parts[3]
    return w, h, pixels

w, h, pixels = read_ppm("/tmp/strip-check.ppm")
# 条带底色实测 ≈ (37,37,40)：半透明的 (26,26,30,242) 叠加在窗口默认底上。
# 卡片底色 ≈ (40,40,46)。两者都在 30..50 的灰阶范围，而条带之下是纯黑。
strip_hits = 0
total = 0
for y in range(5, 140, 4):
    for x in range(10, w - 10, 8):
        idx = (y * w + x) * 3
        r, g, b = pixels[idx], pixels[idx + 1], pixels[idx + 2]
        total += 1
        if 30 <= r <= 52 and 30 <= g <= 52 and 33 <= b <= 56:
            strip_hits += 1
print(f"顶部区域条带底色命中: {strip_hits}/{total}")
assert strip_hits > total * 0.3, "FAIL: 顶部条带未画出"
# 对照：条带之下（y=400）应为纯黑背景（Xvfb 空桌面），确认条带不是全屏图案。
below_hit = 0
below_total = 0
for y in range(300, 460, 8):
    for x in range(10, w - 10, 16):
        idx = (y * w + x) * 3
        r, g, b = pixels[idx], pixels[idx + 1], pixels[idx + 2]
        below_total += 1
        if r < 12 and g < 12 and b < 12:
            below_hit += 1
print(f"条带之下黑色命中: {below_hit}/{below_total}")
assert below_hit > below_total * 0.9, "FAIL: 条带之下不是背景色"

import os
size = os.path.getsize("/tmp/clip.png")
print(f"剪贴板 PNG 大小: {size} 字节")
assert size > 100, "FAIL: 剪贴板里没有图像"

print("PASS: 衣架端到端验证通过（初始化/展开/复制/收起 + 条带渲染 + 剪贴板）")
PY
