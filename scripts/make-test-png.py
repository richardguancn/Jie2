#!/usr/bin/env python3
"""生成确定性测试 PNG（验证平台的 PNG 解码器与门户截图链路）。

像素规则：R = (x*10) % 256，G = (y*10) % 256，B = 128，A = 255。
  每行用**不同的 PNG 行滤波器**（0..4 轮转），覆盖解码器的全部反滤波分支 ——
  只测 filter=0 的图会漏掉 Sub/Up/Average/Paeth 里任何实现错误。

用法：python3 make-test-png.py <输出路径> [宽=64] [高=48]
"""
import struct
import sys
import zlib


def chunk(tag: bytes, data: bytes) -> bytes:
    c = struct.pack(">I", len(data)) + tag + data
    return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def pixel(x: int, y: int):
    return ((x * 10) % 256, (y * 10) % 256, 128, 255)


def main() -> int:
    out_path = sys.argv[1]
    width = int(sys.argv[2]) if len(sys.argv) > 2 else 64
    height = int(sys.argv[3]) if len(sys.argv) > 3 else 48

    bpp = 4
    raw = bytearray()
    prev = [0] * (width * bpp)
    for y in range(height):
        row = []
        for x in range(width):
            row.extend(pixel(x, y))
        filter_type = y % 5
        filtered = bytearray()
        for i, b in enumerate(row):
            a = row[i - bpp] if i >= bpp else 0
            up = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if filter_type == 0:
                f = b
            elif filter_type == 1:
                f = (b - a) & 0xFF
            elif filter_type == 2:
                f = (b - up) & 0xFF
            elif filter_type == 3:
                f = (b - (a + up) // 2) & 0xFF
            else:
                p = a + up - c
                pa, pb, pc = abs(p - a), abs(p - up), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (up if pb <= pc else c)
                f = (b - pred) & 0xFF
            filtered.append(f)
        raw.append(filter_type)
        raw += filtered
        prev = row

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(out_path, "wb") as fh:
        fh.write(png)
    print("已生成 %dx%d 测试 PNG（5 种行滤波器轮转）: %s" % (
        width, height, out_path))
    return 0


if __name__ == "__main__":
    sys.exit(main())
