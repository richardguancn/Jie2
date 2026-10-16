#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "core/image_buffer.h"

// 「截2」平台层 — PNG 解码（精简版）
//
// 为什么不放 core 层：
//   core 的硬约束是"只依赖 C++ 标准库"（见 src/core/CMakeLists.txt），
//   而本解码器依赖 zlib。它属于"平台形态 → core 形态"的转换器，
//   归平台层是恰当的定位。
//
// 为什么需要它：
//   Wayland 的截图门户（xdg-desktop-portal Screenshot）返回的是
//   **PNG 文件**（file:// URI），而图像内核以 RGBA 像素工作。
//   平台层不依赖 Qt，所以这里要有一个最小、可控的 PNG 解码器，
//   而不是把解码偷偷塞进 UI 层。
//
// 支持范围（门户截图产出的实际形态）：
//   - 8-bit 位深；颜色类型 0/2/4/6（灰度 / RGB / 灰度+Alpha / RGBA）
//   - 非隔行（interlace=0）；无调色板
// 不支持：16-bit、调色板、隔行 —— 明确返回错误（不猜、不静默出坏图）。

namespace jie2::platform {

/// 解码 PNG 二进制为 RGBA8888 图像。失败时 error 必填。
bool decodePng(const std::uint8_t* data, std::size_t size,
               core::ImageBuffer* out, std::string* error);

} // namespace jie2::platform
