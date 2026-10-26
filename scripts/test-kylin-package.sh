#!/usr/bin/env bash
# 只在一次性 Debian 系容器中执行：安装、运行、升级和卸载成品。
set -euo pipefail
if [ ! -f /.dockerenv ] || [ "$(id -u)" != 0 ]; then
    echo "此脚本会安装和卸载软件包，只允许在 root 身份的一次性 Docker 容器内运行。" >&2
    exit 1
fi
PACKAGE="${1:?用法：test-kylin-package.sh /path/to/jie2_arm64.deb}"
PACKAGE="$(realpath "$PACKAGE")"
test "$(dpkg-deb -f "$PACKAGE" Architecture)" = arm64
test "$(dpkg --print-architecture)" = arm64
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends xvfb xauth desktop-file-utils python3 python3-pil "$PACKAGE"
test "$(dpkg-query -W -f='${Status}' jie2)" = 'install ok installed'
desktop-file-validate /usr/share/applications/jie2.desktop
for size in 16 32 48 64 128 256; do
    test -s "/usr/share/icons/hicolor/${size}x${size}/apps/jie2.png"
done
for binary in /usr/bin/jie2 /usr/bin/jie2-probe /usr/bin/jie2-line; do
    dependencies="$(ldd "$binary")"
    if [[ "$dependencies" == *'not found'* ]]; then
        echo "$dependencies" >&2
        exit 1
    fi
done
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
export QT_QPA_PLATFORM=xcb XDG_SESSION_TYPE=x11 XDG_CONFIG_HOME="$WORK/config" XDG_DATA_HOME="$WORK/data"
unset WAYLAND_DISPLAY
xvfb-run -a bash -euo pipefail -c '
    /usr/bin/jie2 --self-check
    /usr/bin/jie2 --select-rect 20,20,100,60 --output "$1/installed-capture.png"
    python3 - "$1/installed-capture.png" <<"PY"
from PIL import Image
import sys
assert Image.open(sys.argv[1]).size == (100, 60)
PY
' _ "$WORK"
# 普通用户也必须能读取安装文件并启动，不能只在 root 下验证。
mkdir -p "$WORK/user"
chmod 0755 "$WORK"
chown nobody "$WORK/user"
install -d -m 0700 -o nobody "$WORK/user/runtime"
runuser -u nobody -- env HOME="$WORK/user" \
    XDG_CONFIG_HOME="$WORK/user/config" XDG_DATA_HOME="$WORK/user/data" \
    XDG_RUNTIME_DIR="$WORK/user/runtime" QT_QPA_PLATFORM=xcb \
    xvfb-run -a /usr/bin/jie2 --self-check
mkdir -p "$XDG_CONFIG_HOME/jie2"
printf 'keep-user-data\n' > "$XDG_CONFIG_HOME/jie2/package-preservation-check"
# 同版本重新安装覆盖安装路径，验证安装后脚本可重复运行。
dpkg -i "$PACKAGE"
dpkg --remove jie2
test ! -e /usr/bin/jie2
test ! -e /usr/share/applications/jie2.desktop
test ! -e /usr/share/icons/hicolor/256x256/apps/jie2.png
test "$(cat "$XDG_CONFIG_HOME/jie2/package-preservation-check")" = keep-user-data
echo "安装、重复安装、启动、截图和卸载检查通过，用户配置保留。"
