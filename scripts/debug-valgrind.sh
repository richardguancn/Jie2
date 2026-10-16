#!/usr/bin/env bash
# valgrind 定位衣架崩溃：跑最小复现并收集第一次非法访问
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

# valgrind 启动应用（关闭 leak 检查，聚焦内存错误）
G_SLICE=always-malloc valgrind \
    --error-exitcode=99 \
    --errors-for-leak-kinds=none \
    --leak-check=no \
    --track-origins=yes \
    --log-file=/tmp/vg.log \
    /work/build-linux/src/ui/jie2 > /tmp/vg-app.log 2>&1 &
VG_PID=$!
sleep 8   # valgrind 下启动慢

# 触发：展开 + 点击卡片
xdotool mousemove --sync 640 1
sleep 2
xdotool mousemove --sync 91 74
sleep 0.5
xdotool click 1
sleep 3

pkill -f "build-linux/src/ui/jie2" 2>/dev/null
sleep 3   # 等 valgrind 写完报告
kill $VG_PID 2>/dev/null
sleep 0.5
kill $XVFB 2>/dev/null

echo "==== valgrind 报告（前 80 行）===="
head -80 /tmp/vg.log
echo
echo "==== 应用日志尾部 ===="
tail -12 /tmp/vg-app.log
exit 0
