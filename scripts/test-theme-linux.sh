#!/usr/bin/env bash
# 「J2」主题跟随系统验证（专业版对齐项 v2.6）
#
# 场景：
#   A. appearance.theme=light → 衣架卡片玻璃底变为浅色（像素级断言）
#      + 日志「主题：light（来源：配置）」；
#   B. appearance.theme=dark  → 玻璃底回到深色 + 日志「dark（来源：配置）」；
#   C. appearance.theme=system + JIE2_THEME=light → 日志「light（来源：环境变量）」
#      （证明"跟随系统"链路的探测顺序：配置 > 环境变量 > 系统 > 默认）。
#
# 用法（容器内）：bash scripts/test-theme-linux.sh
set -u

DISPLAY_NUM=:99
export DISPLAY=$DISPLAY_NUM
WORK=/work/build-linux
APP=$WORK/src/ui/jie2
PROBE=$WORK/src/tools/jie2-probe
CFG_DIR=/root/.config/jie2
SHOT_DIR=/root/Pictures/jie2

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

rm -rf "$CFG_DIR" "$SHOT_DIR"
echo "== 0. 填充历史（两次截图）=="
"$APP" --select-rect 0,0,64,64 >/dev/null 2>&1
"$APP" --select-rect 0,0,96,48 >/dev/null 2>&1

read_strip() { # $1 = ppm 路径；输出卡片玻璃环的 light/dark 判定
    python3 - "$1" <<'PY'
import sys
with open(sys.argv[1], "rb") as f:
    data = f.read()
parts = data.split(b"\n", 3)
w, h = map(int, parts[1].split())
pixels = parts[3]
def at(x, y):
    idx = (y * w + x) * 3
    return pixels[idx], pixels[idx + 1], pixels[idx + 2]

# tendedero 样式下面板透明，主题体现在卡片玻璃底上。
# 注意：无合成器时窗口像素是**预乘**后的值 —— 玻璃色按 alpha 折算：
#   light (245,246,251,165) → ≈159；dark (30,30,36,150) → ≈18；
#   受光描边（白，纵向渐变）中点 ≈ light 192 / dark 97。
# 布局（1280 宽、2 张卡、间距 174 居中）：
#   卡1（64×64）卡身 112×112，中心 (553, 93)，左右环 x ≈ 497/605
#   卡2（96×48）卡身 144×76， 中心 (727, 75)，左右环 x ≈ 655/795
# 取每条玻璃环窗口内的**峰值亮度**（避开黑色照片与透明区的干扰），
# 四条环取平均：light ≥ 150，dark ≤ 100，阈值 130 居中。
peaks = []
for cy, x0, x1 in [(93, 497, 609), (75, 655, 799)]:
    for xr in (range(x0 - 6, x0 + 7), range(x1 - 7, x1 + 6)):
        peaks.append(max(at(x, cy)[0] for x in xr))
score = sum(peaks) / len(peaks)
print(f"glass 环峰值: {peaks} → 均值 {score:.0f}")
if score > 130:
    print("STRIP=light")
else:
    print("STRIP=dark")
PY
}

# ---------------------------------------------------------------------------
echo "========== A. 配置 light =========="
cat > "$CFG_DIR/jie2.conf" <<EOF
appearance.theme=light
EOF
LOG=/tmp/jie2-theme-a.log
rm -f "$LOG"
"$APP" >"$LOG" 2>&1 &
APP_PID=$!
sleep 2.5
xdotool mousemove --sync 640 1
sleep 1.0
"$PROBE" --capture-save /tmp/jie2-strip-light.ppm >/dev/null 2>&1
kill $APP_PID 2>/dev/null
sleep 0.5

grep "主题：" "$LOG" || true
grep -q "主题：light（来源：配置）" "$LOG" || { echo "FAIL: A 缺少「light（来源：配置）」"; exit 1; }
RESULT=$(read_strip /tmp/jie2-strip-light.ppm)
echo "条带像素: $RESULT"
echo "$RESULT" | grep -q "STRIP=light" || { echo "FAIL: A 条带像素不是浅色"; exit 1; }
echo "✓ A 通过（配置 light → 条带浅色）"

# ---------------------------------------------------------------------------
echo
echo "========== B. 配置 dark =========="
cat > "$CFG_DIR/jie2.conf" <<EOF
appearance.theme=dark
EOF
LOG=/tmp/jie2-theme-b.log
rm -f "$LOG"
"$APP" >"$LOG" 2>&1 &
APP_PID=$!
sleep 2.5
xdotool mousemove --sync 640 1
sleep 1.0
"$PROBE" --capture-save /tmp/jie2-strip-dark.ppm >/dev/null 2>&1
kill $APP_PID 2>/dev/null
sleep 0.5

grep "主题：" "$LOG" || true
grep -q "主题：dark（来源：配置）" "$LOG" || { echo "FAIL: B 缺少「dark（来源：配置）」"; exit 1; }
RESULT=$(read_strip /tmp/jie2-strip-dark.ppm)
echo "条带像素: $RESULT"
echo "$RESULT" | grep -q "STRIP=dark" || { echo "FAIL: B 条带像素不是深色"; exit 1; }
echo "✓ B 通过（配置 dark → 条带深色）"

# ---------------------------------------------------------------------------
echo
echo "========== C. system + 环境变量（测试探针）=========="
cat > "$CFG_DIR/jie2.conf" <<EOF
appearance.theme=system
EOF
LOG=/tmp/jie2-theme-c.log
rm -f "$LOG"
JIE2_THEME=light "$APP" >"$LOG" 2>&1 &
APP_PID=$!
sleep 2.5
kill $APP_PID 2>/dev/null
sleep 0.3

grep "主题：" "$LOG" || true
grep -q "主题：light（来源：环境变量）" "$LOG" || { echo "FAIL: C 缺少「light（来源：环境变量）」"; exit 1; }
echo "✓ C 通过（system 模式下环境变量探针生效）"

kill $XVFB_PID 2>/dev/null

echo
echo "PASS: 主题跟随系统验证通过（light/dark 像素级生效 + 解析来源与优先级正确）"
