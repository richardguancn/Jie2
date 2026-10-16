#!/usr/bin/env bash
# 验证 macOS 自包含包（bundle-qt）：解压 → 运行 --version → GUI 冒烟
set -u
ZIP="/Users/gdy/Documents/My/App/jie2/dist/jie2-0.1.0-macos-arm64.zip"
DEST=/tmp/jie2-selfcontained-check

pkill -f "$DEST" 2>/dev/null
sleep 0.3
rm -rf "$DEST"
mkdir -p "$DEST"
cd "$DEST" || exit 1
ditto -x -k "$ZIP" .

echo "=== 包内框架 ==="
ls jie2.app/Contents/Frameworks/

echo
echo "=== --version（不启动 GUI）==="
./jie2.app/Contents/MacOS/jie2 --version
echo "rc=$?"

echo
echo "=== 隐藏 Homebrew Qt 后运行（模拟无 Qt 机器）==="
# 通过 DYLD_LIBRARY_PATH 无法"隐藏"绝对路径 —— 但我们的引用已改成
# @executable_path，不会去找 Homebrew。用 dyld 的日志确认实际加载来源：
DYLD_PRINT_LIBRARIES=1 ./jie2.app/Contents/MacOS/jie2 --version 2>&1 | grep -iE "Qt(Core|Gui|Widgets|DBus)" | head -6

echo
echo "=== GUI 冒烟（4 秒）==="
./jie2.app/Contents/MacOS/jie2 > /tmp/sc-gui.log 2>&1 &
APP=$!
sleep 4
if kill -0 $APP 2>/dev/null; then
    echo "GUI 运行中 OK"
    kill $APP 2>/dev/null
else
    echo "GUI 已退出 FAIL"
fi
sleep 0.5
tail -3 /tmp/sc-gui.log
exit 0
