#pragma once

#include <string>

// 「截2」平台能力描述
//
// 这一组类型是平台集成层与 UI 层之间的契约：
//   平台层负责"探测出真实状态"，UI 层负责"把受限状态如实告诉用户"。
// 评估报告 3.2 要求 Wayland 降级必须对用户可见，本文件是那件事的类型基础。

namespace jie2::core {

/// 当前所处的桌面会话类型。决定后续所有能力的可用性判断。
enum class SessionType {
    Unknown,
    MacOS,
    X11,
    Wayland,
    Windows,
};

const char* toString(SessionType type);

/// 单项能力的可用状态。
enum class CapabilityStatus {
    Unknown,        // 尚未探测
    Available,      // 直接可用，无需额外授权
    Degraded,       // 可用但受限（需要用户授权 / 每次弹窗 / 功能缩水）
    Unavailable,    // 当前会话下不可用
    NotApplicable,  // 该平台不存在此概念（例如 macOS 没有 layer-shell）
};

const char* toString(CapabilityStatus status);

struct Capability {
    CapabilityStatus status = CapabilityStatus::Unknown;
    std::string detail;  // 面向用户的一句话说明；受限时必须说明原因

    bool ok() const { return status == CapabilityStatus::Available; }
};

/// 平台能力矩阵。
struct PlatformCapabilities {
    SessionType session = SessionType::Unknown;
    std::string sessionDetail;   // 桌面 / 合成器名称等补充信息

    Capability screenCapture;    // 抓屏
    Capability globalHotkey;     // 全局热键
    Capability alwaysOnTop;      // 屏幕置顶窗口（贴图）
    Capability layerShell;       // 顶部透明条（衣架）
    Capability clipboardImage;   // 剪贴板图像读写

    /// 是否存在任何会让体验弱于对标的受限项（用于启动时提示用户）。
    bool hasLimitations() const;

    /// 生成可读的多行摘要（诊断 / 菜单展示用）。
    std::string describe() const;
};

} // namespace jie2::core
