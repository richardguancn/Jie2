#!/usr/bin/env bash
# 「截2」RPM 打包脚本（Fedora / RHEL / openEuler 系）
#
# 产出：jie2-<版本>-1.<架构>.rpm
# 内容与依赖策略对齐 deb / AppImage：
#   - 内容：/usr/bin/jie2（GUI）+ jie2-probe（诊断）+ 桌面文件 + 图标
#   - 依赖：发行版自带 Qt（qt6-qtbase 等），不捆绑运行时
#
# 需要 rpmbuild（Debian 系：apt install rpm；Fedora 系自带）。
#
# 用法（在构建目录已存在的前提下）：
#   bash scripts/build-rpm.sh [build-dir] [output-dir]
set -eu

BUILD_DIR="${1:-build-linux}"
OUT_DIR="${2:-dist}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$ROOT/.dyworker/tmp/rpmbuild"
VERSION="0.1.0"
ARCH="$(uname -m)"

if ! command -v rpmbuild >/dev/null 2>&1; then
    echo "错误：找不到 rpmbuild。Debian 系请先 apt install rpm。" >&2
    exit 1
fi

JIE2_BIN="$ROOT/$BUILD_DIR/src/ui/jie2"
PROBE_BIN="$ROOT/$BUILD_DIR/src/tools/jie2-probe"
[ -x "$JIE2_BIN" ] || { echo "错误：找不到 $JIE2_BIN（先构建）" >&2; exit 1; }
[ -x "$PROBE_BIN" ] || { echo "错误：找不到 $PROBE_BIN（先构建）" >&2; exit 1; }

# ---- 组装 rpmbuild 工作树 ----
rm -rf "$WORK"
mkdir -p "$WORK"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}
mkdir -p "$WORK/SOURCES/payload"

install -m 0755 "$JIE2_BIN" "$WORK/SOURCES/payload/jie2"
install -m 0755 "$PROBE_BIN" "$WORK/SOURCES/payload/jie2-probe"
# jie2-line（Wayland 顶部条）：可选，存在才打包。
if [ -x "$ROOT/$BUILD_DIR/src/tools/jie2-line" ]; then
    install -m 0755 "$ROOT/$BUILD_DIR/src/tools/jie2-line" "$WORK/SOURCES/payload/jie2-line"
fi

# 桌面文件与图标：优先取打包脚本已备好的版本，否则现场生成。
if [ -f "$ROOT/packaging/jie2.desktop" ]; then
    cp "$ROOT/packaging/jie2.desktop" "$WORK/SOURCES/payload/jie2.desktop"
else
    cat > "$WORK/SOURCES/payload/jie2.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=截2
Comment=截图 / 标注 / 贴图工具
Exec=jie2
Icon=jie2
Terminal=false
Categories=Graphics;Utility;
EOF
fi

if [ -f "$ROOT/packaging/jie2.png" ]; then
    cp "$ROOT/packaging/jie2.png" "$WORK/SOURCES/payload/jie2.png"
else
    # 占位图标：64x64 蓝底白角 PNG（由 Python 现场生成，不依赖设计资源）
    python3 - "$WORK/SOURCES/payload/jie2.png" <<'PYEOF'
import struct, sys, zlib

size = 256
rows = []
for y in range(size):
    row = bytearray([0])  # filter type 0
    for x in range(size):
        # 圆角方格 + 对角留白（极简占位）
        inside = 24 <= x < size - 24 and 24 <= y < size - 24
        slash = abs((x - 64) - (y - 64)) < 22
        r, g, b = (37, 99, 235) if inside and not slash else (255, 255, 255)
        row += bytes((r, g, b))
    rows.append(bytes(row))
raw = b"".join(rows)

def chunk(tag, data):
    c = struct.pack(">I", len(data)) + tag + data
    c += struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    return c

png = b"\x89PNG\r\n\x1a\n"
png += chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(raw, 9))
png += chunk(b"IEND", b"")
with open(sys.argv[1], "wb") as fh:
    fh.write(png)
print("已生成占位图标", sys.argv[1])
PYEOF
fi

# spec：替换版本与架构占位符
sed -e "s/@VERSION@/$VERSION/" -e "s/@ARCH@/$ARCH/" \
    "$ROOT/packaging/jie2.spec.in" > "$WORK/SPECS/jie2.spec"

# ---- 构建 ----
rpmbuild -bb "$WORK/SPECS/jie2.spec" \
    --define "_topdir $WORK" \
    --buildroot "$WORK/BUILDROOT" 2>&1 | tail -6

# ---- 收集产物 ----
mkdir -p "$ROOT/$OUT_DIR"
RPM_FILE="$(find "$WORK/RPMS" -name '*.rpm' | head -1)"
cp "$RPM_FILE" "$ROOT/$OUT_DIR/"
echo "RPM 打包完成：$OUT_DIR/$(basename "$RPM_FILE")"
