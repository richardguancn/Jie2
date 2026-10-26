#pragma once

// 「J2」应用数据路径。
//
// 为什么单独成文件：配置、历史、Wayland items 文件必须是**同一个目录**，
// 而 macOS 上的正确目录与 Qt 的 AppConfigLocation 并不一致（见下）。
// 把这些路径集中在一处，避免各处各写一份、迁移时漏掉某个文件。

#include <QString>

namespace jie2::ui {

/// 应用数据目录（配置、截图历史、中间文件都放这里）。
///
/// macOS 用 `~/Library/Application Support/jie2`，不用
/// `QStandardPaths::AppConfigLocation`（= `~/Library/Preferences/jie2`）：
/// Preferences 目录由系统偏好系统管理，未签名的普通程序往里写普通文件
/// 会被系统拒绝（实测：QSaveFile 报 "not writable"，退回临时文件+rename
/// 后报 EPERM "Operation not permitted"），表现为**设置改了保存不下来**。
/// 其它平台沿用 AppConfigLocation（Windows 为 AppData/Roaming，
/// Linux 为 ~/.config），行为不变。
QString appDataDirectory();

/// 配置文件路径（目录内的 jie2.conf）。
QString appConfigFile();

/// 截图历史文件路径（目录内的 history.txt）。
QString appHistoryFile();

/// 把旧位置的数据搬到新位置（只在 macOS 且新位置尚无配置时执行一次）。
/// 返回是否发生了迁移。搬的是**复制**：旧文件保留，用户仍可回退。
bool migrateLegacyAppData(const QString& legacyDirectory);

/// 把文件移入系统回收站；失败时保留原文件，不降级为永久删除。
bool moveFileToTrash(const QString& path);

} // namespace jie2::ui
