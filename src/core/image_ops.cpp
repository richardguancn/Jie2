#include "core/image_ops.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace jie2::core {

ImageBuffer crop(const ImageBuffer& source, const Rect& region) {
    const Rect clipped = region.intersected(Rect{0, 0, source.width(), source.height()});
    if (clipped.isEmpty()) {
        return ImageBuffer{};
    }
    ImageBuffer out(clipped.width, clipped.height, source.format());
    const std::size_t rowBytes = static_cast<std::size_t>(clipped.width) * 4;
    for (int y = 0; y < clipped.height; ++y) {
        const std::uint8_t* srcRow = source.scanline(clipped.y + y) +
                                     static_cast<std::size_t>(clipped.x) * 4;
        std::uint8_t* dstRow = out.scanline(y);
        std::copy(srcRow, srcRow + rowBytes, dstRow);
    }
    return out;
}

void blit(ImageBuffer& dst, const ImageBuffer& src, Point topLeft) {
    if (dst.isEmpty() || src.isEmpty()) {
        return;
    }
    const Rect target{topLeft.x, topLeft.y, src.width(), src.height()};
    const Rect clipped = target.intersected(Rect{0, 0, dst.width(), dst.height()});
    if (clipped.isEmpty()) {
        return;
    }
    const int srcX = clipped.x - topLeft.x;
    const int srcY = clipped.y - topLeft.y;
    const std::size_t rowBytes = static_cast<std::size_t>(clipped.width) * 4;
    for (int y = 0; y < clipped.height; ++y) {
        const std::uint8_t* srcRow =
            src.scanline(srcY + y) + static_cast<std::size_t>(srcX) * 4;
        std::uint8_t* dstRow =
            dst.scanline(clipped.y + y) + static_cast<std::size_t>(clipped.x) * 4;
        std::copy(srcRow, srcRow + rowBytes, dstRow);
    }
}

ImageBuffer scaleNearest(const ImageBuffer& source, int width, int height) {
    if (source.isEmpty() || width <= 0 || height <= 0) {
        return ImageBuffer{};
    }
    ImageBuffer out(width, height, source.format());
    for (int y = 0; y < height; ++y) {
        const int srcY = std::min(source.height() - 1, y * source.height() / height);
        for (int x = 0; x < width; ++x) {
            const int srcX = std::min(source.width() - 1, x * source.width() / width);
            const std::uint8_t* sp = source.scanline(srcY) + static_cast<std::size_t>(srcX) * 4;
            std::uint8_t* dp = out.scanline(y) + static_cast<std::size_t>(x) * 4;
            dp[0] = sp[0];
            dp[1] = sp[1];
            dp[2] = sp[2];
            dp[3] = sp[3];
        }
    }
    return out;
}

PixelDiff comparePixels(const ImageBuffer& a, const ImageBuffer& b) {
    PixelDiff diff;
    if (a.width() != b.width() || a.height() != b.height()) {
        diff.maxChannelDelta = 255;
        diff.differingPixels = static_cast<std::size_t>(a.width()) * a.height() +
                               static_cast<std::size_t>(b.width()) * b.height();
        return diff;
    }
    for (int y = 0; y < a.height(); ++y) {
        const std::uint8_t* ra = a.scanline(y);
        const std::uint8_t* rb = b.scanline(y);
        for (int x = 0; x < a.width(); ++x) {
            bool differs = false;
            for (int c = 0; c < 4; ++c) {
                const int delta = std::abs(static_cast<int>(ra[x * 4 + c]) -
                                           static_cast<int>(rb[x * 4 + c]));
                if (delta > diff.maxChannelDelta) {
                    diff.maxChannelDelta = delta;
                }
                if (delta != 0) {
                    differs = true;
                }
            }
            if (differs) {
                ++diff.differingPixels;
            }
        }
    }
    return diff;
}

namespace {

/// 对单通道平面做一次横向 box blur（半径 r，滑动窗口）。
/// 输入是 float 平面，避免多次迭代的累积量化误差。
void blurPlaneHorizontal(std::vector<float>& plane, int width, int height, int r) {
    if (r <= 0 || width <= 0 || height <= 0) {
        return;
    }
    std::vector<float> out(plane.size(), 0.0f);
    const float norm = 1.0f / static_cast<float>(2 * r + 1);
    for (int y = 0; y < height; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * width;
        // 初始窗口 [0, r]（左侧越界部分用边界像素补齐）
        float sum = 0.0f;
        for (int k = -r; k <= r; ++k) {
            const int x = std::clamp(k, 0, width - 1);
            sum += plane[row + static_cast<std::size_t>(x)];
        }
        for (int x = 0; x < width; ++x) {
            out[row + static_cast<std::size_t>(x)] = sum * norm;
            const int addX = std::clamp(x + r + 1, 0, width - 1);
            const int subX = std::clamp(x - r, 0, width - 1);
            sum += plane[row + static_cast<std::size_t>(addX)] -
                   plane[row + static_cast<std::size_t>(subX)];
        }
    }
    plane.swap(out);
}

void blurPlaneVertical(std::vector<float>& plane, int width, int height, int r) {
    if (r <= 0 || width <= 0 || height <= 0) {
        return;
    }
    std::vector<float> out(plane.size(), 0.0f);
    const float norm = 1.0f / static_cast<float>(2 * r + 1);
    for (int x = 0; x < width; ++x) {
        float sum = 0.0f;
        for (int k = -r; k <= r; ++k) {
            const int y = std::clamp(k, 0, height - 1);
            sum += plane[static_cast<std::size_t>(y) * width + x];
        }
        for (int y = 0; y < height; ++y) {
            out[static_cast<std::size_t>(y) * width + x] = sum * norm;
            const int addY = std::clamp(y + r + 1, 0, height - 1);
            const int subY = std::clamp(y - r, 0, height - 1);
            sum += plane[static_cast<std::size_t>(addY) * width + x] -
                   plane[static_cast<std::size_t>(subY) * width + x];
        }
    }
    plane.swap(out);
}

namespace {

/// 椭圆掩码判定：点 (x,y) 是否落在以 region 为外接矩形的椭圆内。
/// 定义在文件前部：blurRegionInPlace 与 pixelate 都要用。
bool insideEllipse(const Rect& region, int x, int y) {
    const double rx = region.width / 2.0;
    const double ry = region.height / 2.0;
    if (rx <= 0.0 || ry <= 0.0) {
        return false;
    }
    const double dx = (x + 0.5) - (region.x + rx);
    const double dy = (y + 0.5) - (region.y + ry);
    return (dx * dx) / (rx * rx) + (dy * dy) / (ry * ry) <= 1.0;
}

} // namespace

/// 对 RGBA 缓冲的指定区域做三次 box blur 近似高斯；区域外不动。
/// ellipseMask 为 true 时只把结果写回椭圆内像素（椭圆由 region 外接），
/// 但模糊本身仍按整个矩形计算 —— 这样椭圆边界处的模糊是连续的，
/// 不会出现"边界内侧没有邻域数据"的暗边。
void blurRegionInPlace(ImageBuffer& buffer, const Rect& region, int radius,
                       bool ellipseMask = false) {
    if (region.isEmpty() || radius <= 0) {
        return;
    }
    const int w = region.width;
    const int h = region.height;
    const std::size_t count = static_cast<std::size_t>(w) * h;

    // 拆四个通道平面（只在区域内）
    std::vector<float> planes[4];
    for (auto& plane : planes) {
        plane.resize(count);
    }
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* row = buffer.scanline(region.y + y) +
                                  static_cast<std::size_t>(region.x) * 4;
        for (int x = 0; x < w; ++x) {
            const std::size_t idx = static_cast<std::size_t>(y) * w + x;
            for (int c = 0; c < 4; ++c) {
                planes[c][idx] = static_cast<float>(row[x * 4 + c]);
            }
        }
    }

    // 三次 box blur ≈ 高斯（半径按 1/3 缩放以保持总扩散量）
    const int r = std::max(1, radius / 3 + (radius % 3 != 0 ? 1 : 0));
    for (int pass = 0; pass < 3; ++pass) {
        for (auto& plane : planes) {
            blurPlaneHorizontal(plane, w, h, r);
            blurPlaneVertical(plane, w, h, r);
        }
    }

    // 写回
    for (int y = 0; y < h; ++y) {
        std::uint8_t* row = buffer.scanline(region.y + y) +
                            static_cast<std::size_t>(region.x) * 4;
        for (int x = 0; x < w; ++x) {
            if (ellipseMask && !insideEllipse(region, region.x + x, region.y + y)) {
                continue;
            }
            const std::size_t idx = static_cast<std::size_t>(y) * w + x;
            for (int c = 0; c < 4; ++c) {
                const float v = std::clamp(planes[c][idx], 0.0f, 255.0f);
                row[x * 4 + c] = static_cast<std::uint8_t>(v + 0.5f);
            }
        }
    }
}

} // namespace

ImageBuffer addBorder(const ImageBuffer& source, int width, Rgba color) {
    if (source.isEmpty() || width <= 0) {
        return source;
    }
    ImageBuffer out(source.width() + 2 * width, source.height() + 2 * width,
                    source.format());
    // 先铺满边框色
    for (int y = 0; y < out.height(); ++y) {
        std::uint8_t* row = out.scanline(y);
        for (int x = 0; x < out.width(); ++x) {
            std::uint8_t* p = row + static_cast<std::size_t>(x) * 4;
            p[0] = color.r;
            p[1] = color.g;
            p[2] = color.b;
            p[3] = color.a;
        }
    }
    blit(out, source, Point{width, width});
    return out;
}

ImageBuffer addShadow(const ImageBuffer& source, const ShadowSpec& spec) {
    if (source.isEmpty()) {
        return source;
    }
    const int blur = std::max(0, spec.blurRadius);
    const int pad = blur * 2 + std::abs(spec.offsetX) + std::abs(spec.offsetY);
    ImageBuffer out(source.width() + 2 * pad, source.height() + 2 * pad,
                    source.format());

    // 1) 阴影层：源图 alpha 放进右下偏移的位置，整层做模糊
    ImageBuffer shadow(out.width(), out.height(), source.format());
    {
        // alpha 平面（源图尺寸 + 偏移），直接构造为 float 处理
        std::vector<float> alpha(static_cast<std::size_t>(out.width()) * out.height(), 0.0f);
        const int baseX = pad + spec.offsetX;
        const int baseY = pad + spec.offsetY;
        for (int y = 0; y < source.height(); ++y) {
            const std::uint8_t* row = source.scanline(y);
            for (int x = 0; x < source.width(); ++x) {
                const int dx = baseX + x;
                const int dy = baseY + y;
                if (dx < 0 || dy < 0 || dx >= out.width() || dy >= out.height()) {
                    continue;
                }
                alpha[static_cast<std::size_t>(dy) * out.width() + dx] =
                    static_cast<float>(row[x * 4 + 3]);
            }
        }
        if (blur > 0) {
            const int r = std::max(1, blur / 3 + (blur % 3 != 0 ? 1 : 0));
            for (int pass = 0; pass < 3; ++pass) {
                blurPlaneHorizontal(alpha, out.width(), out.height(), r);
                blurPlaneVertical(alpha, out.width(), out.height(), r);
            }
        }
        // 写成阴影色 × 模糊后的 alpha
        for (int y = 0; y < out.height(); ++y) {
            std::uint8_t* row = shadow.scanline(y);
            for (int x = 0; x < out.width(); ++x) {
                const float a = std::clamp(
                    alpha[static_cast<std::size_t>(y) * out.width() + x], 0.0f, 255.0f);
                std::uint8_t* p = row + static_cast<std::size_t>(x) * 4;
                p[0] = spec.color.r;
                p[1] = spec.color.g;
                p[2] = spec.color.b;
                p[3] = static_cast<std::uint8_t>(
                    a * (static_cast<float>(spec.color.a) / 255.0f) + 0.5f);
            }
        }
    }

    // 2) 把阴影画到输出，再叠加源图（源图在上）
    blit(out, shadow, Point{0, 0});
    blit(out, source, Point{pad, pad});
    return out;
}

ImageBuffer roundCorners(const ImageBuffer& source, int radius) {
    if (source.isEmpty() || radius <= 0) {
        return source;
    }
    ImageBuffer out = source;  // 拷贝，改 alpha
    const int r = std::min(radius, std::min(source.width(), source.height()) / 2);
    if (r <= 0) {
        return out;
    }
    // 四个角的覆盖计算：到圆心距离决定 alpha（距离场抗锯齿）。
    // 圆心在 (±r, ±r) 处，signX/signY 决定取哪个角的像素。
    auto applyCorner = [&](int signX, int signY) {
        for (int y = 0; y < r; ++y) {
            for (int x = 0; x < r; ++x) {
                const float dx = static_cast<float>(r - x) - 0.5f;
                const float dy = static_cast<float>(r - y) - 0.5f;
                const float dist = std::sqrt(dx * dx + dy * dy);
                // dist > r 时在圆外
                const float coverage = std::clamp((static_cast<float>(r) - dist) + 0.5f,
                                                  0.0f, 1.0f);
                const int px = signX > 0 ? (source.width() - 1 - x) : x;
                const int py = signY > 0 ? (source.height() - 1 - y) : y;
                std::uint8_t* p = out.scanline(py) + static_cast<std::size_t>(px) * 4;
                p[3] = static_cast<std::uint8_t>(
                    static_cast<float>(p[3]) * coverage + 0.5f);
            }
        }
    };
    applyCorner(-1, -1);
    applyCorner(+1, -1);
    applyCorner(-1, +1);
    applyCorner(+1, +1);
    return out;
}

ImageBuffer pixelate(const ImageBuffer& source, const Rect& region, int blockSize,
                     bool ellipseMask) {
    if (source.isEmpty() || blockSize <= 1) {
        return source;
    }
    ImageBuffer out = source;
    const Rect clipped = region.intersected(Rect{0, 0, source.width(), source.height()});
    if (clipped.isEmpty()) {
        return out;
    }
    for (int by = clipped.y; by < clipped.bottom(); by += blockSize) {
        for (int bx = clipped.x; bx < clipped.right(); bx += blockSize) {
            const int ex = std::min(bx + blockSize, clipped.right());
            const int ey = std::min(by + blockSize, clipped.bottom());
            // 块内平均（含 alpha 加权，避免透明块混入错误颜色）。
            // 椭圆模式下：块与椭圆完全不相交时跳过；
            // 相交时只统计椭圆内的像素（保证椭圆边缘的颜色来源正确）。
            std::uint64_t sum[4] = {0, 0, 0, 0};
            std::uint64_t weight = 0;
            std::uint64_t counted = 0;
            for (int y = by; y < ey; ++y) {
                const std::uint8_t* row = source.scanline(y);
                for (int x = bx; x < ex; ++x) {
                    if (ellipseMask && !insideEllipse(clipped, x, y)) {
                        continue;
                    }
                    const std::uint8_t* p = row + static_cast<std::size_t>(x) * 4;
                    const std::uint64_t a = p[3];
                    sum[0] += static_cast<std::uint64_t>(p[0]) * a;
                    sum[1] += static_cast<std::uint64_t>(p[1]) * a;
                    sum[2] += static_cast<std::uint64_t>(p[2]) * a;
                    sum[3] += a;
                    weight += a;
                    ++counted;
                }
            }
            if (weight == 0 || counted == 0) {
                continue;
            }
            const std::uint8_t avg[4] = {
                static_cast<std::uint8_t>(sum[0] / weight),
                static_cast<std::uint8_t>(sum[1] / weight),
                static_cast<std::uint8_t>(sum[2] / weight),
                static_cast<std::uint8_t>(sum[3] / counted),
            };
            for (int y = by; y < ey; ++y) {
                std::uint8_t* row = out.scanline(y);
                for (int x = bx; x < ex; ++x) {
                    if (ellipseMask && !insideEllipse(clipped, x, y)) {
                        continue;
                    }
                    std::uint8_t* p = row + static_cast<std::size_t>(x) * 4;
                    p[0] = avg[0];
                    p[1] = avg[1];
                    p[2] = avg[2];
                    p[3] = avg[3];
                }
            }
        }
    }
    return out;
}

ImageBuffer boxBlur(const ImageBuffer& source, const Rect& region, int radius,
                    bool ellipseMask) {
    if (source.isEmpty() || radius <= 0) {
        return source;
    }
    ImageBuffer out = source;
    const Rect clipped = region.intersected(Rect{0, 0, source.width(), source.height()});
    if (clipped.isEmpty()) {
        return out;
    }
    blurRegionInPlace(out, clipped, radius, ellipseMask);
    return out;
}

} // namespace jie2::core
