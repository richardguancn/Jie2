#!/usr/bin/env bash
# 最小复现：衣架点击卡片崩溃（供 gdb 抓栈）
set -u
export DISPLAY=:99
pkill -f "Xvfb :99" 2>/dev/null
pkill -f "build-linux/src/ui" 2>/dev/null
sleep 0.5
Xvfb :99 -screen 0 1280x800x24 >/dev/null 2>&1 &
XVFB=$!
sleep 1.5

rm -rf /root/.config/jie2 /root/Pictures/jie2
/work/build-linux/src/ui/jie2 --select-rect 0,0,64,64 >/dev/null 2>&1

# 在 gdb 里启动托盘进程
cat > /tmp/gdb-cmds.txt <<'GDB'
run
bt
info registers pc
quit
GDB

# 后台启 gdb（attach 式启动）
gdb -batch -x /tmp/gdb-cmds.txt \
    -ex "set args" \
    --args /work/build-linux/src/ui/jie2 > /tmp/gdb-out.txt 2>&1 &
GDB_PID=$!
sleep 4   # 等应用起来

# 触发展开 + 点击卡片 1
xdotool mousemove --sync 640 1
sleep 1.2
xdotool mousemove --sync 91 74
sleep 0.3
xdotool click 1
sleep 2

# 若还活着就杀掉，让 gdb 退出
if kill -0 $GDB_PID 2>/dev/null; then
    pkill -f "build-linux/src/ui/jie2" 2>/dev/null
    sleep 1
    # 发 SIGINT 让 gdb 打印一次
    pkill -INT -f "gdb -batch" 2>/dev/null
    sleep 1
    kill $GDB_PID 2>/dev/null
fi
sleep 0.5
kill $XVFB 2>/dev/null

echo "==== gdb 输出 ===="
tail -60 /tmp/gdb-out.txt
exit 0
