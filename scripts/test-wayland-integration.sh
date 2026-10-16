#!/usr/bin/env bash
# Wayland 集成验证：jie2 主程序在 Wayland 会话下自动启用 layer-shell 顶部条
#
# 链路（全真实）：
#   1. jie2test 的配置目录预置 history.txt（2 条真实 PNG）
#   2. sway headless 里以 Wayland 平台插件启动 jie2
#   3. 断言：日志"衣架改用 layer-shell 顶部条" + "已启动 jie2-line"
#   4. 断言：items 文件被写出（2 行）；jie2-line 子进程在跑
#   5. 断言：grim 截图里顶部条带与缩略图色块存在（合成器输出）
#   6. 杀掉 jie2 → 断言 jie2-line 子进程随之退出（无孤儿）
#
# 用法（容器内）：bash scripts/test-wayland-integration.sh
set -u

TEST_USER=jie2test
RUNTIME_DIR=/tmp/xdg-jie2
SWAY_CONF=/tmp/sway-jie2.conf
INNER=/tmp/jie2-wayland-integration.sh
HOME_DIR=/home/jie2test

# 清场注意：用 `pkill -x`（精确匹配**进程名**）而不是 `pkill -f`。
# -f 是匹配整个命令行，而"执行 pkill 的 shell 自己"的命令行里也含
# "jie2-line" 字样 —— pkill 会把自己杀掉（实测退出码 137，输出被截断）。
pkill -x jie2-line 2>/dev/null
pkill -f "sway -c $SWAY_CONF" 2>/dev/null
pkill -f "build-linux/src/ui/jie2" 2>/dev/null
sleep 0.6

if ! id "$TEST_USER" >/dev/null 2>&1; then
    useradd -m -s /bin/bash "$TEST_USER"
fi
rm -rf "$RUNTIME_DIR"
mkdir -p "$RUNTIME_DIR"
chown "$TEST_USER" "$RUNTIME_DIR"
chmod 700 "$RUNTIME_DIR"

cat > "$SWAY_CONF" <<'EOF'
output HEADLESS-1 resolution 1280x800
EOF
chmod 644 "$SWAY_CONF"

# 预置历史（2 条真实 PNG；这是 jie2 读到的"已有截图"）
python3 - <<'PY'
from PIL import Image
Image.new("RGB", (120, 80), (220, 40, 40)).save("/tmp/hist-1.png")
Image.new("RGB", (80, 120), (40, 120, 220)).save("/tmp/hist-2.png")
PY
mkdir -p "$HOME_DIR/.config/jie2"
cat > "$HOME_DIR/.config/jie2/history.txt" <<EOF
# jie2 capture history (oldest first)
/tmp/hist-1.png|20261016-100000|120|80
/tmp/hist-2.png|20261016-100001|80|120
EOF
# 主题固定深色：本脚本的像素断言基于深色主题的卡片玻璃配色。
# 容器 gsettings 返回 'default'（解析为浅色），不固定主题会让断言
# 随环境漂移 —— 与 test-clothesline-linux.sh 同样的处置（实测教训）。
cat > "$HOME_DIR/.config/jie2/jie2.conf" <<EOF
appearance.theme=dark
EOF
chown -R "$TEST_USER" "$HOME_DIR/.config"
rm -f "$HOME_DIR/.config/jie2/line-items.txt"

rm -f /tmp/sway-jie2.log /tmp/jie2-wayland.log /tmp/jie2-line.log \
      /tmp/jie2-line-shot.png /tmp/grim.err

cat > "$INNER" <<'INNER_EOF'
#!/usr/bin/env bash
set -u
export XDG_RUNTIME_DIR=/tmp/xdg-jie2
export WLR_BACKENDS=headless
export WLR_LIBINPUT_NO_DEVICES=1
export QT_QPA_PLATFORM=wayland

sway -c /tmp/sway-jie2.conf > /tmp/sway-jie2.log 2>&1 &
SWAY_PID=$!
for i in $(seq 1 60); do
    SOCK=$(ls "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null | head -1)
    [ -n "$SOCK" ] && break
    sleep 0.25
done
[ -n "${SOCK:-}" ] || { echo "NO-SOCKET"; exit 1; }
export WAYLAND_DISPLAY="$(basename "$SOCK")"
export SWAYSOCK="$(ls "$XDG_RUNTIME_DIR"/sway-ipc.* 2>/dev/null | head -1)"
sleep 0.6

# 启动 jie2（走 Wayland 平台插件；后台常驻）
/work/build-linux/src/ui/jie2 > /tmp/jie2-wayland.log 2>&1 &
JIE2_PID=$!
sleep 4.0

# 截图（合成器输出）
grim /tmp/jie2-line-shot.png 2>/tmp/grim.err || echo "GRIM-FAILED"

# 记录子进程状态。
# 注意：必须排除**僵尸**（状态 Z）——容器 PID 1 通常不回收孤儿，
# 上一轮测试死掉的 jie2-line 会以 <defunct> 形式残留；僵尸已死亡，
# 不能当作"仍在运行"（早期版本因此出现假阳性失败）。
alive_line_pids() {
    for p in $(pgrep -x jie2-line 2>/dev/null); do
        st=$(ps -o stat= -p "$p" 2>/dev/null | tr -d ' ')
        case "$st" in
            Z*|"") ;;          # 僵尸或已消失：不算存活
            *) echo "$p" ;;
        esac
    done
}
alive_line_pids > /tmp/line-pids.txt

# 杀掉 jie2，等它清理子进程（SIGTERM → 优雅退出 → 析构停子进程）
kill $JIE2_PID 2>/dev/null
sleep 2.5
alive_line_pids > /tmp/line-pids-after.txt

kill $SWAY_PID 2>/dev/null
sleep 0.4
exit 0
INNER_EOF
chmod 755 "$INNER"

echo "== 启动（sway + jie2，Wayland 集成）=="
runuser -u "$TEST_USER" -- bash "$INNER" || true

echo
echo "== jie2 日志 =="
grep -E "Wayland|layer-shell|顶部条|已启动|热键" /tmp/jie2-wayland.log | head -10

echo
echo "== 断言 1：日志与文件 =="
python3 - <<'PY'
import re, os
log = open("/tmp/jie2-wayland.log", encoding="utf-8", errors="replace").read()
assert "Wayland" in log, "FAIL: 未检测为 Wayland 会话"
assert "layer-shell 顶部条" in log, "FAIL: 未切换到 layer-shell 顶部条"
assert "已启动" in log and "jie2-line" in log, "FAIL: 未启动 jie2-line"
print("  ✓ 日志确认切换到 layer-shell 顶部条并启动子进程")

items = "/home/jie2test/.config/jie2/line-items.txt"
assert os.path.exists(items), "FAIL: items 文件未写出"
lines = [l for l in open(items, encoding="utf-8").read().splitlines() if l.strip()]
assert len(lines) == 2, f"FAIL: items 应为 2 行，实际 {len(lines)}"
print(f"  ✓ items 文件已写出：{lines}")
PY

echo
echo "== 断言 2：子进程生命周期 =="
python3 - <<'PY'
pids = [p for p in open("/tmp/line-pids.txt").read().split() if p.strip()]
after = [p for p in open("/tmp/line-pids-after.txt").read().split() if p.strip()]
assert pids, "FAIL: jie2-line 子进程未运行"
print(f"  ✓ 运行时 jie2-line 进程存在：{pids}")
assert not after, f"FAIL: jie2 退出后 jie2-line 仍在：{after}（孤儿进程）"
print("  ✓ jie2 退出后子进程已清理（无孤儿）")
PY

echo
echo "== 断言 3：合成器输出像素 =="
python3 - <<'PY'
from PIL import Image
im = Image.open("/tmp/jie2-line-shot.png").convert("RGB")
w, h = im.size
print(f"  截图尺寸: {w}x{h}")

# tendedero 样式布局（1280 宽、2 张卡、间距 174 居中）：
#   卡1（120×80 红）照片 136×91，中心 (553, 87)
#   卡2（80×120 蓝）照片 69×104，中心 (727, 93)
# 缩略图（不透明、色值精确）—— 最强判据
t1 = im.getpixel((553, 87))
t2 = im.getpixel((727, 93))
print(f"  缩略图1: {t1}（期望红 220,40,40）")
print(f"  缩略图2: {t2}（期望蓝 40,120,220）")
assert abs(t1[0] - 220) < 30 and t1[1] < 80, "FAIL: 缩略图1缺失"
assert t2[2] > 150 and t2[0] < 130, "FAIL: 缩略图2缺失"

# 抛物线晾衣绳：中心 (640,~33) 有绳芯。该处高光（白 45%）与灰芯（140）
# 叠在同一行 → ≈192；灰芯单独可见处 ≈140。容差 90..210。
rope = im.getpixel((640, 33))
print(f"  绳芯 (640,33): {rope}")
assert 90 <= rope[0] <= 210 and abs(rope[0] - rope[1]) < 20 and \
    abs(rope[1] - rope[2]) < 20, "FAIL: 晾衣绳未渲染"

# 透明面板：无绳无卡区域应透出桌面背景
# （sway headless 默认灰色 (63,63,63) —— 实测标定；判据用灰阶 + 与绳芯的差）。
# 空白点 (400,100)：卡片区 x∈[481,799]、绳区 y≤35 之外。
bg = im.getpixel((400, 100))
print(f"  面板空白区 (400,100): {bg}")
is_gray = abs(bg[0] - bg[1]) < 6 and abs(bg[1] - bg[2]) < 6
assert is_gray and bg[0] < 100, f"FAIL: 面板不透明或背景异常: {bg}"

# 条带下方同样是桌面背景（面板透明，上下应一致）。
below = im.getpixel((700, 400))
print(f"  条带下方: {below}")
is_gray = abs(below[0] - below[1]) < 6 and abs(below[1] - below[2]) < 6
assert is_gray, f"FAIL: 条带下方不是背景: {below}"
assert abs(sum(below) - sum(bg)) < 30, "FAIL: 面板上下背景不一致"
print("  ✓ layer-shell 条带真实渲染到了合成器输出（透明面板 + 绳 + 卡片）")
PY

echo
echo "PASS: Wayland 集成验证通过（自动启用 layer-shell 顶部条 + 子进程生命周期）"
