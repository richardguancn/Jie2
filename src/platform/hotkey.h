#pragma once

#include <string>

#include "core/capability.h"

// 「J2」平台集成层 — 全局热键类型
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
    F13, F14, F15, F16, F17, F18, F19, F20,
    Space, Tab, Return, Escape, Backspace, Delete, Insert, Home, End,
    Left, Right, Up, Down, PageUp, PageDown,
    Minus, Equal, BracketLeft, BracketRight, Backslash, Semicolon,
    Apostrophe, Comma, Period, Slash, Grave,
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

/// 解析热键文本为注册规格（全局快捷键自定义用）。
///
/// 语法：修饰键 + 主键，用 "+" 分隔，大小写不敏感，允许空白。
///   修饰键：ctrl/control、alt/option、shift、cmd/command/super/win/meta
///   （cmd/command/super/win/meta 是同一位 —— macOS 的 Command、
///    Windows 的 Win、Linux 的 Super，用户按本机叫法填写即可）
///   主键：字母、数字、F1–F20、空格、方向键、编辑键和常见符号键
/// 修饰键可省略，必须恰好一个主键；重复修饰键、未知词、缺主键
/// 都算失败（返回 false，不写 *spec）。spec 的 id 字段由调用方填写。
bool parseHotkeyText(const std::string& text, HotkeySpec* spec);

/// 把热键渲染成平台习惯的菜单文本：
/// macOS 用 "Cmd"，Windows 用 "Win"，Linux 用 "Super"，其余同 describeHotkey。
std::string displayHotkey(const HotkeySpec& spec);

/// 全局快捷键的出厂默认文本：修饰键按平台习惯
/// （macOS 用 cmd+alt，其它平台用 ctrl+alt），主键由参数给出（'t' / 'p' / 'c'）。
/// 供 UI 层默认值、配置回退与设置界面占位提示共用，避免多处各写一份。
std::string defaultHotkeyText(char key);

} // namespace jie2::platform
