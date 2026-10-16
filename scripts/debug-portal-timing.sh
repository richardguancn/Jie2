#!/usr/bin/env bash
# Wayland portal 时序调试：就绪探测 + 循环复现 + 失败现场保留
set -u
LOOPS="${1:-12}"
inner='
    MOCK_TRIGGER_DELAY_MS=1500 python3 /work/scripts/mock-portal.py > /tmp/fmp.log 2>&1 &
    MOCK_PID=$!
    # 等 mock 真正持有总线名字（固定 sleep 会偶发"名字未就绪"的假失败）
    for i in $(seq 1 40); do
      if dbus-send --session --print-reply --dest=org.freedesktop.DBus \
           /org/freedesktop/DBus org.freedesktop.DBus.NameHasOwner \
           string:org.freedesktop.portal.Desktop 2>/dev/null | grep -q "true"; then
        break
      fi
      sleep 0.1
    done
    export XDG_SESSION_TYPE=wayland WAYLAND_DISPLAY=wayland-0 JIE2_PORTAL_DEBUG=1
    /work/build-linux/src/tools/jie2-probe --test-hotkey --wait-activation-ms 5000 > /tmp/fp.log 2>&1
    rc=$?
    sleep 0.3
    kill $MOCK_PID 2>/dev/null
    exit $rc
'
for i in $(seq 1 "$LOOPS"); do
  dbus-run-session -- bash -c "$inner"
  rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "FAIL at run $i"
    cp /tmp/fp.log /tmp/fail-fp.log
    cp /tmp/fmp.log /tmp/fail-fmp.log
    break
  fi
  echo "run $i PASS"
done

echo "=== 失败现场（若有）：probe stderr 时序 ==="
grep -E "portal-debug|hotkey-activated" /tmp/fail-fp.log 2>/dev/null | tail -16 || echo "(无失败样本)"
echo "=== 失败现场：probe stdout 尾部 ==="
tail -3 /tmp/fail-fp.log 2>/dev/null
echo "=== 失败现场：mock ==="
cat /tmp/fail-fmp.log 2>/dev/null | head -8
exit 0
