#include "platform/linux/png_decode.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#include <zlib.h>

namespace jie2::platform {

namespace {

std::uint32_t readU32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

/// PNG 规定的 Paeth 预测器。
int paethPredictor(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a);
    const int pb = std::abs(p - b);
    const int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) {
        return a;
    }
    if (pb <= pc) {
        return b;
    }
    return c;
}

/// 通道数（按颜色类型）。
int channelsOf(int colorType) {
    switch (colorType) {
        case 0: return 1;  // 灰度
        case 2: return 3;  // RGB
        case 4: return 2;  // 灰度 + Alpha
        case 6: return 4;  // RGBA
        default: return 0;
    }
}

/// 解压 zlib 流到预期大小的缓冲区。
bool inflateExact(const std::vector<std::uint8_t>& compressed,
                  std::size_t expected, std::vector<std::uint8_t>* out,
                  std::string* error) {
    out->resize(expected);
    uLongf destLen = static_cast<uLongf>(expected);
    const int rc = ::uncompress(out->data(), &destLen, compressed.data(),
                                static_cast<uLong>(compressed.size()));
    if (rc != Z_OK) {
        *error = std::string("PNG zlib 解压失败（zlib 错误码 ") +
                 std::to_string(rc) + "）";
        return false;
    }
    if (destLen != expected) {
        *error = "PNG 解压后大小与预期不符（预期 " + std::to_string(expected) +
                 "，实际 " + std::to_string(destLen) + "）";
        return false;
    }
    return true;
}

} // namespace

bool decodePng(const std::uint8_t* data, std::size_t size,
               core::ImageBuffer* out, std::string* error) {
    const auto fail = [error](const std::string& reason) {
        if (error != nullptr) {
            *error = reason;
        }
        return false;
    };

    static const std::uint8_t kSignature[8] = {0x89, 0x50, 0x4E, 0x47,
                                               0x0D, 0x0A, 0x1A, 0x0A};
    if (size < 8 || std::memcmp(data, kSignature, 8) != 0) {
        return fail("不是 PNG 数据（文件签名不匹配）");
    }
    if (out == nullptr) {
        return fail("输出缓冲为空");
    }

    int width = 0;
    int height = 0;
    int bitDepth = 0;
    int colorType = 0;
    int interlace = 0;
    std::vector<std::uint8_t> idatChunks;
    bool seenIhdr = false;

    std::size_t offset = 8;
    while (offset + 12 <= size) {
        const std::uint32_t length = readU32(data + offset);
        const std::uint8_t* type = data + offset + 4;
        const std::uint8_t* payload = data + offset + 8;
        if (offset + 12 + static_cast<std::size_t>(length) > size) {
            return fail("PNG 数据截断（chunk 越界）");
        }

        if (std::memcmp(type, "IHDR", 4) == 0 && length >= 13) {
            width = static_cast<int>(readU32(payload));
            height = static_cast<int>(readU32(payload + 4));
            bitDepth = payload[8];
            colorType = payload[9];
            interlace = payload[12];
            seenIhdr = true;
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            idatChunks.insert(idatChunks.end(), payload, payload + length);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            break;
        }
        // 其它 chunk（pHYs / tEXt / ...）跳过。

        offset += 12 + static_cast<std::size_t>(length);
    }

    if (!seenIhdr || width <= 0 || height <= 0) {
        return fail("PNG 缺少有效的 IHDR（宽高无效）");
    }
    if (bitDepth != 8) {
        return fail("不支持的 PNG 位深：" + std::to_string(bitDepth) +
                    "（仅支持 8-bit）");
    }
    const int channels = channelsOf(colorType);
    if (channels == 0) {
        return fail("不支持的 PNG 颜色类型：" + std::to_string(colorType) +
                    "（仅支持灰度/RGB/灰度+Alpha/RGBA；调色板不支持）");
    }
    if (interlace != 0) {
        return fail("不支持的 PNG 隔行模式（仅支持非隔行）");
    }
    if (idatChunks.empty()) {
        return fail("PNG 没有 IDAT 数据");
    }

    // 反滤波后的扫描线总大小：每行 1 字节过滤器 + width*channels 字节。
    const std::size_t rowBytes = static_cast<std::size_t>(width) * channels;
    const std::size_t expected =
        static_cast<std::size_t>(height) * (1 + rowBytes);
    std::vector<std::uint8_t> raw;
    if (!inflateExact(idatChunks, expected, &raw, error)) {
        return false;
    }

    // 逐行反滤波（标准 PNG 五种过滤器）。
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(height) * rowBytes);
    const int bpp = channels;  // 8-bit：1 字节/通道
    for (int y = 0; y < height; ++y) {
        const std::uint8_t filter =
            raw[static_cast<std::size_t>(y) * (1 + rowBytes)];
        const std::uint8_t* src =
            raw.data() + static_cast<std::size_t>(y) * (1 + rowBytes) + 1;
        std::uint8_t* dst =
            pixels.data() + static_cast<std::size_t>(y) * rowBytes;
        const std::uint8_t* prev =
            (y > 0)
                ? pixels.data() + static_cast<std::size_t>(y - 1) * rowBytes
                : nullptr;

        for (std::size_t x = 0; x < rowBytes; ++x) {
            const int rawByte = src[x];
            const int left =
                (x >= static_cast<std::size_t>(bpp)) ? dst[x - bpp] : 0;
            const int up = (prev != nullptr) ? prev[x] : 0;
            const int upLeft =
                (prev != nullptr && x >= static_cast<std::size_t>(bpp))
                    ? prev[x - bpp]
                    : 0;
            int value = 0;
            switch (filter) {
                case 0: value = rawByte; break;
                case 1: value = rawByte + left; break;
                case 2: value = rawByte + up; break;
                case 3: value = rawByte + (left + up) / 2; break;
                case 4: value = rawByte + paethPredictor(left, up, upLeft); break;
                default:
                    return fail("PNG 未知的行过滤器：" + std::to_string(filter));
            }
            dst[x] = static_cast<std::uint8_t>(value & 0xFF);
        }
    }

    // 组装 RGBA8888。
    core::ImageBuffer image(width, height, core::PixelFormat::RGBA8888);
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* src =
            pixels.data() + static_cast<std::size_t>(y) * rowBytes;
        std::uint8_t* dst = image.scanline(y);
        for (int x = 0; x < width; ++x) {
            const std::uint8_t* px = src + static_cast<std::size_t>(x) * channels;
            std::uint8_t* outPx = dst + static_cast<std::size_t>(x) * 4;
            switch (colorType) {
                case 0:  // 灰度
                    outPx[0] = outPx[1] = outPx[2] = px[0];
                    outPx[3] = 255;
                    break;
                case 2:  // RGB
                    outPx[0] = px[0];
                    outPx[1] = px[1];
                    outPx[2] = px[2];
                    outPx[3] = 255;
                    break;
                case 4:  // 灰度 + Alpha
                    outPx[0] = outPx[1] = outPx[2] = px[0];
                    outPx[3] = px[1];
                    break;
                case 6:  // RGBA
                    outPx[0] = px[0];
                    outPx[1] = px[1];
                    outPx[2] = px[2];
                    outPx[3] = px[3];
                    break;
                default:
                    break;  // channelsOf 已确保不会到这
            }
        }
    }

    *out = std::move(image);
    return true;
}

} // namespace jie2::platform
