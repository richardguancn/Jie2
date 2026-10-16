#!/usr/bin/env python3
"""解析 XWD (X Window Dump) 文件并输出像素值 — 用于独立核对抓屏结果。

用法：python3 debug_xwd.py <file.xwd> [x y] ...
不带坐标时采样四角与中心。

注意：XWD 头部按 X11 协议惯例使用**大端序**存储，
而像素数据按头部的 byte_order 字段（0=LSBFirst）决定字节序。
colormap 条目为 12 字节（4B pixel + 2B r + 2B g + 2B b + 1B flags + 1B pad）。
"""
import struct
import sys

path = sys.argv[1]
coords = [(int(sys.argv[i]), int(sys.argv[i + 1])) for i in range(2, len(sys.argv) - 1, 2)]

with open(path, "rb") as f:
    d = f.read()

(hs, ver, fmt, depth, w, h, xoff) = struct.unpack_from(">IIIIIII", d, 0)
(byte_order, bunit, bbit, bpad, bpp, bpl) = struct.unpack_from(">IIIIII", d, 28)
(vclass, rm, gm, bm, bprgb, cmap, ncol) = struct.unpack_from(">IIIIIII", d, 52)

print(f"XWD: {w}x{h} depth={depth} bpp={bpp} bpl={bpl} byte_order={byte_order}")
print(f"     masks r={rm:08x} g={gm:08x} b={bm:08x} ncolors={ncol} header={hs}")

# 像素数据起点：header_size（已含 window name）+ colormap 条目（12 字节/条）
idx = hs + ncol * 12
expected = idx + bpl * h
print(f"     pixel data at {idx}, file size {len(d)}, expected {expected}")

if not coords:
    coords = [(0, 0), (w - 1, 0), (w // 2, h // 2), (0, h - 1), (w - 1, h - 1)]

def get_pixel(x, y):
    off = idx + y * bpl + x * (bpp // 8)
    span = bpp // 8
    val = int.from_bytes(d[off:off + span], "little" if byte_order == 0 else "big")
    r = (val & rm) >> 16 if rm else 0
    g = (val & gm) >> 8 if gm else 0
    b = val & bm if bm else 0
    return r, g, b, val

for (x, y) in coords:
    if 0 <= x < w and 0 <= y < h:
        r, g, b, val = get_pixel(x, y)
        print(f"  pixel({x:4d},{y:4d}) = #{r:02X}{g:02X}{b:02X}  raw=0x{val:08X}")
    else:
        print(f"  pixel({x},{y}) 越界")
