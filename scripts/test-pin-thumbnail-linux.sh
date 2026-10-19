#!/usr/bin/env bash
# 「J2」缩略图快捷键自定义验证（专业版对齐项 v2.11）
#
# 场景：
#   A. 默认（shift）：贴图 → Shift+双击 → 断言"缩略图模式"日志
#   B. 配置 pin.thumbnail_modifier=ctrl：重启 → 贴图 → Ctrl+双击 →
#      断言"缩略图模式"；再 Shift+双击 → 断言"贴图关闭"（回落到关闭语义）
#
# 用法（容器内）：bash scripts/test-pin-thumbnail-linux.sh
set -u

DISPLAY_NUM=:93
export DISPLAY=$DISPLAY_NUM
WORK=/work/build-linux
APP=$WORK/src/ui/jie2
CFG_DIR=/root/.config/jie2
LOG=/tmp/jie2-pin-thumb.log

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

python3 -c "from PIL import Image; Image.new('RGB',(40,30),(255,0,0)).save('/tmp/th-red.png')"

start_app() {
    rm -f /tmp/jie2-single-root
    xclip -selection clipboard -t image/png -i /tmp/th-red.png
    sleep 0.3
    "$APP" >"$LOG" 2>&1 &
    APP_PID=$!
    sleep 3
    if ! kill -0 "$APP_PID" 2>/dev/null; then
        echo "FAIL: 应用未启动"; cat "$LOG"; exit 1
    fi
}

pin_once() {
    xdotool key --clearmodifiers ctrl+alt+p
    sleep 1.2
}

find_pin_windows() {
    for id in $(xdotool search --class "jie2" 2>/dev/null); do
        name=$(xdotool getwindowname "$id" 2>/dev/null || true)
        case "$name" in
            *贴图*) echo "$id" ;;
        esac
    done
}

focus_center() {
    local id=$1 geo cx cy
    geo=$(xdotool getwindowgeometry --shell "$id" 2>/dev/null)
    cx=$(( $(echo "$geo" | grep -oE 'X=[0-9]+' | cut -d= -f2) + \
          $(echo "$geo" | grep -oE 'WIDTH=[0-9]+' | cut -d= -f2) / 2 ))
    cy=$(( $(echo "$geo" | grep -oE 'Y=[0-9]+' | cut -d= -f2) + \
          $(echo "$geo" | grep -oE 'HEIGHT=[0-9]+' | cut -d= -f2) / 2 ))
    xdotool mousemove --sync "$cx" "$cy" sleep 0.15 click 1
    sleep 0.3
}

double_click_center() {
    local id=$1 geo cx cy
    geo=$(xdotool getwindowgeometry --shell "$id" 2>/dev/null)
    cx=$(( $(echo "$geo" | grep -oE 'X=[0-9]+' | cut -d= -f2) + \
          $(echo "$geo" | grep -oE 'WIDTH=[0-9]+' | cut -d= -f2) / 2 ))
    cy=$(( $(echo "$geo" | grep -oE 'Y=[0-9]+' | cut -d= -f2) + \
          $(echo "$geo" | grep -oE 'HEIGHT=[0-9]+' | cut -d= -f2) / 2 ))
    xdotool mousemove --sync "$cx" "$cy" sleep 0.2
    xdotool click --repeat 2 --delay 80 1
    sleep 0.4
}

# ============ A. 默认 shift ============
echo "== A. 默认（shift+双击 = 缩略图）=="
rm -rf "$CFG_DIR" /root/Pictures/jie2
start_app
pin_once
WINS=($(find_pin_windows))
[ "${#WINS[@]}" -ge 1 ] || { echo "FAIL: 未创建贴图"; cat "$LOG"; exit 1; }
focus_center "${WINS[0]}"
# Shift+双击（按住修饰键做双击）
xdotool keydown shift
double_click_center "${WINS[0]}"
xdotool keyup shift
sleep 0.5
grep -c "缩略图模式" "$LOG" | sed 's/^/   A 段缩略图日志: /'
cp "$LOG" /tmp/jie2-thumb-a.log
kill "$APP_PID" 2>/dev/null
sleep 0.5

# ============ B. ctrl 配置 ============
echo "== B. 配置 ctrl+双击 = 缩略图 =="
mkdir -p "$CFG_DIR"
echo "pin.thumbnail_modifier=ctrl" > "$CFG_DIR/jie2.conf"
rm -f "$LOG"
start_app
pin_once
WINS=($(find_pin_windows))
[ "${#WINS[@]}" -ge 1 ] || { echo "FAIL: 未创建贴图"; cat "$LOG"; exit 1; }

focus_center "${WINS[0]}"
xdotool keydown ctrl
double_click_center "${WINS[0]}"
xdotool keyup ctrl
sleep 0.5
grep -c "缩略图模式" "$LOG" | sed 's/^/   B 段(ctrl)缩略图日志: /'

# Shift+双击 → 应走关闭
focus_center "${WINS[0]}" 2>/dev/null || true
xdotool keydown shift
double_click_center "${WINS[0]}"
xdotool keyup shift
sleep 0.5

kill "$APP_PID" 2>/dev/null
kill "$XVFB_PID" 2>/dev/null

echo
echo "== 日志（B 段）=="
grep -E "缩略图|关闭" "$LOG" | tail -5

python3 - <<'PY'
import re

log_a = open("/tmp/jie2-thumb-a.log", encoding="utf-8").read()
log_b = open("/tmp/jie2-pin-thumb.log", encoding="utf-8").read()

assert re.search(r"贴图缩略图模式", log_a), "FAIL: A 段缺少『缩略图模式』（默认 Shift+双击未生效）"
print("✓ A 段：默认 Shift+双击 → 缩略图模式")
assert re.search(r"贴图缩略图模式", log_b), "FAIL: B 段缺少『缩略图模式』（Ctrl+双击未生效）"
print("✓ B 段：Ctrl+双击 → 缩略图模式（配置 pin.thumbnail_modifier=ctrl 生效）")
assert re.search(r"贴图关闭（双击）", log_b), "FAIL: B 段缺少『贴图关闭』（Shift+双击未回落关闭）"
print("✓ B 段：Shift+双击 → 关闭（不再触发缩略图）")
print("PASS: 缩略图快捷键自定义验证通过")
PY
