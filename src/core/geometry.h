#pragma once

// 「J2」图像内核 — 基础几何类型
// 约束：本模块只依赖 C++ 标准库。

namespace jie2::core {

struct Point {
    int x = 0;
    int y = 0;
    friend constexpr bool operator==(const Point& a, const Point& b) {
        return a.x == b.x && a.y == b.y;
    }
    friend constexpr bool operator!=(const Point& a, const Point& b) {
        return !(a == b);
    }
};

struct Size {
    int width = 0;
    int height = 0;
    friend constexpr bool operator==(const Size& a, const Size& b) {
        return a.width == b.width && a.height == b.height;
    }
    friend constexpr bool operator!=(const Size& a, const Size& b) {
        return !(a == b);
    }
};

/// 以左上角为原点的整数矩形（像素坐标，右下开区间）。
struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    constexpr int left() const { return x; }
    constexpr int top() const { return y; }
    constexpr int right() const { return x + width; }
    constexpr int bottom() const { return y + height; }
    constexpr bool isEmpty() const { return width <= 0 || height <= 0; }

    constexpr bool contains(Point p) const {
        return p.x >= x && p.x < right() && p.y >= y && p.y < bottom();
    }

    constexpr Rect intersected(const Rect& other) const {
        const int nx = x > other.x ? x : other.x;
        const int ny = y > other.y ? y : other.y;
        const int nr = right() < other.right() ? right() : other.right();
        const int nb = bottom() < other.bottom() ? bottom() : other.bottom();
        return {nx, ny, nr > nx ? nr - nx : 0, nb > ny ? nb - ny : 0};
    }

    friend constexpr bool operator==(const Rect& a, const Rect& b) {
        return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
    }
    friend constexpr bool operator!=(const Rect& a, const Rect& b) {
        return !(a == b);
    }
};

} // namespace jie2::core
