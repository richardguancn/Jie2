#!/usr/bin/env bash
# --show-group-manager 验证（容器内）：弹窗出现 + 可关闭 + 主实例存活
set -u
export DISPLAY=:99
WORK=/work/build-linux
APP=$WORK/src/ui/jie2
LOG=/tmp/jie2-gm.log

pkill -f "Xvfb :99" 2>/dev/null
pkill -f "build-linux/src/ui" 2>/dev/null
sleep 0.5
Xvfb :99 -screen 0 1280x800x24 >/dev/null 2>&1 &
XVFB=$!
sleep 1.5

rm -f "$LOG"
"$APP" >"$LOG" 2>&1 &
MAIN=$!
sleep 3

echo "== 发送 --show-group-manager =="
"$APP" --show-group-manager
echo "   rc=$?"
sleep 1.5

echo "== 窗口列表（找管理窗口标题）=="
python3 - <<'PY'
import subprocess
out = subprocess.run(["xdotool", "search", "--name", ".*"], capture_output=True, text=True).stdout.split()
found = None
for wid in out:
    r = subprocess.run(["xdotool", "getwindowname", wid], capture_output=True, text=True)
    name = r.stdout.strip()
    if "分组管理" in name:
        found = (wid, name)
        print("  找到窗口:", found)
        break
assert found, "FAIL: 未找到分组管理窗口"
PY

echo "== Esc 关闭对话框 =="
xdotool key Escape
sleep 1

echo "== 主实例应仍存活 =="
if kill -0 $MAIN 2>/dev/null; then
    echo "   主实例存活 OK"
else
    echo "   FAIL: 主实例已退出"
fi

echo "== 再发一次（验证可重复打开）=="
"$APP" --show-group-manager
sleep 1.2
xdotool key Escape
sleep 0.8

kill $MAIN 2>/dev/null
sleep 0.3
kill $XVFB 2>/dev/null

echo
echo "== 日志 =="
grep -E "单实例|分组管理" "$LOG" | head -8

python3 - <<'PY'
import re
log = open("/tmp/jie2-gm.log", encoding="utf-8").read()
n = len(re.findall(r"打开贴图分组管理", log))
assert n >= 2, f"FAIL: 期望至少 2 次打开，实际 {n}"
print(f"✓ 分组管理窗口打开 {n} 次（可重复）")
print("PASS: --show-group-manager 验证通过")
PY
