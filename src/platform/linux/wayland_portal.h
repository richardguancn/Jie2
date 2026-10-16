#pragma once

#include <memory>
#include <string>

#include "platform/hotkey.h"

// 「截2」Linux/Wayland 全局热键 —— xdg-desktop-portal GlobalShortcuts 门户
//
// 为什么必须走门户（评估报告 3.2）：
//   Wayland 没有全局热键的核心协议，应用无法自己"抢"按键。
//   门户（portal）是 Wayland 下唯一合规路径：由桌面环境代持绑定，
//   用户在系统设置里可见、可改（GNOME 48+ / KDE Plasma 6.1+ 已支持）。
//
// 实现约束（三层解耦）：
//   本文件属于 platform 层，**不得依赖 Qt**。D-Bus 用 libdbus-1 直连。
//
// 线程模型：
//   本类持有专属 D-Bus 工作线程。所有 public 方法可跨线程调用；
//   热键回调在 D-Bus 线程触发 —— 与 X11 后端同一约定
//   （见 platform.h：UI 层负责把回调投递回自己的主循环）。

namespace jie2::platform {

class WaylandPortalHotkeys {
public:
    WaylandPortalHotkeys();
    ~WaylandPortalHotkeys();

    WaylandPortalHotkeys(const WaylandPortalHotkeys&) = delete;
    WaylandPortalHotkeys& operator=(const WaylandPortalHotkeys&) = delete;

    /// 门户服务是否可用（会话总线上 org.freedesktop.portal.Desktop 是否有属主，
    /// 且 GlobalShortcuts 接口是否可自省）。
    /// 用于能力探测：不可用时如实报告，不假装能绑定。
    /// detail 可选，返回诊断信息（版本号或失败原因）。
    static bool serviceAvailable(std::string* detail);

    /// 绑定一个热键。
    /// 流程：CreateSession → BindShortcuts（可能弹系统确认框）→ 等 Response。
    /// 超时（默认 8 秒）返回失败并说明原因 —— 不静默成功。
    bool grab(const HotkeySpec& spec, HotkeyCallback callback, void* userData,
              std::string* error);

    /// 关闭门户会话（同时释放该会话内的全部绑定）。
    void releaseAll();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace jie2::platform
