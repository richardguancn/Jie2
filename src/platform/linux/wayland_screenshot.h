#pragma once

#include <string>

#include "core/image_buffer.h"

// 「J2」平台层 — Wayland 截图门户（xdg-desktop-portal Screenshot）
//
// 与 GlobalShortcuts（wayland_portal.h）不同，截图是**一次性请求-响应**：
//   调用 Screenshot → 等 Response → 读回 PNG 文件 → 解码 → 释放连接。
// 因此这里用独立的短生命周期 D-Bus 连接、在调用线程同步完成，
// 不需要常驻工作线程（也就不存在多线程碰连接的问题）。
//
// 已知平台限制（评估报告 3.2 已核实）：
//   interactive=false 时，GNOME 仍会弹一次授权提示；KDE 等可能直接截图。
//   这是 portal 的安全模型决定的，不是产品缺陷 —— 失败/超时如实报告。

namespace jie2::platform {

class WaylandScreenshotPortal {
public:
    /// 门户的 Screenshot 接口是否可用（服务名与接口版本真实查询）。
    static bool serviceAvailable(std::string* detail);

    /// 抓取全屏（非交互模式），等待用户/门户响应的超时可配。
    /// 成功后 out 为解码后的 RGBA 图像；失败时 error 必填。
    bool captureFullscreen(core::ImageBuffer* out, std::string* error,
                           int timeoutMs = 60000) const;
};

} // namespace jie2::platform
