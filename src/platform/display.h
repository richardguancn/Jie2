#pragma once

#include <string>
#include <vector>

#include "core/geometry.h"
#include "core/image_buffer.h"

// 「截2」平台集成层 — 显示器与抓屏类型
//
// 坐标约定（全项目统一，务必遵守）：
//   - 显示器 bounds 与抓屏区域都使用**虚拟桌面坐标**（全局坐标）。
//   - 主显示器左上角为原点 (0,0)；多屏时其它屏幕可能为负坐标或更大坐标。
//   - 坐标单位是**显示器坐标空间的自然单位**，不是物理像素：
//       macOS：逻辑点。CGDisplayBounds 与 CGDisplayCreateImageForRect 都在
//              这个空间里工作（实测 macOS 27：主屏 bounds 1710x1107 逻辑点，
//              传 1710x1107 进去得到 3420x2214 物理像素的图）。
//       Linux X11：无独立逻辑空间，等于物理像素。
//     物理像素只体现在**抓屏返回的图像尺寸**上：HiDPI 屏上
//     图像尺寸 = 逻辑尺寸 × scaleFactor。
//   - 需要物理像素尺寸或做像素级换算时，用 DisplayInfo::scaleFactor。
//   - 注意 CGDisplayPixelsWide 在 macOS 27 上返回逻辑点（不是物理像素），
//     推断缩放比例要用 CGDisplayModeGetPixelWidth / GetWidth。

namespace jie2::platform {

struct DisplayInfo {
    std::string id;          // 平台内稳定标识（macOS: display ID；X11: 输出名）
    std::string name;        // 人类可读名称
    core::Rect bounds;       // 虚拟桌面坐标下的物理像素范围
    double scaleFactor = 1.0;  // 逻辑点 → 物理像素的比例（HiDPI）
    bool isPrimary = false;
};

/// 抓屏结果。失败时 error 必填 —— 不静默返回空图。
struct CaptureResult {
    bool ok = false;
    core::ImageBuffer image;
    std::string error;

    static CaptureResult failure(std::string reason) {
        CaptureResult result;
        result.ok = false;
        result.error = std::move(reason);
        return result;
    }
    static CaptureResult success(core::ImageBuffer buffer) {
        CaptureResult result;
        result.ok = true;
        result.image = std::move(buffer);
        return result;
    }
};

} // namespace jie2::platform
