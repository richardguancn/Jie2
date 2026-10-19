#!/usr/bin/env bash
# 「J2」贴图 Esc 行为自定义验证（专业版对齐项 v2.8）
#
# 场景：
#   A. 默认配置（esc=close）：贴 1 张 → Esc → 断言"贴图关闭"日志 + 窗口消失
#   B. 配置 pin.esc_action=hide：重启 → 贴 1 张 → Esc →
#      断言"贴图隐藏"日志 + 窗口仍存在但不可见（xdotool --onlyvisible 找不到）
#
# 用法（容器内）：bash scripts/test-pin-esc-linux.sh
set -u

DISPLAY_NUM=:95
export DISPLAY=$DISPLAY_NUM
WORK=/work/build-linux
APP=$WORK/src/ui/jie2
CFG_DIR=/root/.config/jie2
CFG=$CFG_DIR/jie2.conf
LOG=/tmp/jie2-pin-esc.log
XVFB_PID=""
APP_PID=""

cleanup() {
    [ -n "$APP_PID" ] && kill "$APP_PID" 2>/dev/null
    [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null
    return 0
}
trap cleanup EXIT

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

python3 -c "from PIL import Image; Image.new('RGB',(40,30),(255,0,0)).save('/tmp/esc-red.png')"

start_app() {
    rm -f /tmp/jie2-single-root
    xclip -selection clipboard -t image/png -i /tmp/esc-red.png
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

find_pin_windows() {  # $1: --onlyvisible 可选
    for id in $(xdotool search ${1:-} --class "jie2" 2>/dev/null); do
        name=$(xdotool getwindowname "$id" 2>/dev/null || true)
        case "$name" in
            *贴图*) echo "$id" ;;
        esac
    done
}

wait_window_focus() {  # 聚焦某个窗口（X 下用 setfocus 或点击中心）
    local id=$1
    local geo cx cy
    geo=$(xdotool getwindowgeometry --shell "$id" 2>/dev/null)
    cx=$(( $(echo "$geo" | grep -oE 'X=[0-9]+' | cut -d= -f2) + \
          $(echo "$geo" | grep -oE 'WIDTH=[0-9]+' | cut -d= -f2) / 2 ))
    cy=$(( $(echo "$geo" | grep -oE 'Y=[0-9]+' | cut -d= -f2) + \
          $(echo "$geo" | grep -oE 'HEIGHT=[0-9]+' | cut -d= -f2) / 2 ))
    xdotool mousemove --sync "$cx" "$cy" sleep 0.15 click 1
    sleep 0.3
}

# ============ A. 默认：Esc = 关闭 ============
echo "== A. 默认配置（esc=close）=="
rm -rf "$CFG_DIR" /root/Pictures/jie2
start_app
pin_once
WINS=($(find_pin_windows))
echo "   贴图窗口: ${WINS[*]:-无}"
if [ "${#WINS[@]}" -lt 1 ]; then echo "FAIL: 未创建贴图"; cat "$LOG"; exit 1; fi
wait_window_focus "${WINS[0]}"
xdotool key --clearmodifiers Escape
sleep 0.8
VISIBLE=($(find_pin_windows --onlyvisible))
echo "   Esc 后可见贴图窗口: ${VISIBLE[*]:-无}"
if [ "${#VISIBLE[@]}" -ne 0 ]; then
    echo "FAIL: esc=close 后不应再有可见贴图窗口"
    exit 1
fi
cp "$LOG" /tmp/jie2-esc-a.log   # A 段日志留档（B 段会覆盖工作日志）
kill "$APP_PID" 2>/dev/null; APP_PID=""
sleep 0.5

# ============ B. 自定义：Esc = 隐藏 ============
echo "== B. 配置 pin.esc_action=hide =="
mkdir -p "$CFG_DIR"
cat > "$CFG" <<EOF
pin.esc_action=hide
EOF
rm -f "$LOG"
start_app
pin_once
WINS=($(find_pin_windows))
echo "   贴图窗口: ${WINS[*]:-无}"
if [ "${#WINS[@]}" -lt 1 ]; then echo "FAIL: 未创建贴图"; cat "$LOG"; exit 1; fi
wait_window_focus "${WINS[0]}"
xdotool key --clearmodifiers Escape
sleep 0.8
VISIBLE=($(find_pin_windows --onlyvisible))
ALL=($(find_pin_windows))
echo "   Esc 后：全部=${ALL[*]:-无}  可见=${VISIBLE[*]:-无}"
# esc=hide：窗口应保留（全部>0）但不可见（可见=0）
if [ "${#ALL[@]}" -lt 1 ]; then
    echo "FAIL: esc=hide 后窗口应保留在列表里"
    exit 1
fi
if [ "${#VISIBLE[@]}" -ne 0 ]; then
    echo "FAIL: esc=hide 后不应有可见窗口"
    exit 1
fi

kill "$APP_PID" 2>/dev/null; APP_PID=""
kill "$XVFB_PID" 2>/dev/null; XVFB_PID=""

echo
echo "== 日志（B 段）=="
grep -E "贴图关闭|贴图隐藏" "$LOG" | tail -6

python3 - <<'PY'
import re

log_a = open("/tmp/jie2-esc-a.log", encoding="utf-8").read()
log_b = open("/tmp/jie2-pin-esc.log", encoding="utf-8").read()

assert re.search(r"贴图关闭（Esc）", log_a), "FAIL: A 段缺少『贴图关闭』日志"
print("✓ A 段（默认配置）：Esc →『贴图关闭』且窗口销毁")
assert re.search(r"贴图隐藏（Esc", log_b), "FAIL: B 段缺少『贴图隐藏』日志"
print("✓ B 段（pin.esc_action=hide）：Esc →『贴图隐藏』且窗口保留不可见")
PY

echo
echo "PASS: 贴图 Esc 行为自定义验证通过"
