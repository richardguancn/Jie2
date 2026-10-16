#include "ui/theme.h"

#include <QByteArray>
#include <QGuiApplication>
#include <QProcess>
#include <QStyleHints>
#include <QtGlobal>

namespace jie2::ui {

namespace {

/// 读系统深色状态（绕过 Qt 版本差异）。
/// 返回 1 = 深色、0 = 浅色、-1 = 无法判定。
int detectSystemDark() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
    if (scheme == Qt::ColorScheme::Dark) {
        return 1;
    }
    if (scheme == Qt::ColorScheme::Light) {
        return 0;
    }
#endif
    // Qt 判定不了（或版本过低）：走平台命令兜底。
#if defined(Q_OS_MACOS)
    {
        QProcess process;
        process.start(QStringLiteral("defaults"),
                      {QStringLiteral("read"), QStringLiteral("-g"),
                       QStringLiteral("AppleInterfaceStyle")});
        if (process.waitForFinished(500) && process.exitCode() == 0) {
            const QByteArray out = process.readAllStandardOutput();
            return out.contains("Dark") ? 1 : 0;
        }
        // 键不存在 = 浅色（macOS 只在深色时写这个键）。
        return 0;
    }
#elif defined(Q_OS_LINUX)
    {
        QProcess process;
        process.start(QStringLiteral("gsettings"),
                      {QStringLiteral("get"),
                       QStringLiteral("org.gnome.desktop.interface"),
                       QStringLiteral("color-scheme")});
        if (process.waitForFinished(500) && process.exitCode() == 0) {
            const QByteArray out = process.readAllStandardOutput().toLower();
            if (out.contains("dark")) {
                return 1;
            }
            if (out.contains("light") || out.contains("default")) {
                return 0;
            }
        }
    }
    {
        QProcess process;
        process.start(QStringLiteral("gsettings"),
                      {QStringLiteral("get"),
                       QStringLiteral("org.gnome.desktop.interface"),
                       QStringLiteral("gtk-theme")});
        if (process.waitForFinished(500) && process.exitCode() == 0) {
            const QByteArray out = process.readAllStandardOutput().toLower();
            if (out.contains("dark")) {
                return 1;
            }
            return 0;
        }
    }
    return -1;
#else
    return -1;
#endif
}

} // namespace

ThemeResolution resolveTheme(const QString& setting) {
    const QString value = setting.trimmed().toLower();
    if (value == QStringLiteral("dark")) {
        return {true, QStringLiteral("配置")};
    }
    if (value == QStringLiteral("light")) {
        return {false, QStringLiteral("配置")};
    }

    // system（或未知值）：先看测试环境变量，再探系统。
    const QByteArray env = qgetenv("JIE2_THEME").toLower();
    if (env == "dark") {
        return {true, QStringLiteral("环境变量")};
    }
    if (env == "light") {
        return {false, QStringLiteral("环境变量")};
    }

    const int systemDark = detectSystemDark();
    if (systemDark == 1) {
        return {true, QStringLiteral("系统")};
    }
    if (systemDark == 0) {
        return {false, QStringLiteral("系统")};
    }
    return {true, QStringLiteral("默认")};
}

} // namespace jie2::ui
