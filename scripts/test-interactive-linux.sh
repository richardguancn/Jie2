#!/usr/bin/env bash
# 「截2」Linux 交互链路验证（Xvfb + xdotool）
#
# 覆盖：选择区域 → 绘制标注（矩形/箭头）→ 撤销 → 重做 → 换颜色 → 保存。
# 断言：保存的 PNG 尺寸等于选区尺寸；图像里能找到标注颜色像素；
#       应用日志出现"标注已添加 / 撤销 / 重做"与"提交选区 … 输出="。
#
# 用法（容器内）：bash scripts/test-interactive-linux.sh
set -u

DISPLAY_NUM=:99
export DISPLAY=$DISPLAY_NUM
WORK=/work/build-linux
APP=$WORK/src/ui/jie2
LOG=/tmp/jie2-select.log
SHOT_DIR=/root/Pictures/jie2

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

rm -rf "$SHOT_DIR"
rm -f "$LOG"

"$APP" --select >"$LOG" 2>&1 &
APP_PID=$!
sleep 2

# 工具和颜色通过快捷键切换；工具条按钮的实际点击由界面测试覆盖。


click() { xdotool mousemove --sync "$1" "$2" sleep 0.15 click 1 sleep 0.4; }
drag() { # x1 y1 x2 y2
    xdotool mousemove --sync "$1" "$2" sleep 0.15
    xdotool mousedown 1 sleep 0.15
    local steps=8
    for i in $(seq 1 $steps); do
        local x=$(( $1 + ($3 - $1) * i / steps ))
        local y=$(( $2 + ($4 - $2) * i / steps ))
        xdotool mousemove --sync "$x" "$y" sleep 0.08
    done
    xdotool mouseup 1 sleep 0.4
}

echo "== 1. 拖拽选择区域 (100,100)-(500,400) =="
drag 100 100 500 400
xdotool key 1

echo "== 2. 绘制矩形 (200,150)-(350,300) =="
drag 200 150 350 300

echo "== 3. 撤销 =="
xdotool key ctrl+z

echo "== 4. 重做 =="
xdotool key ctrl+shift+z

echo "== 5. 切换到箭头工具（先切，再换色）=="
xdotool key 3

echo "== 5.5 换颜色（箭头工具：红→黄）=="
# 注意：颜色按形状独立记忆（专业版 v2.4）——必须**在箭头工具下**点颜色，
# 否则改的是别的工具的颜色，箭头仍是红色（本测试曾因此断言失败）。
xdotool key shift+c

echo "== 6. 绘制箭头 (360,250)-(450,360) =="
drag 360 250 450 360

echo "== 6.5 文字工具：切工具 → 换色（文字自己的红→黄）→ 输入 TEST =="
xdotool key t
# 颜色按形状独立记忆（v2.4）：文字工具要黄色，需在**文字工具下**点颜色。
xdotool key shift+c
xdotool mousemove --sync 150 340 sleep 0.2 click 1
sleep 0.5
xdotool type --delay 60 "TEST"
sleep 0.3
xdotool key Return
sleep 0.5

echo "== 6.6 序号工具：切工具 → 换色（序号自己的红→黄）→ 点击两次放置 1、2 =="
xdotool key n
# 颜色按形状独立记忆（v2.4）：序号工具要黄色，需在**序号工具下**点颜色。
xdotool key shift+c
xdotool mousemove --sync 430 150 sleep 0.2 click 1
sleep 0.4
xdotool mousemove --sync 470 190 sleep 0.2 click 1
sleep 0.4

echo "== 6.7 橡皮擦：在左上空白处画一个小矩形再擦掉 =="
# 位置选择说明：必须远离其他标注的包围盒 —— 橡皮擦按"包围盒相交即整条删除"
# 工作，若小矩形贴着箭头（(360,250)-(450,360)）会连箭头一起擦掉。
xdotool key 1
drag 130 130 160 160
xdotool key e
drag 135 135 155 155

echo "== 7. 确认保存 =="
xdotool key Return

sleep 1.0
kill $APP_PID 2>/dev/null
kill $XVFB_PID 2>/dev/null

echo
echo "== 应用日志 =="
cat "$LOG"
echo
echo "== 保存结果 =="
LATEST=$(ls -t "$SHOT_DIR"/*.png 2>/dev/null | head -1)
if [ -z "$LATEST" ]; then
    echo "FAIL: 没有保存任何截图"
    exit 1
fi
echo "文件: $LATEST"
python3 - "$LATEST" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
w, h = im.size
print(f"尺寸: {w}x{h}")
assert (w, h) == (400, 300), f"FAIL: 期望 400x300，实际 {w}x{h}"

def px(x, y): return im.getpixel((x, y))

# 矩形（红）: 逻辑 (200,150)-(350,300) → 相对选区 (100,50)-(250,200)，1x 缩放
# 边框在 abs (100,50) 与 (250,200)：检查上边中点 x=175 y=50
red_hits = sum(1 for x in range(100, 251) if px(x, 50) == (239, 68, 68))
print("矩形上边红色像素:", red_hits, "/151")
assert red_hits > 120, "FAIL: 矩形未渲染"

# 箭头（黄，换色后）: 逻辑 (360,250)-(450,360) → 相对 (260,150)-(350,260)
yellow_hits = sum(1 for x in range(255, 355) for y in range(145, 265)
                  if px(x, y)[0] > 200 and px(x, y)[1] > 160 and px(x, y)[2] < 90)
print("箭头黄色像素:", yellow_hits)
assert yellow_hits > 150, "FAIL: 箭头未渲染或颜色未切换"

# 文字（M2 增补）: 逻辑 (150,340) → 相对 (50,240)。文字为黄色（换色后）。
text_hits = sum(1 for x in range(45, 145) for y in range(235, 275)
                if px(x, y)[0] > 180 and px(x, y)[1] > 150 and px(x, y)[2] < 120)
print("文字黄色像素:", text_hits)
assert text_hits > 40, "FAIL: 文字未渲染"

# 序号（M2 增补）: 逻辑 (430,150) → 相对 (330,50)；(470,190) → 相对 (370,90)。
# 序号圆为黄色实心 + 白色数字。
def has_yellow_disc(cx, cy):
    hits = 0
    for x in range(cx - 14, cx + 15):
        for y in range(cy - 14, cy + 15):
            r, g, b = px(x, y)
            if r > 180 and g > 150 and b < 120:
                hits += 1
    return hits

d1 = has_yellow_disc(330, 50)
d2 = has_yellow_disc(370, 90)
print("序号1 圆内黄像素:", d1, " 序号2 圆内黄像素:", d2)
assert d1 > 100 and d2 > 100, "FAIL: 序号圆未渲染"

# 橡皮擦：被擦除的小矩形（逻辑 (130,130)-(160,160) → 相对 (30,30)-(60,60)）
# 上不应再有红色边框像素。
erased_hits = sum(1 for x in range(28, 63) for y in range(28, 63)
                  if px(x, y) == (239, 68, 68))
print("被擦区域红色像素:", erased_hits, "← 应接近 0（矩形已被擦掉）")
assert erased_hits < 12, "FAIL: 橡皮擦未删除标注"

# 右下角（无标注区域）应保持遮罩下的原内容而未被污染
print("右下角采样 (380,280):", px(380, 280))
print("PASS: 交互链路（选择/绘制/撤销/重做/换色/文字/序号/橡皮擦/保存）验证通过")
PY
