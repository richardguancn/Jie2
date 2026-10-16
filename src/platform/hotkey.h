#pragma once

#include <string>

#include "core/capability.h"

// 「截2」平台集成层 — 全局热键类型
// 约束：本层不依赖 UI 框架。热键回调可能在非主线程触发，
// 调用方（UI 层）负责把回调投递回自己的事件循环。

namespace jie2::platform {

/// 修饰键位。
enum HotkeyModifier : unsigned {
    kModCtrl  = 1u << 0,
    kModAlt   = 1u << 1,
    kModShift = 1u << 2,
    kModSuper = 1u << 3,  // macOS Command / Windows Win / Linux Super
};

/// 平台无关键码（M0 只覆盖常用键；后续按需扩展）。
enum class Key : unsigned {
    Unknown = 0,
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
};

/// 一次热键注册请求。
struct HotkeySpec {
    std::string id;          // 调用方自定义标识，触发时回传
    unsigned modifiers = 0;  // HotkeyModifier 位或
    Key key = Key::Unknown;
};

/// 触发回调。可能在后台线程执行 —— 实现方必须在文档与行为上保持一致。
using HotkeyCallback = void (*)(const std::string& id, void* userData);

/// 把热键渲染成可读文本（"Ctrl+Alt+T"），用于日志与菜单。
std::string describeHotkey(const HotkeySpec& spec);

} // namespace jie2::platform
