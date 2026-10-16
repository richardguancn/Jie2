#include "core/image_buffer.h"

namespace jie2::core {

ImageBuffer::ImageBuffer(int width, int height, PixelFormat format)
    : width_(width > 0 ? width : 0),
      height_(height > 0 ? height : 0),
      format_(format) {
    bytes_.resize(byteSize(), 0);
}

Rgba ImageBuffer::pixel(int x, int y) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) {
        return Rgba{0, 0, 0, 0};
    }
    const std::uint8_t* p = scanline(y) + static_cast<std::size_t>(x) * 4;
    Rgba out;
    switch (format_) {
        case PixelFormat::RGBA8888:
            out = Rgba{p[0], p[1], p[2], p[3]};
            break;
        case PixelFormat::BGRA8888:
            out = Rgba{p[2], p[1], p[0], p[3]};
            break;
    }
    return out;
}

ImageBuffer ImageBuffer::filled(int width, int height, Rgba color) {
    ImageBuffer buffer(width, height, PixelFormat::RGBA8888);
    if (buffer.isEmpty()) {
        return buffer;
    }
    for (int y = 0; y < buffer.height(); ++y) {
        std::uint8_t* row = buffer.scanline(y);
        for (int x = 0; x < buffer.width(); ++x) {
            std::uint8_t* p = row + static_cast<std::size_t>(x) * 4;
            p[0] = color.r;
            p[1] = color.g;
            p[2] = color.b;
            p[3] = color.a;
        }
    }
    return buffer;
}

} // namespace jie2::core
