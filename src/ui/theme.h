#pragma once

#include <QString>

// 「J2」主题解析（专业版对齐项 v2.6：主题色跟随系统主题）
//
// 取值来源与优先级：
//   1. 配置显式指定：appearance.theme = dark / light
//   2. 环境变量 JIE2_THEME（自动化测试用：无法在容器里改"系统主题"）
//   3. 系统探测：
//      - Qt ≥ 6.5：QStyleHints::colorScheme()
//      - macOS 兜底：defaults read -g AppleInterfaceStyle
//      - Linux 兜底：gsettings color-scheme / gtk-theme 名称含 dark
//   4. 全部不可用 → 深色（维持既有默认观感）
//
// 返回结构带 source 字段：写进日志，测试与排障都能看到"为什么是这个主题"。

namespace jie2::ui {

struct ThemeResolution {
    bool dark = true;
    QString source;  // "配置" / "环境变量" / "系统" / "默认"
};

/// setting 为配置值（"system" / "dark" / "light" / 空）。
ThemeResolution resolveTheme(const QString& setting);

} // namespace jie2::ui
