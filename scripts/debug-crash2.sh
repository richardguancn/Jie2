#!/usr/bin/env bash
# gdb 打印衣架崩溃现场（Debug 构建，精确行号）
set -u
export DISPLAY=:99
BIN=/work/build-debug/src/ui/jie2
[ -x "$BIN" ] || { echo "缺 build-debug 二进制"; exit 2; }

pkill -f "Xvfb :99" 2>/dev/null
pkill -f "build-debug/src/ui" 2>/dev/null
sleep 0.5
Xvfb :99 -screen 0 1280x800x24 >/dev/null 2>&1 &
XVFB=$!
sleep 1.5

rm -rf /root/.config/jie2 /root/Pictures/jie2
/work/build-linux/src/ui/jie2 --select-rect 0,0,64,64 >/dev/null 2>&1

cat > /tmp/gdb-cmds.txt <<'GDB'
run
bt 6
frame 1
info args
p this->entries_.size()
p this->entries_.data()
p this->pressedIndex_
p this->pressedOnCard_
p this->entries_[0].entry.path.size()
quit
GDB

gdb -batch -x /tmp/gdb-cmds.txt "$BIN" > /tmp/gdb-out2.txt 2>&1 &
GDB_PID=$!
sleep 4

xdotool mousemove --sync 640 1
sleep 1.5
xdotool mousemove --sync 91 74
sleep 0.3
xdotool click 1
sleep 3

pkill -f "build-debug/src/ui/jie2" 2>/dev/null
sleep 2
kill $GDB_PID 2>/dev/null
sleep 0.3
kill $XVFB 2>/dev/null

echo "==== gdb 输出 ===="
grep -vE "^\[New Thread|^\[Thread|Detaching" /tmp/gdb-out2.txt | tail -50
exit 0
