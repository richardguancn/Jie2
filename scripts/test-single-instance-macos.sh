#!/usr/bin/env bash
# 单实例通道验证（macOS 本机）
# 场景：启动常驻实例 → --empty-group 送达 → 二次启动唤起 → 清理
set -u
APP="/Users/gdy/Documents/My/App/jie2/build-macos/src/ui/jie2.app/Contents/MacOS/jie2"
LOG=/tmp/jie2-si.log
USER_NAME="$(whoami)"

pkill -f "MacOS/jie2$" 2>/dev/null
pkill -f "MacOS/jie2 " 2>/dev/null
sleep 0.5
# 清理可能的残留 socket
rm -f "/tmp/jie2-single-${USER_NAME}" 2>/dev/null

rm -f "$LOG"
echo "== 1. 启动常驻实例 =="
"$APP" >"$LOG" 2>&1 &
MAIN_PID=$!
sleep 3
if ! kill -0 $MAIN_PID 2>/dev/null; then
    echo "FAIL: 主实例未启动"
    cat "$LOG"
    exit 1
fi
echo "   主实例 pid=$MAIN_PID"

echo "== 2. 发送 --empty-group =="
"$APP" --empty-group
echo "   送达 rc=$?"
sleep 1

echo "== 3. 二次启动（无参数，应唤起并退出）=="
"$APP"
echo "   二次启动 rc=$?"
sleep 1

echo "== 4. 检查进程数（应只有 1 个实例）=="
COUNT=$(pgrep -f "MacOS/jie2$" | wc -l | tr -d ' ')
echo "   实例数=$COUNT"

echo "== 5. 清理 =="
kill $MAIN_PID 2>/dev/null
sleep 1
kill -9 $MAIN_PID 2>/dev/null

echo
echo "== 主实例日志 =="
grep -E "单实例|唤起|清空|贴图" "$LOG" | head -12

python3 - <<'PY'
import re
log = open("/tmp/jie2-si.log", encoding="utf-8").read()

def need(pattern, name):
    m = re.search(pattern, log)
    assert m, f"FAIL: 日志缺少「{name}」"
    print(f"✓ {name}")

need(r"单实例通道收到命令：--empty-group", "收到 empty-group 命令")
need(r"已清空贴图第 \d+ 组", "执行了清空")
need(r"唤起已有实例", "二次启动唤起")

count_line = None
print("OK")
PY
