#!/usr/bin/env bash
# 「J2」自定义应用内快捷键验证（专业版对齐项 v2.11.3）
#
# 场景：配置 shortcut.tool.text=x（文字工具从 T 改绑到 X）
#       + shortcut.style.dashed=none（禁用虚线快捷键）。
# 断言：
#   1. 按 X 触发文字工具（1 次）；
#   2. 按 T 不再触发（总数仍 1）；
#   3. 按 D 无任何"线型"反馈（已禁用）；
#   4. 未改动的默认绑定 E（橡皮擦）仍正常；
#   5. [ / Shift+] 仍工作（粗细 3→4、橡皮擦区域旋转到 30°）。
#
# 用法（容器内）：bash scripts/test-shortcuts-linux.sh
set -u

DISPLAY_NUM=:99
export DISPLAY=$DISPLAY_NUM
WORK=/work/build-linux
APP=$WORK/src/ui/jie2
LOG=/tmp/jie2-shortcuts.log
CFG_DIR=/root/.config/jie2

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

mkdir -p "$CFG_DIR"
cat > "$CFG_DIR/jie2.conf" <<EOF
shortcut.tool.text=x
shortcut.style.dashed=none
EOF
rm -f "$LOG"

"$APP" --select >"$LOG" 2>&1 &
APP_PID=$!
sleep 2.5

# 选区 (100,100)-(500,400)
xdotool mousemove --sync 100 100 sleep 0.15 mousedown 1 sleep 0.15
for i in 1 2 3; do
    xdotool mousemove --sync $((100 + i * 130)) $((100 + i * 100)) sleep 0.08
done
xdotool mouseup 1 sleep 0.6

# 断言辅助：expect_log <期望次数> <模式>
expect_log() {
    local want="$1" pat="$2" got
    got=$(grep -c "$pat" "$LOG" || true)
    if [ "$got" != "$want" ]; then
        echo "FAIL: 「$pat」出现 $got 次（期望 $want）"
        echo "---- 日志摘录 ----"
        grep -E "工具：|线型|粗细|旋转" "$LOG" || true
        exit 1
    fi
    echo "✓ 「$pat」次数=$got"
}

echo "== 1. 按 X（文字工具的新绑定）=="
xdotool key x
sleep 0.6
expect_log 1 "工具：文字"

echo "== 2. 按 T（旧绑定，应不再触发）=="
xdotool key t
sleep 0.6
expect_log 1 "工具：文字"

echo "== 3. 按 D（已禁用，应无反应）=="
xdotool key d
sleep 0.6
expect_log 0 "线型"

echo "== 4. 按 E（未改动的默认绑定，应正常）=="
xdotool key e
sleep 0.6
expect_log 1 "工具：橡皮擦"

echo "== 5. ] 加粗与 Shift+] 旋转橡皮擦区域 =="
xdotool key bracketright
sleep 0.4
expect_log 1 "粗细：4"
xdotool key shift+bracketright
sleep 0.4
xdotool key shift+bracketright
sleep 0.6
expect_log 1 "橡皮擦区域旋转：30°"

xdotool key Escape
sleep 0.8
kill $APP_PID 2>/dev/null
sleep 0.3
kill $XVFB_PID 2>/dev/null

echo
echo "PASS: 自定义应用内快捷键验证通过（X 改绑生效 / T 让位 / D 禁用 / E 默认保留 / [ ] 正常）"
