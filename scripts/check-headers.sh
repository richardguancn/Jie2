#!/usr/bin/env bash
# 「截2」头文件自包含检查
#
# 为什么需要这个脚本：
#   一次真实事故 —— clothesline.h 的 signals 里用了 QString 但没 include <QString>，
#   在 gcc 下引发级联解析错误（QObject 被解析成类内嵌套名）。而增量构建在
#   时间戳不巧时**不会重编**该文件，于是"构建看起来成功、测试跑的是旧二进制"，
#   白白浪费了很长一段排查时间。
#
# 做法：对每个头文件单独做语法检查（-fsyntax-only），任何头都必须
# 能在"不依赖其它头先被包含"的前提下通过编译。
# 并行执行（-P）：串行时 6 个 Qt 头要 90 秒以上，曾把回归项拖到超时。
#
# 用法（容器内，需已装 qt6-base-dev）：bash scripts/check-headers.sh
set -u

QT_INC=$(pkg-config --cflags Qt6Widgets Qt6Network 2>/dev/null || true)
if [ -z "$QT_INC" ]; then
    # 退化路径：直接给常见 include 根
    QT_INC="-I/usr/include/aarch64-linux-gnu/qt6 -I/usr/include/aarch64-linux-gnu/qt6/QtCore -I/usr/include/aarch64-linux-gnu/qt6/QtGui -I/usr/include/aarch64-linux-gnu/qt6/QtWidgets -I/usr/include/aarch64-linux-gnu/qt6/QtNetwork"
fi

cd "$(dirname "$0")/.." || exit 1
SRC_ROOT="$(pwd)/src"

check_one() {
    local header="$1"
    local err_file
    err_file="$(mktemp)"
    if g++ -fsyntax-only -std=c++20 -fPIC $QT_INC -I"$SRC_ROOT" "$header" 2>"$err_file"; then
        echo "ok:   $header"
        rm -f "$err_file"
        return 0
    fi
    echo "FAIL: $header 不是自包含的"
    head -6 "$err_file" | sed 's/^/      /'
    rm -f "$err_file"
    return 1
}
export -f check_one
export QT_INC SRC_ROOT

# 并行检查（-P 4 对容器 CPU 数是合理折中；输出经 sort 稳定顺序）。
results=$(printf '%s\n' src/ui/*.h | sort | xargs -P 4 -I{} bash -c 'check_one "{}"')

echo "$results"

if echo "$results" | grep -q "^FAIL"; then
    echo
    echo "存在不自包含的头文件 —— 请补 include 后重试。"
    exit 1
fi
echo
echo "全部头文件自包含检查通过。"
