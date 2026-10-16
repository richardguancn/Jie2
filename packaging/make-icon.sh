#!/usr/bin/env bash
# 「截2」生成应用图标（PNG）
#
# 为什么用脚本生成而不是图片资源：
#   与程序内的托盘图标同源（同一套几何：取景框 + 对角线），
#   改一处两边一致；也避免在仓库里存二进制图片。
#
# 用法：bash packaging/make-icon.sh <输出路径.png> [尺寸]
set -eu

OUT="${1:?用法: make-icon.sh <out.png> [size]}"
SIZE="${2:-256}"

mkdir -p "$(dirname "$OUT")"

python3 - "$OUT" "$SIZE" <<'PY'
import struct
import sys
import zlib

path = sys.argv[1]
size = int(sys.argv[2])

# 几何与程序内托盘图标一致（ui/tray.cpp 的 makeTrayIcon）：
# 蓝色取景框 + 一条对角线。参数按 64 基准等比缩放到目标尺寸。
scale = size / 64.0
edge = int(round(10 * scale))        # 框到边缘的距离
thick = max(2, int(round(6 * scale)))  # 框线粗
diag_lo = int(round(23 * scale))     # 对角线起止（在框内）
diag_hi = int(round(41 * scale))

ACCENT = (64, 158, 255, 255)

px = bytearray()
for y in range(size):
    row = bytearray([0])  # filter type 0
    for x in range(size):
        on_border = (
            (abs(x - edge) < thick and edge - thick <= y <= size - edge + thick)
            or (abs(x - (size - 1 - edge)) < thick and edge - thick <= y <= size - edge + thick)
            or (abs(y - edge) < thick and edge - thick <= x <= size - edge + thick)
            or (abs(y - (size - 1 - edge)) < thick and edge - thick <= x <= size - edge + thick)
        )
        # 对角线：45° 线宽 thick 的近似（|x - y| 判定）
        on_diag = abs(x - y) < thick and diag_lo <= x <= diag_hi
        if on_border or on_diag:
            row += bytes(ACCENT)
        else:
            row += bytes([0, 0, 0, 0])
    px += row

def chunk(tag, data):
    out = struct.pack(">I", len(data)) + tag + data
    out += struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    return out

png = b"\x89PNG\r\n\x1a\n"
png += chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(bytes(px), 9))
png += chunk(b"IEND", b"")
open(path, "wb").write(png)
print("icon written:", path, f"({size}x{size})")
PY
