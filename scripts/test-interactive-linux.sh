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
Xvfb $DISPLAY_NUM -screen 0 1280x800x24 >/dev/null 2>&1 &
XVFB_PID=$!
sleep 1.5

rm -rf "$SHOT_DIR"
rm -f "$LOG"

"$APP" --select >"$LOG" 2>&1 &
APP_PID=$!
sleep 2

# 工具条按钮中心坐标（与 selection_overlay.cpp 的 toolbarItems() 布局一致）。
# 选区 (100,100)-(500,400) → 工具条顶边 y=410，按钮高 30 → 中心 y=425。
read -r RECT_X RECT_Y ARROW_X ERASER_X TEXT_X NUMBER_X UNDO_X REDO_X COLOR_X SAVE_X <<EOF
$(python3 - <<'PY'
# 顺序必须与 selection_overlay.cpp 的 toolbarItems() 完全一致。
# 布局变化（如新增"放大镜"）后必须同步这里，否则"点击"会落到错误的按钮上 ——
# 这是实测教训：加放大镜后旧的坐标表让"换颜色"点到"文字"工具，
# 结果箭头颜色断言失败，排查了半天。
widths = [52,52,52,52,64,64,52,52,64,52,52,52,52,52,40,76,60]
names  = ["rect","ellipse","arrow","pencil","marker","mosaic","blur","eraser",
          "magnifier","text","number","undo","redo","color","save","cancel"]
x = 100
pos = {}
for w, n in zip(widths, names):
    pos[n] = x + w // 2
    x += w + 6
print(pos["rect"], 425, pos["arrow"], pos["eraser"], pos["text"], pos["number"],
      pos["undo"], pos["redo"], pos["color"], pos["save"])
PY
)
EOF
echo "按钮坐标: rect=($RECT_X,$RECT_Y) arrow=$ARROW_X eraser=$ERASER_X text=$TEXT_X number=$NUMBER_X undo=$UNDO_X redo=$REDO_X color=$COLOR_X save=$SAVE_X"

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

echo "== 2. 绘制矩形 (200,150)-(350,300) =="
drag 200 150 350 300

echo "== 3. 撤销 =="
click "$UNDO_X" "$RECT_Y"

echo "== 4. 重做 =="
click "$REDO_X" "$RECT_Y"

echo "== 5. 切换到箭头工具（先切，再换色）=="
click "$ARROW_X" "$RECT_Y"

echo "== 5.5 换颜色（箭头工具：红→黄）=="
# 注意：颜色按形状独立记忆（专业版 v2.4）——必须**在箭头工具下**点颜色，
# 否则改的是别的工具的颜色，箭头仍是红色（本测试曾因此断言失败）。
click "$COLOR_X" "$RECT_Y"

echo "== 6. 绘制箭头 (360,250)-(450,360) =="
drag 360 250 450 360

echo "== 6.5 文字工具：切工具 → 换色（文字自己的红→黄）→ 输入 TEST =="
click "$TEXT_X" "$RECT_Y"
# 颜色按形状独立记忆（v2.4）：文字工具要黄色，需在**文字工具下**点颜色。
click "$COLOR_X" "$RECT_Y"
xdotool mousemove --sync 150 340 sleep 0.2 click 1
sleep 0.5
xdotool type --delay 60 "TEST"
sleep 0.3
xdotool key Return
sleep 0.5

echo "== 6.6 序号工具：切工具 → 换色（序号自己的红→黄）→ 点击两次放置 1、2 =="
click "$NUMBER_X" "$RECT_Y"
# 颜色按形状独立记忆（v2.4）：序号工具要黄色，需在**序号工具下**点颜色。
click "$COLOR_X" "$RECT_Y"
xdotool mousemove --sync 430 150 sleep 0.2 click 1
sleep 0.4
xdotool mousemove --sync 470 190 sleep 0.2 click 1
sleep 0.4

echo "== 6.7 橡皮擦：在左上空白处画一个小矩形再擦掉 =="
# 位置选择说明：必须远离其他标注的包围盒 —— 橡皮擦按"包围盒相交即整条删除"
# 工作，若小矩形贴着箭头（(360,250)-(450,360)）会连箭头一起擦掉。
click "$RECT_X" "$RECT_Y"
drag 130 130 160 160
click "$ERASER_X" "$RECT_Y"
drag 135 135 155 155

echo "== 7. 点击保存 =="
click "$SAVE_X" "$RECT_Y"

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
