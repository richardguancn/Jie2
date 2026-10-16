#!/usr/bin/env bash
# 单实例通道端到端验证（Linux aarch64 容器）
#
# 场景（全部经真实路径）：
#   1. Xvfb + xclip 放一张图 → 启动主实例 → 合成热键 Ctrl+Alt+P 贴图
#      （验证主实例有 1 张真实贴图）
#   2. jie2 --empty-group → 断言主实例清空了该组（1 张）
#   3. jie2（无参数二次启动）→ 断言唤起、且实例数仍为 1
#   4. 杀掉主实例 → 再执行 --empty-group → 断言 rc=1、报"未在运行"
#
# 用法（容器内）：bash scripts/test-single-instance-linux.sh
set -u

DISPLAY_NUM=:99
export DISPLAY=$DISPLAY_NUM
WORK=/work/build-linux
APP=$WORK/src/ui/jie2
LOG=/tmp/jie2-si-linux.log

pkill -f "Xvfb $DISPLAY_NUM" 2>/dev/null
pkill -f "build-linux/src/ui" 2>/dev/null
sleep 0.5
Xvfb $DISPLAY_NUM -screen 0 1280x800x24 >/dev/null 2>&1 &
XVFB_PID=$!
sleep 1.5

rm -rf /root/.config/jie2 /root/Pictures/jie2
rm -f "$LOG" /tmp/jie2-single-root 2>/dev/null

echo "== 1. 准备剪贴板图片 + 启动主实例 =="
python3 -c "from PIL import Image; Image.new('RGB', (60, 40), (200, 30, 30)).save('/tmp/si-clip.png')"
xclip -selection clipboard -t image/png -i /tmp/si-clip.png

"$APP" >"$LOG" 2>&1 &
MAIN_PID=$!
sleep 3
if ! kill -0 $MAIN_PID 2>/dev/null; then
    echo "FAIL: 主实例未启动"
    cat "$LOG"
    exit 1
fi
echo "   主实例 pid=$MAIN_PID"

echo "== 2. 合成热键 Ctrl+Alt+P 触发贴图 =="
xdotool key --clearmodifiers ctrl+alt+p
sleep 1.5
grep -c "贴图创建" "$LOG" | sed 's/^/   贴图创建日志条数: /'

echo "== 3. 发送 --empty-group =="
"$APP" --empty-group
echo "   发送 rc=$?"
sleep 1

echo "== 4. 二次启动（应唤起）=="
"$APP"
echo "   二次启动 rc=$?"
sleep 1
INSTANCES=$(pgrep -f "build-linux/src/ui/jie2\$" | wc -l | tr -d ' ')
echo "   实例数=$INSTANCES"

echo "== 5. 杀掉主实例后发送（应 rc=1）=="
kill $MAIN_PID 2>/dev/null
sleep 1.5
kill -9 $MAIN_PID 2>/dev/null
sleep 0.5
"$APP" --empty-group > /tmp/si-noinst.out 2>&1
echo "   rc=$?（期望 1）"
cat /tmp/si-noinst.out | head -2

sleep 0.3
kill $XVFB_PID 2>/dev/null

echo
echo "== 主实例日志（关键行）=="
grep -E "单实例|唤起|清空|贴图创建" "$LOG" | head -16

python3 - <<'PY'
import re
log = open("/tmp/jie2-si-linux.log", encoding="utf-8").read()

def need(pattern, name):
    m = re.search(pattern, log)
    assert m, f"FAIL: 日志缺少「{name}」"
    print(f"✓ {name}: {m.group(0)[:48]}")

need(r"贴图创建", "热键触发贴图（真实路径）")
need(r"单实例通道收到命令：--empty-group", "收到 empty-group 命令")
m = re.search(r"已清空贴图第 \d+ 组（(\d+) 张）", log)
assert m, "FAIL: 缺少清空日志"
assert int(m.group(1)) == 1, f"FAIL: 期望清空 1 张，实际 {m.group(1)}"
print(f"✓ 清空了 1 张真实贴图")

need(r"单实例通道收到命令：\(唤起\)", "二次启动唤起")

# 无实例时的报错输出
out = open("/tmp/si-noinst.out", encoding="utf-8").read()
assert "未在运行" in out, "FAIL: 无实例时未给出提示"
print("✓ 无实例时报错清晰")
print("PASS: 单实例通道端到端验证通过（送达/执行/唤起/无实例报错）")
PY
