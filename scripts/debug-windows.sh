#!/usr/bin/env bash
# 列出 Xvfb 会话里的窗口（用于调试窗口查找）
set -u
export DISPLAY=:99
pkill -f "Xvfb :99" 2>/dev/null
pkill -f "build-linux/src/ui" 2>/dev/null
sleep 0.5
Xvfb :99 -screen 0 1280x800x24 >/dev/null 2>&1 &
XVFB=$!
sleep 1.5

rm -rf /root/.config/jie2 /root/Pictures/jie2
/work/build-linux/src/ui/jie2 --select-rect 0,0,48,48 >/dev/null 2>&1

/work/build-linux/src/ui/jie2 >/tmp/w.log 2>&1 &
APP=$!
sleep 2.5
xdotool mousemove --sync 640 1
sleep 1.0

{
  echo "=== 遍历所有窗口，看名字与几何 ==="
  for id in $(xdotool search --name ".*" 2>/dev/null); do
    name=$(xdotool getwindowname "$id" 2>/dev/null || echo "(no-name)")
    geo=$(xdotool getwindowgeometry --shell "$id" 2>/dev/null | tr '\n' ' ')
    echo "id=$id name=[$name] $geo"
  done
} > /tmp/wins-out.txt 2>&1

kill $APP 2>/dev/null
sleep 0.2
kill $XVFB 2>/dev/null

cat /tmp/wins-out.txt
exit 0
