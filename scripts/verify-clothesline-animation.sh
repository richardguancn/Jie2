#!/usr/bin/env bash
# 验证衣架"滑入"是**逐步动画**而不是瞬间跳变：
# 触发展开的同时高频采样窗口 y 坐标，检查采样序列中出现中间位置。
set -u
export DISPLAY=:99
WORK=/work/build-linux
APP=$WORK/src/ui/jie2

pkill -f "Xvfb :99" 2>/dev/null
pkill -f "build-linux/src/ui" 2>/dev/null
sleep 0.5
rm -f /tmp/.X99-lock /tmp/.X11-unix/X99
Xvfb :99 -screen 0 1280x800x24 >/dev/null 2>&1 &
XVFB=$!
# 等 X socket 就绪（盲等固定时长在高负载下会竞态 —— 实测教训）。
for i in $(seq 1 25); do
    [ -S /tmp/.X11-unix/X99 ] && break
    sleep 0.2
done
sleep 0.8

rm -rf /root/.config/jie2 /root/Pictures/jie2
"$APP" --select-rect 0,0,48,48 >/dev/null 2>&1   # 一条历史，卡片区就绪

"$APP" >/tmp/anim2.log 2>&1 &
APP_PID=$!
sleep 2.5

# 先在顶边停留让衣架展开一次（建立窗口），再移开等它完全收起。
xdotool mousemove --sync 640 1
sleep 1.2
xdotool mousemove --sync 640 400
sleep 1.2   # 收起动画完成

# 采样器：高频轮询衣架窗口 y。
#
# 两个实测要点：
#   1. Qt 会把窗口标题写成 "截2 衣架 — 截2"（自动追加应用显示名），
#      所以按名字查找必须用"包含"而不是精确相等；
#   2. 窗口在收起状态是 unmapped 的，xdotool search 默认仍能列出未映射窗口，
#      但为稳妥起见遍历全部窗口再按标题过滤。
find_line() {
    for id in $(xdotool search --name ".*" 2>/dev/null); do
        name=$(xdotool getwindowname "$id" 2>/dev/null || true)
        case "$name" in
            *衣架*)
                echo "$id"
                return
                ;;
        esac
    done
}

WIN_ID="$(find_line)"
echo "衣架窗口 id: ${WIN_ID:-（未找到）}"

sample_y() {
    local id="$1"
    xdotool getwindowgeometry --shell "$id" 2>/dev/null \
        | grep '^Y=' | cut -d= -f2 || echo "NA"
}

# 启动采样器（后台持续约 1s，每 ~8ms 一次）
if [ -n "${WIN_ID}" ]; then
    ( for i in $(seq 1 120); do sample_y "$WIN_ID"; done > /tmp/y-samples.txt 2>&1 ) &
    SAMPLER=$!
    sleep 0.05
else
    echo "FAIL: 未找到衣架窗口"
    kill $APP_PID 2>/dev/null
    kill $XVFB 2>/dev/null
    exit 1
fi

# 触发展开（把指针甩到顶边）
xdotool mousemove --sync 640 1
wait $SAMPLER 2>/dev/null

kill $APP_PID 2>/dev/null
sleep 0.2
kill $XVFB 2>/dev/null

echo "== 采样到的 y 序列（升序去重前 20 个）=="
sort -n /tmp/y-samples.txt | uniq | head -20
echo

python3 - <<'PY'
vals = []
for line in open("/tmp/y-samples.txt"):
    line = line.strip()
    if line and line != "NA":
        try:
            vals.append(int(line))
        except ValueError:
            pass
uniq = sorted(set(vals))
print(f"采样点数: {len(vals)}，去重后: {len(uniq)}")
if len(uniq) < 2:
    print("FAIL: 没有观察到动画中间位置（可能瞬间跳变或窗口未找到）")
    raise SystemExit(1)
# 展开动画：从屏幕上方（负 y）滑到 0；中间应有多个负 y 位置。
negatives = [v for v in uniq if v < 0]
print(f"负 y（屏幕上方）采样: {negatives}")
if len(negatives) < 2:
    print("FAIL: 中间位置采样不足，无法证明是逐步动画")
    raise SystemExit(1)
print("PASS: 观察到滑动过程中的多个中间位置 → 是逐步动画而非跳变")
PY
