#!/usr/bin/env bash
# layer-shell 层叠顺序验证：top layer 真的盖在普通窗口（xdg-toplevel）之上
#
# 做法：
#   1. sway headless 里先开一个全屏 magenta 的普通窗口
#      （用 jie2 的白板模式 —— 它是普通 xdg-toplevel，会铺满工作区）
#   2. 再起 jie2-line（top layer 的条带）
#   3. grim 截图：
#        - 顶部 150px 应是条带（深色 + 缩略图）而不是 magenta
#        - 下方应是 magenta（证明白板窗口确实在、且被压在条带之下）
#
# 这比"能渲染"更强：证明的是 layer 协议里 TOP 层的**堆叠语义**。
#
# 用法（容器内）：bash scripts/test-layer-shell-stacking.sh
set -u

TEST_USER=jie2test
RUNTIME_DIR=/tmp/xdg-jie2
SWAY_CONF=/tmp/sway-jie2.conf
ITEMS=/tmp/jie2-line-items.txt
INNER=/tmp/jie2-stacking-inner.sh
APP=/work/build-linux/src/ui/jie2
LINE=/work/build-linux/src/tools/jie2-line

pkill -f jie2-line 2>/dev/null
pkill -f "sway -c $SWAY_CONF" 2>/dev/null
pkill -f "whiteboard" 2>/dev/null
sleep 0.6

if ! id "$TEST_USER" >/dev/null 2>&1; then
    useradd -m -s /bin/bash "$TEST_USER"
fi
rm -rf "$RUNTIME_DIR"
mkdir -p "$RUNTIME_DIR"
chown "$TEST_USER" "$RUNTIME_DIR"
chmod 700 "$RUNTIME_DIR"

cat > "$SWAY_CONF" <<'EOF'
output HEADLESS-1 resolution 1280x800
EOF
chmod 644 "$SWAY_CONF"

python3 - <<'PY'
from PIL import Image
Image.new("RGB", (120, 80), (220, 40, 40)).save("/tmp/line-item-1.png")
Image.new("RGB", (80, 120), (40, 120, 220)).save("/tmp/line-item-2.png")
PY
cat > "$ITEMS" <<EOF
/tmp/line-item-1.png
/tmp/line-item-2.png
EOF
chmod 644 "$ITEMS" /tmp/line-item-*.png
rm -f /tmp/sway-jie2.log /tmp/sway-tree.json /tmp/jie2-line.log \
      /tmp/jie2-line-shot.png /tmp/grim.err /tmp/whiteboard.log

cat > "$INNER" <<'INNER_EOF'
#!/usr/bin/env bash
set -u
export XDG_RUNTIME_DIR=/tmp/xdg-jie2
export WLR_BACKENDS=headless
export WLR_LIBINPUT_NO_DEVICES=1

sway -c /tmp/sway-jie2.conf > /tmp/sway-jie2.log 2>&1 &
SWAY_PID=$!
for i in $(seq 1 60); do
    SOCK=$(ls "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null | head -1)
    [ -n "$SOCK" ] && break
    sleep 0.25
done
[ -n "${SOCK:-}" ] || { echo "NO-SOCKET"; exit 1; }
export WAYLAND_DISPLAY="$(basename "$SOCK")"
export SWAYSOCK="$(ls "$XDG_RUNTIME_DIR"/sway-ipc.* 2>/dev/null | head -1)"
sleep 0.6

# 1) 全屏普通窗口：白板模式（magenta）
QT_QPA_PLATFORM=wayland /work/build-linux/src/ui/jie2 --whiteboard "#ff00ff" \
    > /tmp/whiteboard.log 2>&1 &
WB_PID=$!
sleep 3.0

# 2) top layer 条带
/work/build-linux/src/tools/jie2-line --items /tmp/jie2-line-items.txt \
    --timeout-ms 5000 > /tmp/jie2-line.log 2>&1 &
LINE_PID=$!
sleep 2.0

# 3) 截图
grim /tmp/jie2-line-shot.png 2>/tmp/grim.err || echo "GRIM-FAILED"

kill $WB_PID 2>/dev/null
wait $LINE_PID 2>/dev/null
kill $SWAY_PID 2>/dev/null
sleep 0.4
exit 0
INNER_EOF
chmod 755 "$INNER"

echo "== 层叠场景启动（白板窗口 + top layer 条带）=="
runuser -u "$TEST_USER" -- bash "$INNER" || true

echo
echo "== 白板窗口日志（确认是普通窗口且已显示）=="
grep -E "白板|选区|QStandardPaths" /tmp/whiteboard.log 2>/dev/null | head -4
echo "   （无输出也正常：白板在等待用户框选）"

echo
echo "== 像素断言（层叠顺序）=="
python3 - <<'PY'
from PIL import Image
im = Image.open("/tmp/jie2-line-shot.png").convert("RGB")
w, h = im.size
print(f"   截图尺寸: {w}x{h}")

# 说明（实测标定）：条带底色是**半透明**的 (26,26,30,230)。
# 叠在黑色桌面背景上时呈现 (26,26,30) 附近；叠在被 dim 的白板
# (145,0,145) 上时呈现 (40,26,44) 附近。所以判据不能写死某一色值，
# 要用"与背景区域的对比 + 不透明缩略图的精确色"组合。

# ① 中部：白板内容可见（SelectionOverlay 把画面 dim 成 (145,0,145)）
mid = im.getpixel((w // 2, h // 2))
print(f"   中部 (640,400): {mid}")
assert mid == (145, 0, 145), f"FAIL: 白板窗口不可见或色值不符: {mid}"

# ② 顶部条带区：颜色应显著区别于白板区（半透明条带叠上去的证据）
strip = im.getpixel((700, 30))
print(f"   顶部条带区 (700,30): {strip}")
assert sum(strip) < sum(mid) - 100, \
    f"FAIL: 顶部区域与白板区无区别（条带没盖在上面？）: {strip}"

# ③ 缩略图（不透明）：精确色 —— 最强判据，证明条带内容在白板之上
thumb = im.getpixel((91, 74))
print(f"   条带内缩略图1: {thumb} ← 应为红(220,40,40)")
assert abs(thumb[0] - 220) < 20 and thumb[1] < 60, "FAIL: 缩略图不在顶层"

print("   ✓ top layer 覆盖在普通窗口之上（层叠语义验证通过）")
PY

python3 - <<'PY'
import re
log = open("/tmp/jie2-line.log", encoding="utf-8").read()
assert re.search(r"渲染 1280x150，2 张", log), "FAIL: 条带未渲染"
print("✓ 条带渲染日志正常")
print("PASS: layer-shell 层叠顺序验证通过")
PY
