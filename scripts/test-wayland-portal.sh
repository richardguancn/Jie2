#!/usr/bin/env bash
# 「J2」Wayland GlobalShortcuts portal 端到端验证
#
# 场景（在 dbus-run-session 提供的私有会话总线上）：
#   1. 启动 mock 门户（scripts/mock-portal.py），它按协议实现
#      CreateSession / BindShortcuts / Session.Close / Activated；
#   2. 用环境变量模拟 Wayland 会话（XDG_SESSION_TYPE=wayland 等）；
#   3. 跑 jie2-probe --test-hotkey --wait-activation-ms：
#      - 断言能力探测显示 hotkey "available"（真实查询了门户服务）；
#      - 断言绑定成功（CreateSession → BindShortcuts → Response(0)）；
#      - 断言 mock 延迟发出的 Activated 触发了回调；
#   4. 追加"拒绝"场景：MOCK_RESPONSE_CODE=2 时绑定必须**如实失败**，
#      不得静默成功。
#
# 用法（容器内）：bash scripts/test-wayland-portal.sh
set -u

PROBE=/work/build-linux/src/tools/jie2-probe
MOCK=/work/scripts/mock-portal.py
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

echo "################ 场景 A：正常绑定 + 激活 ################"
dbus-run-session -- bash -c '
    MOCK_TRIGGER_DELAY_MS=1500 python3 '"$MOCK"' > /tmp/mock-portal.log 2>&1 &
    MOCK_PID=$!
    '"$(declare -f wait_for_mock)"'
    wait_for_mock

    export XDG_SESSION_TYPE=wayland
    export WAYLAND_DISPLAY=wayland-0
    export XDG_CURRENT_DESKTOP=KDE

    '"$PROBE"' --test-hotkey --wait-activation-ms 6000 > /tmp/portal-probe.log 2>&1
    echo "probe rc=$?" > /tmp/portal-rc.txt

    sleep 0.3
    kill $MOCK_PID 2>/dev/null
    exit 0
'

echo "--- probe 输出 ---"
cat /tmp/portal-probe.log
cat /tmp/portal-rc.txt
echo "--- mock 门户日志 ---"
cat /tmp/mock-portal.log
echo

echo "断言："
check "能力探测识别 Wayland 会话" "session: Wayland" /tmp/portal-probe.log
check "热键能力为 available（真实查询了门户）" "global-hotkey : available" /tmp/portal-probe.log
check "绑定成功" "hotkey Ctrl+Alt+T: ok" /tmp/portal-probe.log
check "Activated 触发回调" "hotkey-activated: probe" /tmp/portal-probe.log
check "等待激活成功" "wait-activation: fired" /tmp/portal-probe.log
check "门户收到正确的触发串" "BindShortcuts id=probe trigger=CTRL+ALT+t" /tmp/mock-portal.log
check "会话被关闭（Close 调用）" "session closed" /tmp/mock-portal.log

echo
echo "################ 场景 B：门户拒绝绑定（必须如实失败）################"
dbus-run-session -- bash -c '
    MOCK_TRIGGER_DELAY_MS=0 MOCK_RESPONSE_CODE=2 python3 '"$MOCK"' > /tmp/mock-portal2.log 2>&1 &
    MOCK_PID=$!
    '"$(declare -f wait_for_mock)"'
    wait_for_mock

    export XDG_SESSION_TYPE=wayland
    export WAYLAND_DISPLAY=wayland-0

    '"$PROBE"' --test-hotkey > /tmp/portal-probe2.log 2>&1
    echo "probe rc=$?" > /tmp/portal-rc2.txt

    sleep 0.3
    kill $MOCK_PID 2>/dev/null
    exit 0
'

echo "--- probe 输出 ---"
cat /tmp/portal-probe2.log
cat /tmp/portal-rc2.txt
echo

echo "断言："
check "绑定如实失败" "hotkey Ctrl+Alt+T: failed" /tmp/portal-probe2.log
check "失败原因里带 response 码" "response=2" /tmp/portal-probe2.log

echo
if [ "$FAILURES" -eq 0 ]; then
    echo "PASS: Wayland GlobalShortcuts portal 端到端验证通过（绑定/激活/拒绝三态）"
    exit 0
else
    echo "FAIL: $FAILURES 项断言未通过"
    exit 1
fi
