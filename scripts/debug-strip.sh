#!/usr/bin/env bash
# 调试：衣架条带像素检查
set -u
export DISPLAY=:99
pkill -f "Xvfb :99" 2>/dev/null
sleep 0.5
Xvfb :99 -screen 0 1280x800x24 >/dev/null 2>&1 &
XVFB=$!
sleep 1.5

/work/build-linux/src/ui/jie2 >/tmp/dbg.log 2>&1 &
APP=$!
sleep 2.5

xdotool mousemove --sync 640 1
sleep 1.0

/work/build-linux/src/tools/jie2-probe --capture-save /tmp/dbg.ppm >/tmp/probe.log 2>&1
echo "probe rc=$?"
grep -E "capture:|saved:" /tmp/probe.log || true

sleep 0.3
kill $APP 2>/dev/null
kill $XVFB 2>/dev/null

echo "--- app log ---"
tail -6 /tmp/dbg.log

python3 - <<'PYEOF'
data = open("/tmp/dbg.ppm", "rb").read()
parts = data.split(b"\n", 3)
w, h = map(int, parts[1].split())
px = parts[3]
print("尺寸:", w, h, " 字节:", len(px), " 期望:", w*h*3)
for y in [2, 10, 30, 60, 100, 140, 200]:
    row = []
    for x in [20, 200, 640, 1000]:
        i = (y * w + x) * 3
        row.append((px[i], px[i+1], px[i+2]))
    print(f"y={y}: {row}")
PYEOF
exit 0
