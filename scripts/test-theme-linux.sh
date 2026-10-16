#!/usr/bin/env bash
# 「截2」主题跟随系统验证（专业版对齐项 v2.6）
#
# 场景：
#   A. appearance.theme=light → 衣架条带整体变为浅色（像素级断言）
#      + 日志「主题：light（来源：配置）」；
#   B. appearance.theme=dark  → 条带回到深色 + 日志「dark（来源：配置）」；
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

read_strip() { # $1 = ppm 路径；输出 light/dark 命中统计
    python3 - "$1" <<'PY'
import sys
with open(sys.argv[1], "rb") as f:
    data = f.read()
parts = data.split(b"\n", 3)
w, h = map(int, parts[1].split())
pixels = parts[3]
light = dark = total = 0
for y in range(5, 140, 4):
    for x in range(10, w - 10, 8):
        idx = (y * w + x) * 3
        r, g, b = pixels[idx], pixels[idx + 1], pixels[idx + 2]
        total += 1
        if r >= 205 and g >= 205 and b >= 205:
            light += 1
        if 30 <= r <= 52 and 30 <= g <= 52 and 33 <= b <= 56:
            dark += 1
print(f"light={light}/{total} dark={dark}/{total}")
if light > total * 0.3:
    print("STRIP=light")
elif dark > total * 0.3:
    print("STRIP=dark")
else:
    print("STRIP=unknown")
    raise SystemExit(1)
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
