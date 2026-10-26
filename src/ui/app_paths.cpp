#include "ui/app_paths.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

namespace jie2::ui {

namespace {

/// 应用名子目录名（与 QApplication::setApplicationName 一致）。
constexpr const char* kAppFolder = "jie2";

} // namespace

QString appDataDirectory() {
#if defined(Q_OS_MACOS)
    // AppDataLocation 在 macOS = ~/Library/Application Support/<AppName>。
    QString base = QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation);
    if (base.isEmpty()) {
        base = QDir::homePath() + QStringLiteral("/Library/Application Support/") +
               QLatin1String(kAppFolder);
    }
#else
    QString base = QStandardPaths::writableLocation(
        QStandardPaths::AppConfigLocation);
    if (base.isEmpty()) {
        base = QDir::homePath() + QLatin1Char('/') + QLatin1String(kAppFolder);
    }
#endif
    QDir().mkpath(base);
    return base;
}

QString appConfigFile() {
    return appDataDirectory() + QStringLiteral("/jie2.conf");
}

QString appHistoryFile() {
    return appDataDirectory() + QStringLiteral("/history.txt");
}

bool migrateLegacyAppData(const QString& legacyDirectory) {
    if (legacyDirectory.isEmpty()) {
        return false;
    }
    const QString target = appDataDirectory();
    if (QFileInfo(legacyDirectory).canonicalFilePath() ==
        QFileInfo(target).canonicalFilePath()) {
        return false;  // 新旧同处，无需迁移
    }
    bool migrated = false;
    for (const char* name : {"jie2.conf", "history.txt"}) {
        const QString from =
            legacyDirectory + QLatin1Char('/') + QLatin1String(name);
        const QString to = target + QLatin1Char('/') + QLatin1String(name);
        if (!QFileInfo::exists(from) || QFileInfo::exists(to)) {
            continue;
        }
        if (QFile::copy(from, to)) {
            migrated = true;
            qInfo().noquote()
                << QStringLiteral("[jie2] 已迁移旧配置：%1 → %2").arg(from, to);
        } else {
            qWarning().noquote()
                << QStringLiteral("[jie2] 旧配置迁移失败：%1").arg(from);
        }
    }
    return migrated;
}

bool moveFileToTrash(const QString& path) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    return QFile::moveToTrash(path);
#elif defined(Q_OS_LINUX)
    // Qt 5.12 没有回收站 API，使用桌面提供的 GIO；不使用 shell 解释路径。
    const QString executable = QStandardPaths::findExecutable(QStringLiteral("gio"));
    if (executable.isEmpty()) return false;
    QProcess process;
    process.start(executable, {QStringLiteral("trash"), QStringLiteral("--"),
                              QFileInfo(path).absoluteFilePath()});
    return process.waitForFinished(10000) &&
           process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
#else
    return false;
#endif
}

} // namespace jie2::ui
