#!/usr/bin/env bash
# 「J2」Wayland 截图门户（Screenshot）端到端验证
#
# 场景（dbus-run-session 私有总线 + mock 门户）：
#   A. 全屏截图：门户返回 64x48 PNG（5 种行滤波轮转）→
#      probe --capture 抓取 → 尺寸与像素逐点核对；
#   B. 区域裁剪：--capture-region 10,5,20,15 → 20x15，首像素 = 原图 (10,5)；
#   C. 越界区域：--capture-region 500,500,20,20 → 必须如实失败；
#   D. 门户拒绝：MOCK_RESPONSE_CODE=1（用户取消）→ 必须如实失败。
#
# 用法（容器内）：bash scripts/test-wayland-capture.sh
set -u

PROBE=/work/build-linux/src/tools/jie2-probe
MOCK=/work/scripts/mock-portal.py
MAKEPNG=/work/scripts/make-test-png.py
FAILURES=0

check() { # 描述 期望子串 文件
    if grep -q "$2" "$3"; then
        echo "  ✓ $1"
    else
        echo "  ✗ $1（在 $3 中找不到「$2」）"
        FAILURES=$((FAILURES + 1))
    fi
}

# 等 mock 真正持有总线名字（固定 sleep 会偶发"名字未就绪"的假失败）。
wait_for_mock() {
    for _ in $(seq 1 40); do
        if dbus-send --session --print-reply --dest=org.freedesktop.DBus \
             /org/freedesktop/DBus org.freedesktop.DBus.NameHasOwner \
             string:org.freedesktop.portal.Desktop 2>/dev/null | grep -q "true"; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

# 校验 PPM：参数 <ppm> <期望宽> <期望高> [点 x y 期望r 期望g 期望b]...
verify_ppm() {
    python3 - "$@" <<'PYEOF'
import sys

path, w, h = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
with open(path, "rb") as fh:
    data = fh.read()
# P6 头：P6\n<w> <h>\n255\n（解析以空白分隔）
parts = data.split(b"\n", 3)
assert parts[0] == b"P6", "不是 P6 PPM"
assert parts[1] == b"%d %d" % (w, h), "尺寸不符: %r" % parts[1]
assert parts[2] == b"255", "位深头不符"
pixels = parts[3]

def px(x, y):
    i = (y * w + x) * 3
    return tuple(pixels[i:i+3])

point_args = sys.argv[4:]
ok = True
# 每个采样点 5 个整数：x y r g b
for i in range(0, len(point_args), 5):
    x, y, r, g, b = map(int, point_args[i:i+5])
    got = px(x, y)
    if got != (r, g, b):
        print("  ✗ 像素 (%d,%d) 期望 %s 实际 %s" % (x, y, (r, g, b), got))
        ok = False
    else:
        print("  ✓ 像素 (%d,%d) = %s" % (x, y, got))
sys.exit(0 if ok else 1)
PYEOF
}

echo "################ 场景 A：全屏截图 + 像素核对 ################"
dbus-run-session -- bash -c '
    python3 '"$MAKEPNG"' /tmp/portal-shot.png 64 48 > /tmp/cap-makepng.log
    MOCK_SCREENSHOT_URI=file:///tmp/portal-shot.png python3 '"$MOCK"' > /tmp/cap-mock-a.log 2>&1 &
    MOCK_PID=$!
    '"$(declare -f wait_for_mock)"'
    wait_for_mock
    export XDG_SESSION_TYPE=wayland WAYLAND_DISPLAY=wayland-0
    '"$PROBE"' --capture --capture-save /tmp/cap-out-a.ppm > /tmp/cap-probe-a.log 2>&1
    sleep 0.3
    kill $MOCK_PID 2>/dev/null
    exit 0
'

echo "--- probe 输出（节选）---"
grep -E "session|screen-capture|global-hotkey|capture:|saved:" /tmp/cap-probe-a.log
echo "--- mock 日志 ---"
cat /tmp/cap-mock-a.log
echo "断言："
check "能力探测：抓屏为 degraded（门户可用）" "screen-capture: degraded" /tmp/cap-probe-a.log
check "抓取成功且尺寸正确" "capture: ok 64x48 rgba" /tmp/cap-probe-a.log
check "门户收到非交互 Screenshot 调用" "Screenshot interactive=False" /tmp/cap-mock-a.log
# 像素核对：左上 (0,0)=(0,0,128)；右上 (63,0)=(630%256=118,0,128)；
# 中心 (32,24)=(320%256=64,240,128)；(10,5)=(100,50,128)
verify_ppm /tmp/cap-out-a.ppm 64 48 0 0 0 0 128 63 0 118 0 128 32 24 64 240 128 10 5 100 50 128
if [ $? -ne 0 ]; then FAILURES=$((FAILURES + 1)); fi

echo
echo "################ 场景 B：区域裁剪 ################"
dbus-run-session -- bash -c '
    python3 '"$MAKEPNG"' /tmp/portal-shot.png 64 48 > /dev/null
    MOCK_SCREENSHOT_URI=file:///tmp/portal-shot.png python3 '"$MOCK"' > /tmp/cap-mock-b.log 2>&1 &
    MOCK_PID=$!
    '"$(declare -f wait_for_mock)"'
    wait_for_mock
    export XDG_SESSION_TYPE=wayland WAYLAND_DISPLAY=wayland-0
    '"$PROBE"' --capture --capture-region 10,5,20,15 --capture-save /tmp/cap-out-b.ppm > /tmp/cap-probe-b.log 2>&1
    sleep 0.3
    kill $MOCK_PID 2>/dev/null
    exit 0
'

echo "--- probe 输出（节选）---"
grep -E "capture:|saved:" /tmp/cap-probe-b.log
echo "断言："
check "裁剪尺寸正确" "capture: ok 20x15 rgba" /tmp/cap-probe-b.log
# 裁剪后 (0,0) = 原图 (10,5) = (100,50,128)；(5,3) = 原图 (15,8) = (150,80,128)
verify_ppm /tmp/cap-out-b.ppm 20 15 0 0 100 50 128 5 3 150 80 128
if [ $? -ne 0 ]; then FAILURES=$((FAILURES + 1)); fi

echo
echo "################ 场景 C：越界区域必须如实失败 ################"
dbus-run-session -- bash -c '
    python3 '"$MAKEPNG"' /tmp/portal-shot.png 64 48 > /dev/null
    MOCK_SCREENSHOT_URI=file:///tmp/portal-shot.png python3 '"$MOCK"' > /tmp/cap-mock-c.log 2>&1 &
    MOCK_PID=$!
    '"$(declare -f wait_for_mock)"'
    wait_for_mock
    export XDG_SESSION_TYPE=wayland WAYLAND_DISPLAY=wayland-0
    '"$PROBE"' --capture --capture-region 500,500,20,20 > /tmp/cap-probe-c.log 2>&1
    sleep 0.3
    kill $MOCK_PID 2>/dev/null
    exit 0
'
grep "capture:" /tmp/cap-probe-c.log
check "越界如实失败并说明" "超出门户截图范围" /tmp/cap-probe-c.log

echo
echo "################ 场景 D：用户取消（response=1）必须如实失败 ################"
dbus-run-session -- bash -c '
    python3 '"$MAKEPNG"' /tmp/portal-shot.png 64 48 > /dev/null
    MOCK_RESPONSE_CODE=1 MOCK_SCREENSHOT_URI=file:///tmp/portal-shot.png \
        python3 '"$MOCK"' > /tmp/cap-mock-d.log 2>&1 &
    MOCK_PID=$!
    '"$(declare -f wait_for_mock)"'
    wait_for_mock
    export XDG_SESSION_TYPE=wayland WAYLAND_DISPLAY=wayland-0
    '"$PROBE"' --capture > /tmp/cap-probe-d.log 2>&1
    sleep 0.3
    kill $MOCK_PID 2>/dev/null
    exit 0
'
grep "capture:" /tmp/cap-probe-d.log
check "取消场景如实失败" "response=1" /tmp/cap-probe-d.log

echo
if [ "$FAILURES" -eq 0 ]; then
    echo "PASS: Wayland 截图门户端到端验证通过（全屏/裁剪/越界/取消）"
    exit 0
else
    echo "FAIL: $FAILURES 项断言未通过"
    exit 1
fi
