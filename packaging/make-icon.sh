#!/usr/bin/env bash
# 「截2」应用图标：使用已校验的导出文件，常用尺寸不需要额外依赖。
# 用法：bash packaging/make-icon.sh <输出路径.png> [尺寸]
set -eu
OUT="${1:?用法: make-icon.sh <out.png> [size]}"
SIZE="${2:-256}"
case "$SIZE" in ''|*[!0-9]*|0) echo '尺寸必须为正整数' >&2; exit 1 ;; esac
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$(dirname "$OUT")"
SOURCE="$ROOT/assets/brand/app/icon-$SIZE.png"
if [ -f "$SOURCE" ]; then
    cp "$SOURCE" "$OUT"
elif command -v rsvg-convert >/dev/null 2>&1; then
    rsvg-convert -w "$SIZE" -h "$SIZE" "$ROOT/assets/brand/app-icon.svg" -o "$OUT"
elif command -v sips >/dev/null 2>&1; then
    sips -z "$SIZE" "$SIZE" "$ROOT/assets/brand/app/icon-1024.png" --out "$OUT" >/dev/null
else
    echo "此尺寸需安装 rsvg-convert；现有尺寸：16 32 64 128 256 512 1024" >&2
    exit 1
fi
printf 'icon written: %s (%sx%s)\n' "$OUT" "$SIZE" "$SIZE"
