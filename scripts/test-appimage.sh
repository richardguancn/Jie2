#!/usr/bin/env bash
# AppImage GUI 冒烟测试（容器内）
set -u
export DISPLAY=:98
pkill -f "Xvfb :98" 2>/dev/null
sleep 0.5
Xvfb :98 -screen 0 1024x768x24 >/dev/null 2>&1 &
XV=$!
sleep 1.5

cd /tmp/appimg-test || exit 2
./jie2-0.1.0-aarch64.AppImage > /tmp/appimg.log 2>&1 &
AG=$!
sleep 4
if kill -0 $AG 2>/dev/null; then
    echo "AppImage GUI 运行中 OK"
    kill $AG 2>/dev/null
else
    echo "AppImage GUI 已退出 FAIL"
fi
sleep 0.5
kill $XV 2>/dev/null
echo "--- 日志 ---"
tail -4 /tmp/appimg.log
exit 0
