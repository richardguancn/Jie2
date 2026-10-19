#!/usr/bin/env bash
# 「J2」橡皮擦增强验证（专业版对齐项 v2.6）
#
# 场景 A（默认：擦除可撤销 + 区域可旋转）：
#   画矩形 → E 切橡皮擦 → Shift+] 旋转区域 30° → 擦掉矩形 → Ctrl+Z 恢复
#   → 保存 → 输出图中红色像素回来了（矩形被撤销恢复）。
# 场景 B（annotation.eraser_disable_reedit=true：擦除不可恢复）：
#   同样流程，Ctrl+Z 之后保存 → 输出图中红色像素仍为 0（A 不复活）。
#
# 用法（容器内）：bash scripts/test-eraser-tools-linux.sh
set -u

DISPLAY_NUM=:99
export DISPLAY=$DISPLAY_NUM
WORK=/work/build-linux
APP=$WORK/src/ui/jie2
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

select_and_draw_rect() { # 选区 + 画红色矩形 (130,130)-(160,160)
    xdotool mousemove --sync 100 100 sleep 0.15 mousedown 1 sleep 0.15
    for i in 1 2 3 4; do
        xdotool mousemove --sync $((100 + i * 100)) $((100 + i * 75)) sleep 0.08
    done
    xdotool mouseup 1 sleep 0.6
    # 默认工具即矩形；拖出小矩形
    xdotool mousemove --sync 130 130 sleep 0.15 mousedown 1 sleep 0.15
    for i in 1 2 3 4; do
        xdotool mousemove --sync $((130 + i * 7)) $((130 + i * 7)) sleep 0.08
    done
    xdotool mouseup 1 sleep 0.5
}

erase_rect() { # E 切换橡皮擦 + 拖过小矩形
    xdotool key e
    sleep 0.5
    xdotool mousemove --sync 133 133 sleep 0.15 mousedown 1 sleep 0.15
    for i in 1 2 3 4 5; do
        xdotool mousemove --sync $((133 + i * 5)) $((133 + i * 5)) sleep 0.08
    done
    xdotool mouseup 1 sleep 0.5
}

check_saved_red() { # $1 下限 $2 说明
    local min="$1" why="$2"
    LATEST=$(ls -t "$SHOT_DIR"/*.png 2>/dev/null | head -1)
    if [ -z "$LATEST" ]; then
        echo "FAIL: 没有保存任何截图（$why）"
        exit 1
    fi
    python3 - "$LATEST" "$min" "$why" <<'PY'
import sys
from PIL import Image
path, min_hits, why = sys.argv[1], int(sys.argv[2]), sys.argv[3]
im = Image.open(path).convert("RGB")
# 选区 (100,100)-(500,400) → 小矩形相对坐标 (30,30)-(60,60)
hits = sum(1 for x in range(26, 66) for y in range(26, 66)
           if im.getpixel((x, y)) == (239, 68, 68))
print(f"{why}: 红色像素 {hits}（下限 {min_hits}）")
if min_hits > 0 and hits < min_hits:
    print("FAIL: 红色像素不足")
    raise SystemExit(1)
if min_hits == 0 and hits >= 12:
    print("FAIL: 期望被永久擦除，但红色像素仍然存在")
    raise SystemExit(1)
PY
}

# ---------------------------------------------------------------------------
echo "========== 场景 A：默认（擦除可撤销）=========="
rm -rf "$CFG_DIR" "$SHOT_DIR"
mkdir -p "$CFG_DIR"
: > "$CFG_DIR/jie2.conf"
LOG=/tmp/jie2-eraser-a.log
rm -f "$LOG"

"$APP" --select >"$LOG" 2>&1 &
APP_PID=$!
sleep 2.5

select_and_draw_rect

echo "== 旋转橡皮擦区域到 30° =="
xdotool key shift+bracketright
sleep 0.3
xdotool key shift+bracketright
sleep 0.4

erase_rect

echo "== Ctrl+Z 撤销擦除 / Ctrl+Shift+Z 重做 / 再 Ctrl+Z =="
xdotool key ctrl+z
sleep 0.5
xdotool key ctrl+shift+z
sleep 0.5
xdotool key ctrl+z
sleep 0.5

echo "== 回车保存 =="
xdotool key Return
sleep 1.0
kill $APP_PID 2>/dev/null
sleep 0.3

echo "---- 场景 A 日志断言 ----"
grep -E "橡皮擦|撤销|重做|旋转" "$LOG" || true
if [ "$(grep -c "橡皮擦删除 1 条" "$LOG" || true)" != "1" ]; then
    echo "FAIL: 场景 A 擦除日志不符（期望 1 次「橡皮擦删除 1 条」）"; exit 1
fi
if [ "$(grep -c "橡皮擦区域旋转：30°" "$LOG" || true)" != "1" ]; then
    echo "FAIL: 场景 A 未出现「橡皮擦区域旋转：30°」"; exit 1
fi
if [ "$(grep -c "撤销（剩 1 条）" "$LOG" || true)" != "2" ]; then
    echo "FAIL: 场景 A 撤销日志不符（期望 2 次「撤销（剩 1 条）」）"; exit 1
fi
if [ "$(grep -c "重做（共 0 条）" "$LOG" || true)" != "1" ]; then
    echo "FAIL: 场景 A 重做日志不符（期望 1 次「重做（共 0 条）」）"; exit 1
fi
echo "✓ 场景 A 日志断言通过"
check_saved_red 20 "场景 A（撤销后应恢复）"

# ---------------------------------------------------------------------------
echo
echo "========== 场景 B：擦除不可恢复（禁用二次编辑）=========="
rm -rf "$CFG_DIR" "$SHOT_DIR"
mkdir -p "$CFG_DIR"
cat > "$CFG_DIR/jie2.conf" <<EOF
annotation.eraser_disable_reedit=true
EOF
LOG=/tmp/jie2-eraser-b.log
rm -f "$LOG"

"$APP" --select >"$LOG" 2>&1 &
APP_PID=$!
sleep 2.5

select_and_draw_rect
erase_rect

echo "== Ctrl+Z（不应恢复）=="
xdotool key ctrl+z
sleep 0.5

echo "== 回车保存 =="
xdotool key Return
sleep 1.0
kill $APP_PID 2>/dev/null
sleep 0.3

echo "---- 场景 B 日志断言 ----"
grep -E "橡皮擦|撤销|重做" "$LOG" || true
if [ "$(grep -c "橡皮擦删除 1 条（剩 0 条）" "$LOG" || true)" != "1" ]; then
    echo "FAIL: 场景 B 擦除日志不符"; exit 1
fi
if [ "$(grep -c "撤销（剩 0 条）" "$LOG" || true)" != "1" ]; then
    echo "FAIL: 场景 B 撤销日志不符（期望「撤销（剩 0 条）」——A 不会被撤销恢复）"; exit 1
fi
if [ "$(grep -c "撤销（剩 1 条）" "$LOG" || true)" != "0" ]; then
    echo "FAIL: 场景 B 出现了「撤销（剩 1 条）」——擦除被恢复了，禁用二次编辑失效"; exit 1
fi
echo "✓ 场景 B 日志断言通过"
check_saved_red 0 "场景 B（永久擦除后不应有红像素）"

kill $XVFB_PID 2>/dev/null

echo
echo "PASS: 橡皮擦增强验证通过（区域可旋转 / 擦除可撤销 / 可禁用二次编辑）"
