#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/geometry.h"

// 「截2」图像内核 — 像素缓冲
// 约束：不依赖任何 UI 框架，不持有平台句柄。所有算法以此结构为输入输出。

namespace jie2::core {

enum class PixelFormat {
    RGBA8888,  // 内存序 R,G,B,A（macOS / Qt 常用）
    BGRA8888,  // 内存序 B,G,R,A（X11 抓屏返回）
};

struct Rgba {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
    friend constexpr bool operator==(const Rgba&, const Rgba&) = default;
};

/// 行优先、四通道、无行填充的像素缓冲。
///
/// 之所以固定为 4 通道紧密排布：截图工具的中间产物与标注器都按 RGBA 工作，
/// 保持单一布局可以避免每条链路都做格式协商。
class ImageBuffer {
public:
    ImageBuffer() = default;
    ImageBuffer(int width, int height, PixelFormat format = PixelFormat::RGBA8888);

    int width() const { return width_; }
    int height() const { return height_; }
    Size size() const { return {width_, height_}; }
    PixelFormat format() const { return format_; }
    bool isEmpty() const { return width_ <= 0 || height_ <= 0; }

    std::size_t stride() const { return static_cast<std::size_t>(width_) * 4; }
    std::size_t byteSize() const { return stride() * static_cast<std::size_t>(height_); }

    std::uint8_t* data() { return bytes_.data(); }
    const std::uint8_t* data() const { return bytes_.data(); }

    std::uint8_t* scanline(int y) {
        return bytes_.data() + static_cast<std::size_t>(y) * stride();
    }
    const std::uint8_t* scanline(int y) const {
        return bytes_.data() + static_cast<std::size_t>(y) * stride();
    }

    /// 取指定像素（越界返回透明黑）。按 format() 解释通道，输出统一为 RGBA。
    Rgba pixel(int x, int y) const;

    /// 以统一颜色构造缓冲。
    static ImageBuffer filled(int width, int height, Rgba color);

private:
    int width_ = 0;
    int height_ = 0;
    PixelFormat format_ = PixelFormat::RGBA8888;
    std::vector<std::uint8_t> bytes_;
};

} // namespace jie2::core
