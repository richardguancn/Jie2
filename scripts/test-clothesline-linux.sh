#!/usr/bin/env bash
# 「J2」衣架（M3，tendedero 合并）端到端验证
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

# 清空环境，保证确定性。
rm -rf "$CFG_DIR" /root/Pictures/jie2
rm -f "$LOG"

# 主题固定为深色：本脚本的像素断言基于深色主题下的玻璃卡片配色 ——
# 不固定的话，运行环境的系统主题（gnome color-scheme / 容器 gsettings）
# 会改变卡片玻璃底配色，让像素断言随环境漂移。
mkdir -p "$CFG_DIR"
cat > "$CFG_DIR/jie2.conf" <<EOF
appearance.theme=dark
EOF

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
# tendedero 样式布局（居中排布，1280 宽、2 张卡、间距 174）：
#   卡 1（64×64 方图）中心 x = 640 - 87 = 553；照片 104×104，卡 112×112，
#   卡顶 = ropeY(553) + 4.5 ≈ 32.6 + 4.5 = 37.1 → 中心 y ≈ 93。
xdotool mousemove --sync 553 93 sleep 0.3 click 1
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

def at(x, y):
    idx = (y * w + x) * 3
    return pixels[idx], pixels[idx + 1], pixels[idx + 2]

# tendedero 样式的像素判据（无旧"条带底色"——面板已改为透明）：
# ① 抛物线绳：中心 (640,~33) 有中性灰绳芯（≈140）。
rope = at(640, 33)
print(f"绳芯 (640,33): {rope}")
# 1.2px 细线经抗锯齿后峰值会有衰减（实测 94），容差放宽到 70..190
assert 70 <= rope[0] <= 190 and abs(rope[0] - rope[1]) < 15 and \
    abs(rope[1] - rope[2]) < 15, "FAIL: 晾衣绳未画出"

# ② 衣夹：卡 1 悬挂点 (553, 23..50) 内有拉丝金属亮像素。
clip_bright = 0
for y in range(24, 48):
    for x in range(547, 560):
        r, g, b = at(x, y)
        if r > 140 and g > 140 and b > 140:
            clip_bright += 1
print(f"衣夹亮像素: {clip_bright}")
assert clip_bright >= 3, "FAIL: 衣夹未画出"

# ③ 透明面板：无绳无卡区域应透出桌面（Xvfb 空桌面 = 纯黑）。
#    卡片区 x∈[481,799]、绳区 y≤35，采样 x∈[60,400], y∈[60,180] 避开。
transparent_hits = 0
total = 0
for y in range(60, 180, 6):
    for x in range(60, 400, 8):
        r, g, b = at(x, y)
        total += 1
        if r < 12 and g < 12 and b < 12:
            transparent_hits += 1
print(f"透明区黑色命中: {transparent_hits}/{total}")
assert transparent_hits > total * 0.9, "FAIL: 面板不透明（还有旧色板条）"

# 对照：条带之下（y=300..460）应为纯黑背景，确认画面不是全屏图案。
below_hit = 0
below_total = 0
for y in range(300, 460, 8):
    for x in range(10, w - 10, 16):
        r, g, b = at(x, y)
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
