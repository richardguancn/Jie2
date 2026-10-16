#!/usr/bin/env bash
# 调试层叠：打印截图关键区域的真实像素
set -u
TEST_USER=jie2test
RUNTIME_DIR=/tmp/xdg-jie2
INNER=/tmp/jie2-stacking-debug.sh
APP=/work/build-linux/src/ui/jie2

pkill -f jie2-line 2>/dev/null
pkill -f "sway -c" 2>/dev/null
pkill -f "whiteboard" 2>/dev/null
sleep 0.6

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
export WAYLAND_DISPLAY="$(basename "$SOCK")"
export SWAYSOCK="$(ls "$XDG_RUNTIME_DIR"/sway-ipc.* 2>/dev/null | head -1)"
sleep 0.6

echo "--- 只有白板窗口时截图 ---"
QT_QPA_PLATFORM=wayland /work/build-linux/src/ui/jie2 --whiteboard "#ff00ff" \
    > /tmp/whiteboard.log 2>&1 &
WB_PID=$!
sleep 3.0
grim /tmp/dbg-wb-only.png 2>/dev/null
echo "--- 再起 jie2-line，等 2.5s 截图 ---"
/work/build-linux/src/tools/jie2-line --items /tmp/jie2-line-items.txt \
    --timeout-ms 5000 > /tmp/jie2-line.log 2>&1 &
LINE_PID=$!
sleep 2.5
grim /tmp/dbg-with-line.png 2>/dev/null

# sway 树信息
swaymsg -t get_tree > /tmp/dbg-tree.json 2>/dev/null

kill $WB_PID 2>/dev/null
wait $LINE_PID 2>/dev/null
kill $SWAY_PID 2>/dev/null
sleep 0.3
exit 0
INNER_EOF
chmod 755 "$INNER"
runuser -u "$TEST_USER" -- bash "$INNER" || true

echo
echo "=== 白板窗口日志 ==="
cat /tmp/whiteboard.log 2>/dev/null | head -6

echo
echo "=== 调试像素 ==="
python3 - <<'PY'
from PIL import Image

for name in ("/tmp/dbg-wb-only.png", "/tmp/dbg-with-line.png"):
    try:
        im = Image.open(name).convert("RGB")
    except Exception as e:
        print(name, "打不开:", e)
        continue
    w, h = im.size
    print(f"\n{name} ({w}x{h})")
    for label, (x, y) in {
        "顶部 (700,30)": (700, 30),
        "条带中段 (700,74)": (700, 74),
        "条带底部 (700,140)": (700, 140),
        "中部 (640,400)": (640, 400),
        "底部 (640,760)": (640, 760),
        "缩略图1 (91,74)": (91, 74),
    }.items():
        print(f"  {label}: {im.getpixel((x, y))}")
PY

echo
echo "=== get_tree 里的 toplevel（白板）==="
python3 - <<'PY'
import json
try:
    tree = json.load(open("/tmp/dbg-tree.json"))
    def walk(n, depth=0):
        if isinstance(n, dict):
            t = n.get("type") or ""
            if t in ("con", "floating_con"):
                print("  " * depth + f"{t}: name={n.get('name')!r} app_id={n.get('app_id')!r} rect={n.get('rect')}")
            for k in ("nodes", "floating_nodes"):
                for c in n.get(k) or []:
                    walk(c, depth + 1)
        elif isinstance(n, list):
            for i in n:
                walk(i, depth)
    walk(tree)
except Exception as e:
    print("tree 解析失败:", e)
PY
