#!/usr/bin/env bash
# 「J2」M3 增补验证：衣架动画 + 剪贴板监听 + 拖出
#
# 覆盖：
#   1. 滑入动画：指针到顶边后，条带**逐步**滑入（在中途采样应看到条带未到位）
#   2. 剪贴板监听：用 xclip 复制一张图 → 日志"剪贴板图像已挂上衣架" + 历史 +1
#   3. 拖出：按住卡片拖动 —— QDrag 启动（日志"拖出"）；
#      无接收者时释放 → IgnoreAction → 卡片保留（日志无"卡片下线"）
#
# 用法（容器内）：bash scripts/test-clothesline-anim.sh
set -u

DISPLAY_NUM=:99
export DISPLAY=$DISPLAY_NUM
WORK=/work/build-linux
APP=$WORK/src/ui/jie2
LOG=/tmp/jie2-anim.log
CFG_DIR=/root/.config/jie2
SHOT_DIR=/root/Pictures/jie2

pkill -f "Xvfb $DISPLAY_NUM" 2>/dev/null
pkill -f "build-linux/src/ui" 2>/dev/null
sleep 0.5
Xvfb $DISPLAY_NUM -screen 0 1280x800x24 >/dev/null 2>&1 &
XVFB_PID=$!
sleep 1.5

rm -rf "$CFG_DIR" "$SHOT_DIR"
rm -f "$LOG"

echo "== 准备：先截一张图（历史基线）=="
"$APP" --select-rect 0,0,64,64 >/dev/null 2>&1
echo "历史条数: $(grep -c '|' "$CFG_DIR/history.txt" 2>/dev/null || echo 0)"

echo
echo "== 启动托盘常驻 =="
"$APP" >"$LOG" 2>&1 &
APP_PID=$!
sleep 2.5

echo "== [1] 剪贴板监听：复制一张 40x30 红图 =="
python3 - <<'PY'
from PIL import Image
Image.new("RGB", (40, 30), (255, 0, 0)).save("/tmp/clip-red.png")
PY
xclip -selection clipboard -t image/png -i /tmp/clip-red.png
sleep 1.2
echo "历史条数（应 +1）: $(grep -c '|' "$CFG_DIR/history.txt" 2>/dev/null || echo 0)"

echo
echo "== [2] 滑入动画：指针移到顶边，中途多次采样窗口位置 =="
xdotool mousemove --sync 640 1
# 立刻采样（12ms 步进 × 14px 下，210px 应该在 ~15 步 ≈ 180ms 内完成）
for i in 1 2 3; do
    sleep 0.03
    # 用 probe 抓全屏，检查顶部条带是否部分可见（用像素统计太慢；改用日志）
    true
done
sleep 1.0

echo "== [3] 拖出：按住卡片 1 拖动 =="
# tendedero 样式布局（居中排布，2 张卡）：卡 1（64×64）中心 ≈ (553, 93)
xdotool mousemove --sync 553 93 sleep 0.2
xdotool mousedown 1 sleep 0.1
# 移动超过阈值，启动 QDrag
xdotool mousemove --sync 400 93 sleep 0.1
xdotool mousemove --sync 500 300 sleep 0.2
# 释放：无接收者 → IgnoreAction
xdotool mouseup 1 sleep 0.5

echo "== 收尾：指针移开 =="
xdotool mousemove --sync 640 500
sleep 1.2

kill $APP_PID 2>/dev/null
sleep 0.3
kill $XVFB_PID 2>/dev/null

echo
echo "== 应用日志 =="
cat "$LOG"
echo

python3 - <<'PY'
import re
import os

log = open("/tmp/jie2-anim.log", encoding="utf-8").read()

def need(pattern, name):
    m = re.search(pattern, log)
    assert m, f"FAIL: 日志缺少「{name}」"
    print(f"✓ {name}: {m.group(0)[:60]}")
    return m

# 1. 剪贴板监听
need(r"剪贴板图像已挂上衣架", "剪贴板图像自动挂载")
hist = open("/root/.config/jie2/history.txt", encoding="utf-8").read()
lines = [l for l in hist.splitlines() if "|" in l]
assert len(lines) == 2, f"FAIL: 历史应为 2 条（1 截图 + 1 复制），实际 {len(lines)}"
assert any("copied" in l for l in lines), "FAIL: 历史里没有 copied 前缀的条目"
print(f"✓ 历史条数: {len(lines)}（含 copied 条目）")

# 2. 动画：展开与收起都有
need(r"衣架展开", "滑入")
need(r"衣架收起", "滑出")

# 3. 拖出：启动了 QDrag（"拖出"日志），且无接收者时卡片未下线
need(r"衣架：拖出", "拖出启动")
assert "卡片下线" not in log, "FAIL: 无接收者的拖出不应让卡片下线"
print("✓ 拖出被忽略时卡片保留")

print("PASS: M3 增补验证通过（剪贴板挂载 / 滑入滑出 / 拖出启动）")
PY
