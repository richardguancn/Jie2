#!/usr/bin/env bash
# 在 macOS/Linux 上生成麒麟 V10 ARM64 的 Qt5 安装包。
# 用法：bash scripts/build-kylin-arm64.sh [--docker|--native] [output-dir]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODE="${1:---docker}"
OUT_DIR="${2:-$ROOT/dist/kylin-v10-arm64}"
case "$MODE" in
    --docker|--native) ;;
    *) echo "用法：$0 [--docker|--native] [output-dir]" >&2; exit 2 ;;
esac
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"
if [ "$MODE" = --docker ]; then
    IMAGE="${JIE2_KYLIN_IMAGE:-jie2-kylin-v10-builder}"
    docker build --platform linux/arm64 -t "$IMAGE" -f "$ROOT/packaging/kylin-v10/Dockerfile" "$ROOT/packaging/kylin-v10"
    docker run --rm --platform linux/arm64 \
        -v "$ROOT:/src:ro" -v "$OUT_DIR:/output" \
        -e JIE2_BUILD_JOBS="${JIE2_BUILD_JOBS:-4}" "$IMAGE" bash -c '
            cp -a /src/CMakeLists.txt /src/src /src/tests /src/assets /src/third_party /src/packaging /src/scripts /work/
            exec bash /work/scripts/build-kylin-arm64.sh --native /output
        '
    # 在没有开发工具的干净系统里，仅根据安装包声明安装运行依赖。
    APP_VERSION="$(awk '/^[[:space:]]+VERSION [0-9]+\.[0-9]+\.[0-9]+/ {print $2; exit}' "$ROOT/CMakeLists.txt")"
    test -n "$APP_VERSION"
    docker run --rm --platform linux/arm64 \
        -v "$ROOT/scripts:/checks:ro" -v "$OUT_DIR:/packages:ro" \
        ubuntu:20.04 bash /checks/test-kylin-package.sh "/packages/jie2_${APP_VERSION}_arm64.deb"
    exit 0
fi
if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != aarch64 ]; then
    echo "--native 必须在 Linux ARM64 上运行；其他环境请用 --docker。" >&2
    exit 1
fi
if [ "$(dpkg --print-architecture)" != arm64 ]; then
    echo "需要 arm64 的 Debian 系软件包环境。" >&2
    exit 1
fi
cd "$ROOT"
pkg-config --modversion x11 xrandr dbus-1 zlib wayland-client wayland-protocols
BUILD_DIR="$ROOT/build-kylin-arm64"
cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
    -DJIE2_QT_MAJOR_VERSION=5 -DJIE2_REQUIRE_UI=ON -DJIE2_BUILD_TESTS=ON
cmake --build "$BUILD_DIR" -j"${JIE2_BUILD_JOBS:-4}"
test -x "$BUILD_DIR/tests/jie2_ui_alignment_tests"
(cd "$BUILD_DIR" && ctest --output-on-failure --timeout 180)
bash "$ROOT/scripts/verify-kylin-arm64.sh" "$BUILD_DIR"
# Qt 5.12 的 HTTPS 后端运行时加载 OpenSSL 1.1，dpkg-shlibdeps 无法发现它。
JIE2_DEB_EXTRA_DEPENDS=libssl1.1 bash "$ROOT/scripts/build-deb.sh" "$BUILD_DIR" "$OUT_DIR"
