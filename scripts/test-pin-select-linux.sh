#!/usr/bin/env bash
# 「截2」贴图全选与拖动联动验证（专业版对齐项 v2.4）
#
# 场景（真实路径）：
#   1. 剪贴板放一张图，热键 Ctrl+Alt+P 贴 3 张
#   2. 在贴图 1 上按 Ctrl+A → 断言日志"贴图全选（3 张）"
#   3. 拖动贴图 1（+120,+60）→ 断言贴图 2、3 的位置同步位移（联动）
#   4. 再按 Ctrl+A → 断言"取消全选"；此后拖动贴图 1 → 贴图 2、3不动
#
# 用法（容器内）：bash scripts/test-pin-select-linux.sh
set -u

# 用独立 display 号（:96）：容器里 :99 被很多旧测试用过，状态可能不干净；
# 独立编号 + socket 就绪等待让本脚本对历史状态免疫。
DISPLAY_NUM=:96
export DISPLAY=$DISPLAY_NUM
WORK=/work/build-linux
APP=$WORK/src/ui/jie2
LOG=/tmp/jie2-pin-select.log

pkill -f "Xvfb $DISPLAY_NUM" 2>/dev/null
pkill -f "build-linux/src/ui" 2>/dev/null
pkill -f xclip 2>/dev/null
sleep 0.5
rm -f "/tmp/.X${DISPLAY_NUM#:}-lock" "/tmp/.X11-unix/X${DISPLAY_NUM#:}" 2>/dev/null
Xvfb $DISPLAY_NUM -screen 0 1280x800x24 >/dev/null 2>&1 &
XVFB_PID=$!
# 等 X socket 就绪（最多 5 秒）
for i in $(seq 1 25); do
    [ -S "/tmp/.X11-unix/X${DISPLAY_NUM#:}" ] && break
    sleep 0.2
done
if ! kill -0 "$XVFB_PID" 2>/dev/null; then
    echo "FAIL: Xvfb 未存活"
    exit 1
fi
sleep 1

# 清理上一次运行的残留：配置、图片目录、**单实例 socket**
# （若有残留 socket，新实例会尝试唤起不存在的旧实例后退出，导致热键无响应）。
rm -rf /root/.config/jie2 /root/Pictures/jie2
rm -f "$LOG" /tmp/jie2-single-root 2>/dev/null

# 剪贴板放一张红图（40x30）
python3 -c "from PIL import Image; Image.new('RGB',(40,30),(255,0,0)).save('/tmp/ps-red.png')"
xclip -selection clipboard -t image/png -i /tmp/ps-red.png
sleep 0.5

"$APP" >"$LOG" 2>&1 &
APP_PID=$!
sleep 3
if ! kill -0 "$APP_PID" 2>/dev/null; then
    echo "FAIL: 应用未启动"
    cat "$LOG"
    kill $XVFB_PID 2>/dev/null
    exit 1
fi

echo "== 贴 3 张（热键 Ctrl+Alt+P）=="
for i in 1 2 3; do
    xdotool key --clearmodifiers ctrl+alt+p
    sleep 1.2
done
sleep 1.0

# 找贴图窗口：**先按 class 搜索**（xdotool search --name 对 UTF-8 中文标题
# 失效 —— 已知限制，--class 可靠），再用 getwindowname 客户端过滤。
mapfile -t WINS < <(for id in $(xdotool search --class "jie2" 2>/dev/null); do
    name=$(xdotool getwindowname "$id" 2>/dev/null || true)
    case "$name" in
        *贴图*) echo "$id" ;;
    esac
done)
echo "贴图窗口数: ${#WINS[@]}"
if [ "${#WINS[@]}" -lt 3 ]; then
    echo "FAIL: 贴图窗口不足 3 个"
    kill $APP_PID 2>/dev/null; kill $XVFB_PID 2>/dev/null
    exit 1
fi

geom() { xdotool getwindowgeometry --shell "$1" 2>/dev/null | tr '\n' ' '; }
pos() { xdotool getwindowgeometry --shell "$1" 2>/dev/null | grep '^X=\|^Y=' | cut -d= -f2 | tr '\n' ','; }

W1=${WINS[0]}; W2=${WINS[1]}; W3=${WINS[2]}
echo "W1=$W1  W2=$W2  W3=$W3"
P1_INIT=$(pos "$W1"); P2_INIT=$(pos "$W2"); P3_INIT=$(pos "$W3")
echo "初始位置: W1=$P1_INIT W2=$P2_INIT W3=$P3_INIT"

echo
echo "== 1. 在贴图 1 上按 Ctrl+A（全选）=="
# 点击贴图 1 使其获得焦点（点中心）
GEO=$(geom "$W1")
gx=$(echo "$GEO" | grep -oE 'X=[0-9]+' | cut -d= -f2)
gy=$(echo "$GEO" | grep -oE 'Y=[0-9]+' | cut -d= -f2)
gw=$(echo "$GEO" | grep -oE 'WIDTH=[0-9]+' | cut -d= -f2)
gh=$(echo "$GEO" | grep -oE 'HEIGHT=[0-9]+' | cut -d= -f2)
CX=$((gx + gw / 2)); CY=$((gy + gh / 2))
xdotool mousemove --sync "$CX" "$CY" sleep 0.2 click 1
sleep 0.4
xdotool key ctrl+a
sleep 0.6

echo "== 2. 拖动贴图 1（+120,+60）=="
xdotool mousemove --sync "$CX" "$CY" sleep 0.2
xdotool mousedown 1 sleep 0.2
for i in $(seq 1 6); do
    xdotool mousemove --sync $((CX + i * 20)) $((CY + i * 10)) sleep 0.08
done
xdotool mouseup 1
sleep 0.8

P1_AFTER=$(pos "$W1"); P2_AFTER=$(pos "$W2"); P3_AFTER=$(pos "$W3")
echo "拖动后位置: W1=$P1_AFTER W2=$P2_AFTER W3=$P3_AFTER"

echo
echo "== 3. 再按 Ctrl+A（取消全选）后拖动贴图 1（+80,+40）=="
# 贴图 1 已移动，重新取位置
GEO=$(geom "$W1")
gx=$(echo "$GEO" | grep -oE 'X=[0-9]+' | cut -d= -f2)
gy=$(echo "$GEO" | grep -oE 'Y=[0-9]+' | cut -d= -f2)
CX=$((gx + gw / 2)); CY=$((gy + gh / 2))
xdotool mousemove --sync "$CX" "$CY" sleep 0.2 click 1
sleep 0.3
xdotool key ctrl+a   # 取消全选
sleep 0.6
P2_BEFORE2=$(pos "$W2"); P3_BEFORE2=$(pos "$W3")

xdotool mousemove --sync "$CX" "$CY" sleep 0.2
xdotool mousedown 1 sleep 0.2
for i in $(seq 1 4); do
    xdotool mousemove --sync $((CX + i * 20)) $((CY + i * 10)) sleep 0.08
done
xdotool mouseup 1
sleep 0.8
P2_AFTER2=$(pos "$W2"); P3_AFTER2=$(pos "$W3")
echo "取消全选后拖动: W2 $P2_BEFORE2 -> $P2_AFTER2, W3 $P3_BEFORE2 -> $P3_AFTER2"

kill $APP_PID 2>/dev/null
sleep 0.3
kill $XVFB_PID 2>/dev/null

echo
echo "== 日志 =="
grep -E "全选|贴图创建" "$LOG" | tail -8

python3 - "$P1_INIT" "$P2_INIT" "$P3_INIT" \
         "$P1_AFTER" "$P2_AFTER" "$P3_AFTER" \
         "$P2_BEFORE2" "$P2_AFTER2" "$P3_BEFORE2" "$P3_AFTER2" <<'PY'
import re
import sys

(p1i, p2i, p3i, p1, p2, p3, p2b2, p2a2, p3b2, p3a2) = sys.argv[1:11]

def xy(s):
    x, y = s.strip().rstrip(',').split(',')
    return int(x), int(y)

log = open("/tmp/jie2-pin-select.log", encoding="utf-8").read()
assert re.search(r"贴图全选（3 张）", log), "FAIL: 全选日志缺失"
print("✓ 日志确认全选（3 张）")
assert re.search(r"贴图取消全选（3 张）", log), "FAIL: 取消全选日志缺失"
print("✓ 日志确认取消全选")

# ---- 断言 1：全选拖动 → W1/W2/W3 位移一致（严格同位移动）----
(x1i, y1i) = xy(p1i); (x2i, y2i) = xy(p2i); (x3i, y3i) = xy(p3i)
(x1, y1) = xy(p1);   (x2, y2) = xy(p2);   (x3, y3) = xy(p3)

d1 = (x1 - x1i, y1 - y1i)
d2 = (x2 - x2i, y2 - y2i)
d3 = (x3 - x3i, y3 - y3i)
print(f"全选拖动位移: W1={d1} W2={d2} W3={d3}")
if d1 != d2 or d2 != d3:
    print("FAIL: 全选拖动后三张位移不一致（未联动）")
    raise SystemExit(1)
if d1 == (0, 0):
    print("FAIL: W1 未移动（拖动未生效）")
    raise SystemExit(1)
print(f"✓ 三张同步位移 {d1}（全选联动）")

# ---- 断言 2：取消全选后拖动 → W2/W3 不动 ----
(x2b, y2b) = xy(p2b2); (x2a, y2a) = xy(p2a2)
(x3b, y3b) = xy(p3b2); (x3a, y3a) = xy(p3a2)
if (x2b, y2b) != (x2a, y2a):
    print(f"FAIL: 取消全选后 W2 不应移动：{p2b2} → {p2a2}")
    raise SystemExit(1)
if (x3b, y3b) != (x3a, y3a):
    print(f"FAIL: 取消全选后 W3 不应移动：{p3b2} → {p3a2}")
    raise SystemExit(1)
print("✓ 取消全选后拖动不再联动（W2/W3 未移动）")

print("PASS: 贴图全选与拖动联动验证通过")
PY
