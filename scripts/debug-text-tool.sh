#!/usr/bin/env bash
# 调试：文字输入之后工具栏/画布点击是否失效
set -u
export DISPLAY=:99
pkill -f "Xvfb :99" 2>/dev/null; sleep 0.5
Xvfb :99 -screen 0 1280x800x24 >/dev/null 2>&1 &
XVFB=$!
sleep 1.5
rm -f /tmp/dbg2.log
/work/build-linux/src/ui/jie2 --select >/tmp/dbg2.log 2>&1 &
APP=$!
sleep 2.5

log() { echo "---- $1 ----"; grep -E "切换工具|标注已添加|文字输入|选区结算" /tmp/dbg2.log | tail -8; }

# 选区
xdotool mousemove --sync 100 100 sleep 0.2 mousedown 1 sleep 0.2
for i in 1 2 3 4 5 6; do xdotool mousemove --sync $((100+i*66)) $((100+i*50)) sleep 0.06; done
xdotool mouseup 1 sleep 0.5
log "选区后"

# 点文字按钮 (556,425)
xdotool mousemove --sync 556 425 sleep 0.2 click 1 sleep 0.8
log "点文字按钮后"

# 点画布 (150,340) → 文字输入
xdotool mousemove --sync 150 340 sleep 0.2 click 1 sleep 0.8
log "点画布（开文字输入）后"

# 输入并回车
xdotool type --delay 60 "TEST" sleep 0.3
xdotool key Return sleep 1.0
log "回车提交后"

# 点序号按钮 (614,425)
xdotool mousemove --sync 614 425 sleep 0.3 click 1 sleep 1.0
log "点序号按钮后"

# 点画布 (430,150)
xdotool mousemove --sync 430 150 sleep 0.3 click 1 sleep 1.0
log "点画布放序号后"

kill $APP 2>/dev/null; kill $XVFB 2>/dev/null
echo "==== 完整日志尾部 ===="
tail -20 /tmp/dbg2.log
exit 0
