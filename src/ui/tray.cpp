#include "ui/system_menu.h"
#include <QScopeGuard>
#include "ui/tray.h"
#include "ui/qt_compat.h"

#include "ui/app_paths.h"
#include "ui/theme.h"
#include "ui/settings_dialog.h"
#include <QScopedValueRollback>

#include <algorithm>
#include <cstdint>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
#include <QColorSpace>
#endif
#include <QCryptographicHash>
#include <QCursor>
#include <QDateTime>
#include <QDebug>
#include <QDialog>
#include <QDesktopServices>
#include <QUrl>
#include <QUuid>
#include <QMimeData>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QFileSystemWatcher>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QScreen>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

#include "core/config.h"
#include "core/image_buffer.h"
#include "core/image_ops.h"
#include "core/ocr.h"
#include "ui/clothesline.h"
#include "ui/ocr_runner.h"
#include "ui/pin_window.h"
#include "ui/selection_overlay.h"
#include "ui/single_instance.h"

namespace jie2::ui {

namespace {

constexpr const char* kHotkeyId = "toggle-capture";
// Ctrl+Alt+C（专业版对齐项 v2.5）：把剪贴板中的图像直接保存为文件。
constexpr const char* kSaveClipboardHotkeyId = "save-clipboard-image";
// 贴图热键（M4）。Snipaste 默认 F3，但 F3 在多数系统上被占用概率高
// （浏览器搜索、IDE 查找），改用 Ctrl+Alt+P：P = Pin。
constexpr const char* kPinHotkeyId = "pin-clipboard";

/// macOS 使用单色模板，由系统按菜单栏背景调整颜色；其他平台使用完整应用图标。
QIcon makeTrayIcon() {
#ifdef Q_OS_MACOS
    QIcon icon;
    icon.addFile(QStringLiteral(":/brand/menubar/black-22.png"), QSize(22, 22));
    icon.addFile(QStringLiteral(":/brand/menubar/black-44.png"), QSize(44, 44));
    icon.addFile(QStringLiteral(":/brand/menubar/black-66.png"), QSize(66, 66));
    icon.setIsMask(true);
    return icon;
#else
    return QIcon(QStringLiteral(":/brand/app/icon-256.png"));
#endif
}

// 菜单只展示按键本身；配置中使用的键名仍保持原样。
QString menuShortcutLabel(const platform::HotkeySpec& spec) {
    using platform::Key;
    QString key;
    switch (spec.key) {
    case Key::Grave: key = QStringLiteral("`"); break;
    case Key::Minus: key = QStringLiteral("-"); break;
    case Key::Equal: key = QStringLiteral("="); break;
    case Key::BracketLeft: key = QStringLiteral("["); break;
    case Key::BracketRight: key = QStringLiteral("]"); break;
    case Key::Backslash: key = QStringLiteral("\\"); break;
    case Key::Semicolon: key = QStringLiteral(";"); break;
    case Key::Apostrophe: key = QStringLiteral("'"); break;
    case Key::Comma: key = QStringLiteral(","); break;
    case Key::Period: key = QStringLiteral("."); break;
    case Key::Slash: key = QStringLiteral("/"); break;
    case Key::Left: key = QStringLiteral("←"); break;
    case Key::Right: key = QStringLiteral("→"); break;
    case Key::Up: key = QStringLiteral("↑"); break;
    case Key::Down: key = QStringLiteral("↓"); break;
    case Key::Space: key = QStringLiteral("空格"); break;
    default: {
        auto plain = spec;
        plain.modifiers = 0;
        key = QString::fromStdString(platform::displayHotkey(plain));
        break;
    }
    }
    QString modifiers;
#ifdef Q_OS_MACOS
    if (spec.modifiers & platform::kModCtrl) modifiers += QStringLiteral("⌃");
    if (spec.modifiers & platform::kModAlt) modifiers += QStringLiteral("⌥");
    if (spec.modifiers & platform::kModShift) modifiers += QStringLiteral("⇧");
    if (spec.modifiers & platform::kModSuper) modifiers += QStringLiteral("⌘");
#else
    if (spec.modifiers & platform::kModCtrl) modifiers += QStringLiteral("Ctrl+");
    if (spec.modifiers & platform::kModAlt) modifiers += QStringLiteral("Alt+");
    if (spec.modifiers & platform::kModShift) modifiers += QStringLiteral("Shift+");
#ifdef Q_OS_WIN
    if (spec.modifiers & platform::kModSuper) modifiers += QStringLiteral("Win+");
#else
    if (spec.modifiers & platform::kModSuper) modifiers += QStringLiteral("Super+");
#endif
#endif
    return modifiers + key;
}

QString configFilePath() {
    return appConfigFile();
}

/// 全局快捷键的出厂默认值（修饰键按平台习惯，实现在平台层）。
QString defaultGlobalHotkey(const char* keyName) {
    return QString::fromStdString(platform::defaultHotkeyText(keyName[0]));
}

/// 截图历史文件（与配置同目录）。
QString historyFilePath() {
    return appHistoryFile();
}

/// 截图保存目录：图片目录下的 jie2 子目录。
QString captureDirectory() {
    QString base =
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (base.isEmpty()) {
        base = QDir::homePath();
    }
    return base + QStringLiteral("/jie2");
}

QString captureFileName() {
    return QStringLiteral("jie2-") +
           QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")) +
           QStringLiteral(".png");
}

/// 把内核图像缓冲包装成 QImage。
///
/// 必须按缓冲自己声明的 format() 选择 QImage 格式，不能写死一个：
///   - BGRA8888（抓屏输出）内存序 B,G,R,A ↔ QImage::Format_ARGB32（小端）；
///   - RGBA8888（fromQImage 的输出）内存序 R,G,B,A ↔ QImage::Format_RGBA8888。
/// 早期版本写死 ARGB32，导致经 fromQImage 往返的图像 R/B 通道对调
/// （历史回放裁剪时实测发现）。
QImage toQImage(const core::ImageBuffer& buffer) {
    if (buffer.isEmpty()) {
        return {};
    }
    const QImage::Format format =
        buffer.format() == core::PixelFormat::RGBA8888 ? QImage::Format_RGBA8888
                                                       : QImage::Format_ARGB32;
    const QImage wrapped(buffer.data(), buffer.width(), buffer.height(),
                         static_cast<qsizetype>(buffer.stride()), format);
    // copy() 让 QImage 拥有自己的像素，脱离内核缓冲的生命周期。
    return wrapped.copy();
}

/// QImage → 内核缓冲。统一转成显式字节序的 RGBA8888，
/// 避免依赖平台对 Format_ARGB32 的内存序假设。
core::ImageBuffer fromQImage(const QImage& image) {
    if (image.isNull()) {
        return {};
    }
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    core::ImageBuffer buffer(rgba.width(), rgba.height(), core::PixelFormat::RGBA8888);
    const std::size_t rowBytes = static_cast<std::size_t>(rgba.width()) * 4;
    for (int y = 0; y < rgba.height(); ++y) {
        const std::uint8_t* src = rgba.constScanLine(y);
        std::uint8_t* dst = buffer.scanline(y);
        std::copy(src, src + rowBytes, dst);
    }
    return buffer;
}

/// 把 Qt 屏幕对回平台层的显示器条目。
///
/// 坐标模型（与 platform/display.h 的约定一致）：
///   - 平台 bounds 与 Qt geometry() 都在**逻辑坐标**空间（macOS 为点，X11 为像素）。
///   - 抓屏输出是物理像素，物理/逻辑比 = scaleFactor（macOS Retina 为 2）。
///
/// 所以先按"逻辑尺寸 == bounds 尺寸"匹配；个别平台若把 bounds 报成物理像素，
/// 再按"逻辑尺寸 × DPR"兜底一次；都匹配不上才退回主屏。
const platform::DisplayInfo* findDisplayForScreen(
    const std::vector<platform::DisplayInfo>& displays, QScreen* screen) {
    if (displays.empty()) {
        return nullptr;
    }
    if (screen != nullptr) {
        const QSize logical = screen->geometry().size();
        for (const auto& display : displays) {
            if (logical.width() == display.bounds.width &&
                logical.height() == display.bounds.height) {
                return &display;
            }
        }
        const qreal dpr = screen->devicePixelRatio();
        for (const auto& display : displays) {
            if (qRound(logical.width() * dpr) == display.bounds.width &&
                qRound(logical.height() * dpr) == display.bounds.height) {
                return &display;
            }
        }
    }
    for (const auto& display : displays) {
        if (display.isPrimary) {
            return &display;
        }
    }
    return &displays.front();
}

} // namespace

TrayController::TrayController(std::unique_ptr<platform::Platform> platform,
                               QObject* parent)
    : QObject(parent), platform_(std::move(platform)) {
    configPath_ = configFilePath();
    // 在构造函数里加载而不是 start()：CLI 直启路径（--capture-once /
    // --select-rect）不经过 start()，若此时不加载，保存时就会用空状态
    // 覆盖磁盘上的历史与计数（早期版本即因此丢失历史）。
    loadConfig();
    loadHistory();
    setupTriggerCorner();
}

TrayController::~TrayController() {
    saveConfig();
    // 先停 Wayland 顶部条子进程：避免它在主进程退出后成为孤儿窗口。
    stopWaylandLine();
    if (platform_) {
        platform_->releaseAllHotkeys();
    }
    // 贴图窗口与衣架窗口：退出时统一销毁。
    // （ASan 曾报告 Clothesline 等退出泄漏 —— 顶层窗口没有父对象，
    //   不显式释放就会泄漏；虽不影响功能，但退出路径应当干净。）
    for (PinWindow* pin : pins_) {
        delete pin;
    }
    pins_.clear();
    delete clothesline_;
    clothesline_ = nullptr;
    delete menu_;
    menu_ = nullptr;
}

bool TrayController::start() {
    // 配置与历史已在构造函数里加载（CLI 路径不经过这里）。
    capabilities_ = platform_->probe();

    // 衣架（M3）：只在托盘常驻模式下创建 —— CLI 一次性路径不需要它，
    // 也不该为了它去启动指针轮询。
    //
    // Wayland 例外（B 项）：Qt 窗口在 Wayland 下不能定位，Qt 衣架的
    // "贴屏幕顶边"语义无法实现 —— 改走 layer-shell 顶部条（jie2-line
    // 子进程），数据经 items 文件单向传递。
    if (clotheslineEnabled_ &&
        capabilities_.session == core::SessionType::Wayland) {
        waylandLineEnabled_ = true;
        qInfo().noquote() << QStringLiteral(
            "[jie2] Wayland 会话：衣架改用 layer-shell 顶部条（jie2-line）");
        updateWaylandLine();
    } else if (capabilities_.session != core::SessionType::Wayland) {
        clothesline_ = new Clothesline();
        clothesline_->setManagedDirectory(captureDirectory());
        clothesline_->setDarkTheme(darkTheme_);  // 主题跟随系统（v2.6）
        // 复制动作由这里执行（而不是衣架窗口自己）—— 写入剪贴板会同步
        // 触发本控制器的剪贴板监听，而监听会重建衣架数据；把写入集中在
        // 一处并在前后屏蔽监听，避免重入（实测：衣架自己写会导致段错误）。
        connect(clothesline_, &Clothesline::copyRequested, this,
                [this](const QString& path) {
                    const QImage image(path);
                    if (image.isNull()) {
                        note(QStringLiteral("读取失败"), QFileInfo(path).fileName(),
                             /*warning=*/true);
                        return;
                    }
                    writeClipboardImage(image);
                    qInfo().noquote()
                        << QStringLiteral("[jie2] 衣架：已复制 %1 到剪贴板")
                               .arg(path);
                    clothesline_->showCopied(path);
                });
        connect(clothesline_, &Clothesline::revealRequested, this, [this](const QString& path) {
#ifdef Q_OS_MACOS
            if (!QProcess::startDetached(QStringLiteral("/usr/bin/open"), {QStringLiteral("-R"), path}))
                note(QStringLiteral("打开失败"), path, true);
#else
            if (!QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath())))
                note(QStringLiteral("打开失败"), path, true);
#endif
        });
        connect(clothesline_, &Clothesline::saveDesktopRequested, this, [this](const QString& path) {
            const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
            if (desktop.isEmpty()) { note(QStringLiteral("保存失败"), QStringLiteral("未找到桌面目录"), true); return; }
            const QFileInfo source(path);
            QString target = QDir(desktop).filePath(source.fileName());
            for (int i = 1; QFileInfo::exists(target); ++i)
                target = QDir(desktop).filePath(QStringLiteral("%1 (%2).%3").arg(source.completeBaseName()).arg(i).arg(source.suffix()));
            if (!QFile::copy(path, target)) {
                note(QStringLiteral("保存失败"), target, true);
                return;
            }
            clothesline_->takeDownPath(path);
            note(QStringLiteral("保存完成"), target);
        });
        connect(clothesline_, &Clothesline::discardRequested, this, [this](const QString& path) {
            const bool owned = QFileInfo(path).canonicalPath() == QFileInfo(captureDirectory()).canonicalFilePath();
            // 自有截图进入废纸篓；外部图片只从衣架取下。
            if (owned && QFileInfo::exists(path) && !moveFileToTrash(path)) {
                note(QStringLiteral("删除失败"), path, true);
                return;
            }
            clothesline_->takeDownPath(path);
        });
        connect(clothesline_, &Clothesline::openRequested, this,
                [this](const QString& path) {
                    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
                        note(QStringLiteral("打开失败"), QFileInfo(path).fileName(), true);
                    }
                });
        connect(clothesline_, &Clothesline::annotateRequested, this,
                [this](const QString& path) {
                    const QImage image(path);
                    if (image.isNull()) {
                        note(QStringLiteral("读取失败"), QFileInfo(path).fileName(), true);
                        return;
                    }
                    captureInProgress_ = true;
                    clothesline_->setWatchEnabled(false);
                    clothesline_->hideNow();
                    SelectionOverlay::Options options;
                    options.selectImage = true;
                    options.darkTheme = darkTheme_;
                    options.shortcuts = shortcutMap_;
                    options.paletteSize = paletteSize_;
                    options.eraserUndoable = eraserUndoable_;
                    options.promoteWindow = makeOverlayPromoter();
                    options.ocrConfig = ocrConfig_;
                    const auto result = SelectionOverlay::run(QPixmap::fromImage(image), 1.0,
                        QGuiApplication::screenAt(QCursor::pos()), options);
                    if (result.accepted && !result.outputImage.isNull()) {
                        QString error;
                        // 标注结果另存，取消或完成都保留原图。
                        if (deliverSelection(result, &error, false).isEmpty()) {
                            note(QStringLiteral("保存失败"), error, true);
                        }
                    }
                    captureInProgress_ = false;
                    clothesline_->setWatchEnabled(clotheslineEnabled_);
                });
        connect(clothesline_, &Clothesline::entryDropped, this,
                [this](const QString& path) {
                    history_.remove(path.toStdString());
                    saveHistory();
                    refreshClothesline();
                });
        refreshClothesline();
        clothesline_->setWatchEnabled(clotheslineEnabled_);
    }

    // 剪贴板图片监听（M3 增补）：复制图像自动挂上衣架。
    // 系统截图（macOS Cmd+Shift+4 等）默认存文件而不进剪贴板，
    // 所以同时监听系统截图的落盘目录。
    setupClipboardWatcher();
    setupSystemCaptureWatcher();

    tray_ = new QSystemTrayIcon(makeTrayIcon(), this);
    tray_->setToolTip(QStringLiteral("J2 — 截图与贴图"));

    menu_ = new QMenu();
    rebuildMenu();
#ifdef Q_OS_MACOS
    // 系统负责菜单绘制与跟踪，关闭后再执行动作；不同时绑定第二套入口。
    connect(tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger ||
                    reason == QSystemTrayIcon::Context) {
                    QTimer::singleShot(0, this, [this] {
                        if (!menu_->isVisible() && !systemMenuIsOpen()) {
                            const QRect anchor = tray_->geometry();
                            popupContextMenu(menu_, anchor.isValid() ? anchor.bottomLeft() : QCursor::pos());
                        }
                    });
                }
            });
#else
    tray_->setContextMenu(menu_);
    connect(tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger) {
                    menu_->popup(QCursor::pos());
                }
            });
#endif
    tray_->show();

#if defined(Q_OS_LINUX)
    // 银河麒麟 UKUI 等 Linux 桌面面板托盘服务可能延迟就绪。
    // 若初始未检测到托盘，定时轮询重试直到托盘可用并重新 show。
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        auto* retryTimer = new QTimer(this);
        retryTimer->setInterval(1500);
        connect(retryTimer, &QTimer::timeout, this, [this, retryTimer]() {
            if (QSystemTrayIcon::isSystemTrayAvailable()) {
                if (tray_ != nullptr) {
                    tray_->show();
                }
                retryTimer->stop();
                retryTimer->deleteLater();
            }
        });
        retryTimer->start();
    }
#endif

    registerHotkey();

    // 平台能力受限时第一时间如实提示（评估报告 3.2 要求降级对用户可见）。
    if (capabilities_.hasLimitations() && tray_->isSystemTrayAvailable()) {
        tray_->showMessage(
            QStringLiteral("功能受限"),
            QString::fromStdString(capabilities_.describe()),
            QSystemTrayIcon::Warning, 8000);
    }
    return true;
}

void TrayController::rebuildMenu() {
    // 测试/CLI 路径没有托盘菜单（start() 未调用）——
    // 早期版本在这里空指针崩溃（--test-pin-groups 实测 SIGSEGV）。
    if (menu_ == nullptr) {
        return;
    }
    menu_->clear();
    menu_->setToolTipsVisible(true);

    auto* captureAction = menu_->addAction(hotkeyRegistered_
        ? QStringLiteral("截图\t%1").arg(menuShortcutLabel(snipHotkeySpec_))
        : QStringLiteral("截图"));
    captureAction->setToolTip(hotkeyRegistered_ ? QStringLiteral("拖动鼠标，选择要截取的区域")
        : QStringLiteral("可点击截图；快捷键不可用：%1").arg(hotkeyError_));
    connect(captureAction, &QAction::triggered, this,
            &TrayController::triggerCapture, Qt::QueuedConnection);

    // 贴图（M4）：把剪贴板内容贴成置顶窗口。
    auto* pinAction = menu_->addAction(pinHotkeyRegistered_
        ? QStringLiteral("贴到屏幕\t%1").arg(menuShortcutLabel(pinHotkeySpec_))
        : QStringLiteral("贴到屏幕"));
    pinAction->setToolTip(pinHotkeyRegistered_ ? QStringLiteral("把已复制的图片、文字或颜色贴在屏幕上")
        : QStringLiteral("可点击贴图；快捷键不可用：%1").arg(pinHotkeyError_));
    connect(pinAction, &QAction::triggered, this, &TrayController::triggerPin, Qt::QueuedConnection);

    // 文字识别（M5）：对剪贴板图像跑 OCR。
    auto* ocrAction = menu_->addAction(
        QStringLiteral("提取文字"));
    ocrAction->setToolTip(QStringLiteral("提取已复制图片中的文字"));
    connect(ocrAction, &QAction::triggered, this, &TrayController::triggerOcr, Qt::QueuedConnection);

    auto* translateAction = menu_->addAction(QStringLiteral("翻译内容"));
    translateAction->setToolTip(QStringLiteral("翻译已复制的文字或图片"));
    connect(translateAction, &QAction::triggered, this, &TrayController::triggerTranslation, Qt::QueuedConnection);

    // 二维码/条形码识别（M6，专业版对齐项 barcode-scan）。
    auto* barcodeAction =
        menu_->addAction(QStringLiteral("扫码"));
    barcodeAction->setToolTip(QStringLiteral("读取已复制图片中的二维码或条形码"));
    connect(barcodeAction, &QAction::triggered, this,
            &TrayController::triggerBarcodeScan, Qt::QueuedConnection);

    if (!pins_.empty()) {
        menu_->addSeparator();
        auto* hidePins = menu_->addAction(
            pinsHidden_ ? QStringLiteral("全部显示") : QStringLiteral("全部隐藏"));
        hidePins->setToolTip(QStringLiteral("共 %1 张屏幕贴图").arg(pins_.size()));
        connect(hidePins, &QAction::triggered, this,
                &TrayController::toggleHidePins, Qt::QueuedConnection);
        auto* savePins = menu_->addAction(
            QStringLiteral("全部保存"));
        savePins->setToolTip(QStringLiteral("保存全部 %1 张屏幕贴图").arg(pins_.size()));
        connect(savePins, &QAction::triggered, this,
                &TrayController::saveAllPins, Qt::QueuedConnection);

        // 贴图分组（专业版对齐项）：只显示当前组的贴图。
        auto* groupMenu = menu_->addMenu(
            QStringLiteral("贴图分组"));
        for (int g = 0; g < std::max(1, pinGroupCount_); ++g) {
            QAction* item = groupMenu->addAction(
                QStringLiteral("第 %1 组").arg(g + 1));
            item->setCheckable(true);
            item->setChecked(g == currentPinGroup_);
            connect(item, &QAction::triggered, this,
                    [this, g]() { switchPinGroup(g); }, Qt::QueuedConnection);
        }
        QAction* nextGroup = groupMenu->addAction(QStringLiteral("下组贴图"));
        connect(nextGroup, &QAction::triggered, this,
                [this]() { switchPinGroup(-1); }, Qt::QueuedConnection);

        auto* closePins = menu_->addAction(QStringLiteral("全部关闭"));
        connect(closePins, &QAction::triggered, this,
                &TrayController::closeAllPins, Qt::QueuedConnection);
    }

    if (captureCount_ > 0) {
        auto* recent = menu_->addMenu(QStringLiteral("最近截图"));
        auto* lastCapture = recent->addAction(recent->fontMetrics().elidedText(
            QFileInfo(lastCapturePath_).fileName(), Qt::ElideMiddle, 240));
        lastCapture->setToolTip(lastCapturePath_);
        lastCapture->setEnabled(false);
    }

    menu_->addSeparator();

    // 输出装饰（M1）：边框 / 阴影 / 圆角。参数走配置文件，菜单只做开关。
    auto* decorationMenu = menu_->addMenu(QStringLiteral("截图外观"));
    decorationMenu->setToolTipsVisible(true);
    auto makeToggle = [this, decorationMenu](const QString& text, bool checked,
                                             auto apply) {
        QAction* action = decorationMenu->addAction(text);
        action->setCheckable(true);
        action->setChecked(checked);
        connect(action, &QAction::triggered, this, [this, apply](bool on) {
            apply(on);
            saveConfig();
            rebuildMenu();
        }, Qt::QueuedConnection);
        return action;
    };
    makeToggle(QStringLiteral("圆角"), roundEnabled_,
               [this](bool on) { roundEnabled_ = on; })
        ->setToolTip(QStringLiteral("圆角半径 %1 像素").arg(roundRadius_));
    makeToggle(QStringLiteral("边框"), borderEnabled_,
               [this](bool on) { borderEnabled_ = on; })
        ->setToolTip(QStringLiteral("边框宽度 %1 像素").arg(borderWidth_));
    makeToggle(QStringLiteral("阴影"), shadowEnabled_,
               [this](bool on) { shadowEnabled_ = on; });

    // 触发角（M1）：屏幕角落停留触发截图。
    QAction* triggerAction =
        menu_->addAction(QStringLiteral("角落截图"));
    triggerAction->setToolTip(QStringLiteral("角落停留 %1 毫秒后截图").arg(triggerDwellMs_));
    triggerAction->setCheckable(true);
    triggerAction->setChecked(triggerEnabled_);
    connect(triggerAction, &QAction::triggered, this, [this](bool on) {
        triggerEnabled_ = on;
        saveConfig();
        setupTriggerCorner();
        rebuildMenu();
    }, Qt::QueuedConnection);

    // 衣架（M3，tendedero 合并）：指针停在屏幕顶边展开。
    QAction* clotheslineAction =
        menu_->addAction(QStringLiteral("图片暂存"));
    clotheslineAction->setToolTip(QStringLiteral("鼠标停在屏幕顶部，查看暂存图片"));
    clotheslineAction->setCheckable(true);
    clotheslineAction->setChecked(clotheslineEnabled_);
    clotheslineAction->setEnabled(clothesline_ != nullptr ||
                                 capabilities_.session == core::SessionType::Wayland);
    connect(clotheslineAction, &QAction::triggered, this, [this](bool on) {
        clotheslineEnabled_ = on;
        saveConfig();
        if (clothesline_ != nullptr) {
            clothesline_->setWatchEnabled(on);
        } else if (capabilities_.session == core::SessionType::Wayland) {
            if (on) {
                waylandLineEnabled_ = true;
                updateWaylandLine();
            } else {
                stopWaylandLine();
            }
        }
    }, Qt::QueuedConnection);

    // 剪贴板图像自动挂载（M3 增补，对齐 tendedero 的 "Hang copied images"）。
    QAction* clipWatch =
        menu_->addAction(QStringLiteral("自动收集"));
    clipWatch->setToolTip(QStringLiteral("自动暂存复制的图片"));
    clipWatch->setCheckable(true);
    clipWatch->setChecked(clipboardWatchEnabled_);
    connect(clipWatch, &QAction::triggered, this, [this](bool on) {
        clipboardWatchEnabled_ = on;
        saveConfig();
        rebuildMenu();
    }, Qt::QueuedConnection);

    menu_->addSeparator();

    auto* settingsAction = menu_->addAction(QStringLiteral("设置…"));
    connect(settingsAction, &QAction::triggered, this, &TrayController::showSettings, Qt::QueuedConnection);

    auto* capabilityAction = menu_->addAction(QStringLiteral("功能检查…"));
    connect(capabilityAction, &QAction::triggered, this,
            &TrayController::showCapabilities, Qt::QueuedConnection);

    menu_->addSeparator();

    auto* quitAction = menu_->addAction(QStringLiteral("退出应用"));
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit, Qt::QueuedConnection);
}

QString TrayController::deliverSelection(const SelectionOverlay::Outcome& outcome, QString* error,
                                         bool applyOutputOptions) {
    using Action = SelectionOverlay::Action;
    if (outcome.action == Action::Finish)
        return saveAndCopy(outcome.outputImage, error, true, applyOutputOptions);
    pendingFlyRect_ = {};
    QImage image = applyOutputOptions ? decorateForOutput(outcome.outputImage) : outcome.outputImage;
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    image.setColorSpace(QColorSpace::SRgb);
#endif
    if (outcome.action == Action::SaveFile) {
        QString path = QFileDialog::getSaveFileName(nullptr, QStringLiteral("保存截图"),
            QDir(captureDirectory()).filePath(captureFileName()), QStringLiteral("PNG 图像 (*.png);;JPEG 图像 (*.jpg)"));
        if (path.isEmpty()) return {};
        if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".png");
        if (!image.save(path)) {
            if (error) *error = QStringLiteral("保存失败：%1").arg(path);
            return {};
        }
        writeClipboardImage(image);
        recordCapture(path, image);
        ++captureCount_;
        lastCapturePath_ = path;
        saveConfig();
        return path;
    }
    writeClipboardImage(image);
    if (outcome.action == Action::Pin) {
        triggerPin();
        return QStringLiteral("(pin)");
    }
    return QStringLiteral("(clipboard)");
}

QString TrayController::saveAndCopy(const QImage& image, QString* error,
                                    bool writeClipboard, bool applyOutputOptions) {
    const auto setError = [error](const QString& text) {
        if (error != nullptr) {
            *error = text;
        }
        return QString{};
    };

    if (image.isNull()) {
        return setError(QStringLiteral("空图像，未保存"));
    }

    // 输出装饰（M1）：边框 / 阴影 / 圆角按设置应用。
    // 放在保存前统一处理，使文件与剪贴板拿到同一份成品。
    QImage output = applyOutputOptions ? decorateForOutput(image) : image;
    // --size W H（M6）：强制输出尺寸（在装饰之后，得到的就是最终尺寸）。
    // --keep-aspect-ratio（专业版对齐项 v2.8.6）：改"拉伸填满"为
    // "等比缩放到 W×H 内" —— 输出可能小于指定尺寸（取决于源比例），
    // 这与 Snipaste 的语义一致（保比例优先于精确尺寸）。
    if (applyOutputOptions && overrides_.hasSize && overrides_.sizeWidth > 0 &&
        overrides_.sizeHeight > 0 &&
        (output.width() != overrides_.sizeWidth ||
         output.height() != overrides_.sizeHeight)) {
        const Qt::AspectRatioMode mode = overrides_.keepAspectRatio
                                             ? Qt::KeepAspectRatio
                                             : Qt::IgnoreAspectRatio;
        output = output.scaled(overrides_.sizeWidth, overrides_.sizeHeight,
                               mode, Qt::SmoothTransformation);
    }
    // 色彩管理（M1 收尾项）：像素语义本就是 sRGB（实测与系统工具差异≤1），
    // 这里补上颜色空间标记，让严格做色彩管理的程序正确解释本文件。
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    output.setColorSpace(QColorSpace::SRgb);
#endif

    // 与 Snipaste 的默认行为一致：截图落点之一是剪贴板。
    // 剪贴板始终写入（--no-save 也只跳过落盘，不影响剪贴板）。
    // 走 writeClipboardImage：它会记录摘要，避免这次写入引发的
    // （异步）剪贴板事件被当成"用户复制"再挂一次。
    // writeClipboard=false 的调用方（Ctrl+Alt+C 保存剪贴板图像）跳过此步：
    // 源内容本来就是剪贴板里的图，回写既多余也可能打扰用户的剪贴板状态。
    if (writeClipboard) {
        writeClipboardImage(output);
    }

    // --no-save（M6）：只写剪贴板，不落盘、不进历史。
    if (applyOutputOptions && overrides_.noSave) {
        qInfo().noquote() << QStringLiteral("[jie2] --no-save：仅写入剪贴板");
        ++captureCount_;
        saveConfig();
        return QStringLiteral("(clipboard)");
    }

    // 输出路径：命令行显式指定优先（M6），否则按时间自动命名。
    QString path;
    if (applyOutputOptions && !overrides_.outputPath.isEmpty()) {
        path = overrides_.outputPath;
        const QFileInfo info(path);
        if (!info.absoluteDir().exists() &&
            !QDir().mkpath(info.absolutePath())) {
            return setError(QStringLiteral("无法创建输出目录：%1")
                                .arg(info.absolutePath()));
        }
        if (path.endsWith(QStringLiteral(".jpg"), Qt::CaseInsensitive) ||
            path.endsWith(QStringLiteral(".jpeg"), Qt::CaseInsensitive)) {
            if (!output.save(path, "JPEG", 95)) {
                return setError(QStringLiteral("保存 JPEG 失败：%1").arg(path));
            }
        } else if (!output.save(path, "PNG")) {
            return setError(QStringLiteral("保存 PNG 失败：%1").arg(path));
        }
    } else {
        const QString directory = captureDirectory();
        if (!QDir().mkpath(directory)) {
            return setError(QStringLiteral("无法创建截图目录：%1").arg(directory));
        }
        path = directory + QLatin1Char('/') +
            (applyOutputOptions ? captureFileName() :
             QStringLiteral("edited-%1.png").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        if (!output.save(path, "PNG")) {
            return setError(QStringLiteral("保存 PNG 失败：%1").arg(path));
        }
    }

    // 记入历史：只有"成功保存"的截图才进历史（与手册的语义一致）。
    recordCapture(path, output);
    ++captureCount_;
    lastCapturePath_ = path;
    saveConfig();
    return path;
}

QString TrayController::captureToFile(QString* error) {
    const auto setError = [error](const QString& text) {
        if (error != nullptr) {
            *error = text;
        }
        return QString{};
    };

    const auto displays = platform_->displays();
    if (displays.empty()) {
        return setError(QStringLiteral("未找到可用显示器"));
    }
    const platform::DisplayInfo* target = nullptr;
    for (const auto& display : displays) {
        if (display.isPrimary) {
            target = &display;
            break;
        }
    }
    if (target == nullptr) {
        target = &displays.front();
    }

    const platform::CaptureResult result = platform_->captureDisplay(target->id);
    if (!result.ok) {
        return setError(QString::fromStdString(result.error));
    }

    const QImage image = toQImage(result.image);
    if (image.isNull()) {
        return setError(QStringLiteral("抓屏返回空图像"));
    }

    return saveAndCopy(image, error);
}

QString TrayController::captureRectToFile(const QRect& logicalSelection,
                                          QScreen* requestedScreen,
                                          QString* error) {
    const auto setError = [error](const QString& text) {
        if (error != nullptr) {
            *error = text;
        }
        return QString{};
    };

    // 未探测过就先探测（例如 --select 直启路径未走 start()）。
    if (capabilities_.screenCapture.status == core::CapabilityStatus::Unknown) {
        capabilities_ = platform_->probe();
    }

    // 抓屏能力在部分会话下不可用（如未实现 portal 的 Wayland）——
    // 如实拒绝并说明原因，而不是弹出一个必然失败的选择界面。
    if (capabilities_.screenCapture.status == core::CapabilityStatus::Unavailable ||
        capabilities_.screenCapture.status == core::CapabilityStatus::Unknown) {
        const QString detail = QString::fromStdString(
            capabilities_.screenCapture.detail);
        return setError(detail.isEmpty()
                            ? QStringLiteral("当前会话不支持抓屏")
                            : QStringLiteral("当前会话不支持抓屏：%1").arg(detail));
    }

    // 目标屏幕：调用方指定优先，其次光标所在屏，最后主屏。
    QScreen* screen = requestedScreen;
    if (screen == nullptr) {
        screen = QGuiApplication::screenAt(QCursor::pos());
    }
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    const auto displays = platform_->displays();
    const platform::DisplayInfo* target = findDisplayForScreen(displays, screen);
    if (target == nullptr) {
        return setError(QStringLiteral("未找到可用显示器"));
    }

    // 冻结画面（在遮罩出现之前抓取，遮罩本身不会进入截图）。
    const platform::CaptureResult result = platform_->captureDisplay(target->id);
    if (!result.ok) {
        return setError(QString::fromStdString(result.error));
    }
    const QImage image = toQImage(result.image);
    if (image.isNull()) {
        return setError(QStringLiteral("抓屏返回空图像"));
    }

    // 逻辑点 → 物理像素的比例，由"抓到的图像宽度 / 逻辑宽度"得出。
    // 这是唯一可靠的口径：它描述的是**实际拿到的像素**与选区坐标的关系，
    // 不依赖平台把 bounds 报成点还是像素（早期版本用 bounds/geometry
    // 在 Retina 上会得出错误的 1.0，导致裁剪与取色整体错位）。
    const QRect screenGeometry = screen->geometry();
    if (screenGeometry.isEmpty()) {
        return setError(QStringLiteral("屏幕几何无效"));
    }
    const double scale = double(image.width()) / double(screenGeometry.width());

    const QRect selection = logicalSelection.intersected(screenGeometry);
    if (selection.isEmpty()) {
        return setError(QStringLiteral("选区为空或超出屏幕范围"));
    }

    // 裁剪：选区（逻辑点）→ 物理像素 → 从抓屏结果取像素。
    const core::Rect region{
        qRound(selection.x() * scale),
        qRound(selection.y() * scale),
        qRound(selection.width() * scale),
        qRound(selection.height() * scale)};
    const core::ImageBuffer cropped = core::crop(result.image, region);
    if (cropped.isEmpty()) {
        return setError(QStringLiteral("选区无效（裁剪结果为空）"));
    }

    // 脚本化截图（--select）也播飞入动效：selection 已是全局逻辑坐标。
    pendingFlyRect_ = selection;
    const QString saved = saveAndCopy(toQImage(cropped), error);
    if (saved.isEmpty()) {
        pendingFlyRect_ = QRect();
    }
    return saved;
}

QString TrayController::captureWithSelection(QString* error) {
    // 先做能力自检与屏幕解析：复用 captureRectToFile 的同一套前置逻辑，
    // 这里只多一步"让用户选区域"。为避免重复实现，先探测能力，
    // 再抓冻结画面给遮罩，最后把选区交给同一条裁剪链路。
    if (capabilities_.screenCapture.status == core::CapabilityStatus::Unknown) {
        capabilities_ = platform_->probe();
    }
    if (capabilities_.screenCapture.status == core::CapabilityStatus::Unavailable ||
        capabilities_.screenCapture.status == core::CapabilityStatus::Unknown) {
        const QString detail =
            QString::fromStdString(capabilities_.screenCapture.detail);
        if (error != nullptr) {
            *error = detail.isEmpty()
                         ? QStringLiteral("当前会话不支持抓屏")
                         : QStringLiteral("当前会话不支持抓屏：%1").arg(detail);
        }
        return {};
    }

    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    const auto displays = platform_->displays();
    const platform::DisplayInfo* target = findDisplayForScreen(displays, screen);
    if (target == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("未找到可用显示器");
        }
        return {};
    }

    const platform::CaptureResult result = platform_->captureDisplay(target->id);
    if (!result.ok) {
        if (error != nullptr) {
            *error = QString::fromStdString(result.error);
        }
        return {};
    }
    const QImage image = toQImage(result.image);
    if (image.isNull()) {
        if (error != nullptr) {
            *error = QStringLiteral("抓屏返回空图像");
        }
        return {};
    }

    const QRect screenGeometry = screen->geometry();
    if (screenGeometry.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("屏幕几何无效");
        }
        return {};
    }
    const double scale = double(image.width()) / double(screenGeometry.width());

    // 历史路径（旧的在前）供 , / . 回放。
    QStringList historyPaths;
    for (const auto& entry : history_.entries()) {
        historyPaths << QString::fromStdString(entry.path);
    }
    // F5 刷新回调：重抓同一屏（遮罩自己会先隐藏，见 SelectionOverlay::recapture）。
    const std::string targetId = target->id;
    auto recapture = [this, targetId]() -> QPixmap {
        const platform::CaptureResult fresh = platform_->captureDisplay(targetId);
        if (!fresh.ok) {
            return {};
        }
        return QPixmap::fromImage(toQImage(fresh.image));
    };

    // 每次冻结画面时同时记录窗口顺序与元素查询对象，排除随后出现的遮罩。
    const QPointer<QScreen> snapScreen(screen);
    auto snapTargets = [this, snapScreen]() {
        SelectionSnapTargets targets;
        if (!snapScreen) return targets;
        const QRect geometry = snapScreen->geometry();
        const core::Rect screenBounds{0, 0, geometry.width(), geometry.height()};
        for (const auto& window : platform_->windowRects()) {
            const auto local = core::Rect{window.x - geometry.x(), window.y - geometry.y(),
                window.width, window.height}.intersected(screenBounds);
            if (!local.isEmpty()) targets.windows.push_back(local);
        }
        auto locator = platform_->createElementLocator();
        targets.accessibilityRequired = locator.accessibilityRequired;
        if (locator.rectanglesAt) {
            targets.elementsAt = [query = std::move(locator.rectanglesAt), geometry, screenBounds](core::Point point) {
                std::vector<core::Rect> local;
                for (const auto& item : query({point.x + geometry.x(), point.y + geometry.y()})) {
                    const auto clipped = core::Rect{item.x - geometry.x(), item.y - geometry.y(),
                        item.width, item.height}.intersected(screenBounds);
                    if (!clipped.isEmpty()) local.push_back(clipped);
                }
                return local;
            };
        }
        return targets;
    };

    SelectionOverlay::Options overlayOptions;
    overlayOptions.historyPaths = historyPaths;
    overlayOptions.recapture = recapture;
    overlayOptions.snapTargets = std::move(snapTargets);
    overlayOptions.paletteSize = paletteSize_;
    overlayOptions.shortcuts = shortcutMap_;
    overlayOptions.darkTheme = darkTheme_;
    overlayOptions.eraserUndoable = eraserUndoable_;
    overlayOptions.promoteWindow = makeOverlayPromoter();
    overlayOptions.ocrConfig = ocrConfig_;

    const SelectionOverlay::Outcome outcome = SelectionOverlay::run(
        QPixmap::fromImage(image), scale, screen, overlayOptions);
    if (!outcome.accepted) {
        return {};  // 用户取消：静默返回，不算错误
    }

    // 最终输出由遮罩合成并带出（源图 → 标注合成 → 按选区裁剪），
    // 这里直接保存 —— 不再重复做缩放换算与合成，杜绝两处实现不一致。
    if (outcome.outputImage.isNull()) {
        if (error != nullptr) {
            *error = QStringLiteral("选区无效（输出为空）");
        }
        return {};
    }
    // 飞入动效的源区域（tendedero CaptureFlight）：选区是本屏逻辑坐标，
    // 转全局逻辑坐标，recordCapture 据此让卡片从被截取的区域飞上衣架。
    pendingFlyRect_ = outcome.selection;
    pendingFlyRect_.translate(screen->geometry().topLeft());
    const QString path = deliverSelection(outcome, error);
    if (path.isEmpty()) {
        pendingFlyRect_ = QRect();  // 保存失败：清掉，避免污染下一张截图
    }
    return path;
}

QString TrayController::whiteboardWithSelection(const QColor& background,
                                                QString* error) {
    const auto setError = [error](const QString& text) {
        if (error != nullptr) {
            *error = text;
        }
        return QString{};
    };

    // 目标屏幕：光标所在屏，退回主屏（与交互截图一致）。
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen == nullptr) {
        return setError(QStringLiteral("未找到可用屏幕"));
    }
    const QRect geometry = screen->geometry();
    if (geometry.isEmpty()) {
        return setError(QStringLiteral("屏幕几何无效"));
    }

    // 画布用物理像素尺寸（与抓屏同口径），遮罩按 devicePixelRatio 换算。
    const double scale =
        screen->devicePixelRatio() > 0 ? screen->devicePixelRatio() : 1.0;
    QImage board(qRound(geometry.width() * scale),
                 qRound(geometry.height() * scale), QImage::Format_RGBA8888);
    board.fill(background);

    // 白板没有"历史回放"与"F5 刷新"的语义，两者都不传。
    SelectionOverlay::Options overlayOptions;
    overlayOptions.paletteSize = paletteSize_;
    overlayOptions.shortcuts = shortcutMap_;
    overlayOptions.darkTheme = darkTheme_;
    overlayOptions.eraserUndoable = eraserUndoable_;
    overlayOptions.promoteWindow = makeOverlayPromoter();
    overlayOptions.ocrConfig = ocrConfig_;
    const SelectionOverlay::Outcome outcome = SelectionOverlay::run(
        QPixmap::fromImage(board), scale, screen, overlayOptions);
    if (!outcome.accepted) {
        return {};  // 用户取消：静默
    }
    if (outcome.outputImage.isNull()) {
        return setError(QStringLiteral("白板输出为空"));
    }
    qInfo().noquote() << QStringLiteral("[jie2] 白板模式：输出 %1x%2")
                             .arg(outcome.outputImage.width())
                             .arg(outcome.outputImage.height());
    // 同 captureWithSelection：白板选区也从被截取区域飞上衣架。
    pendingFlyRect_ = outcome.selection;
    pendingFlyRect_.translate(geometry.topLeft());
    const QString path = deliverSelection(outcome, error);
    if (path.isEmpty()) {
        pendingFlyRect_ = QRect();
    }
    return path;
}

std::function<bool(void*)> TrayController::makeOverlayPromoter() {
    // 遮罩打开前先收起衣架（它本身是置顶窗，可能与遮罩抢层级）。
    // 返回的回调被遮罩在每次 show 时调用：平台层负责把窗口提到
    // 能盖住全屏应用 / 远程桌面客户端的层级并抢键盘焦点。
    std::function<bool(void*)> promoter = [this](void* nativeWindow) {
        if (!platform_) {
            return false;
        }
        const bool promoted =
            platform_->promoteOverlayWindow(nativeWindow);
        // 只提示一次：合成器不支持的会话（如 Wayland）每次截图都会失败，
        // 反复提示反而噪声。
        if (!promoted && !overlayPromoteWarned_) {
            overlayPromoteWarned_ = true;
            qInfo().noquote() << QStringLiteral(
                "[jie2] 当前会话无法提升遮罩层级（Wayland 合成器不允许客户端"
                "自定层级）；若遮罩被全屏或远程桌面窗口盖住，请先退出全屏");
        }
        return promoted;
    };
    return promoter;
}

void TrayController::triggerCapture() {
    // 遮罩打开期间抑制触发角重复触发。
    captureInProgress_ = true;
    // 截图前把衣架收起来：否则指针停在顶边时衣架展开，会被拍进截图。
    if (clothesline_ != nullptr) {
        clothesline_->hideNow();
    }
    QString error;
    const QString path = captureWithSelection(&error);
    captureInProgress_ = false;
    if (path.isEmpty() && error.isEmpty()) {
        return;  // 用户取消
    }
    if (path.isEmpty()) {
        note(QStringLiteral("截图失败"), error, /*warning=*/true);
        qWarning().noquote() << QStringLiteral("[jie2] 截图失败：%1").arg(error);
        return;
    }
    // 计数、历史与配置更新统一在 saveAndCopy 里完成（所有保存路径的公共出口）。
    if (path == QStringLiteral("(pin)")) { rebuildMenu(); return; }
    if (path == QStringLiteral("(clipboard)")) {
        note(QStringLiteral("复制完成"), QStringLiteral("截图已复制"));
        rebuildMenu(); return;
    }
    qInfo().noquote() << QStringLiteral("[jie2] 截图已保存：%1").arg(path);
    note(QStringLiteral("截图完成"),
         QStringLiteral("%1\n（也已复制到剪贴板）").arg(path));
    rebuildMenu();
}

QImage TrayController::decorateForOutput(const QImage& image) const {
    // 装饰参数：命令行覆盖优先于配置（M6）。
    const bool round = overrides_.hasRound ? (overrides_.roundRadius > 0)
                                           : roundEnabled_;
    const int radius = overrides_.hasRound ? overrides_.roundRadius : roundRadius_;
    const bool border = overrides_.hasBorder ? (overrides_.borderWidth > 0)
                                             : borderEnabled_;
    const int borderWidth = overrides_.hasBorder ? overrides_.borderWidth
                                                 : borderWidth_;
    const bool shadow = overrides_.hasShadow ? overrides_.shadow : shadowEnabled_;

    if (image.isNull() || (!round && !border && !shadow)) {
        return image;
    }

    core::ImageBuffer buffer = fromQImage(image);
    if (buffer.isEmpty()) {
        return image;
    }

    // 顺序：先切圆角（改内部形状），再加边框（贴住新形状），最后投影。
    if (round && radius > 0) {
        buffer = core::roundCorners(buffer, radius);
    }
    if (border && borderWidth > 0) {
        buffer = core::addBorder(buffer, borderWidth,
                                 core::Rgba{static_cast<std::uint8_t>(borderColor_.red()),
                                            static_cast<std::uint8_t>(borderColor_.green()),
                                            static_cast<std::uint8_t>(borderColor_.blue()),
                                            static_cast<std::uint8_t>(borderColor_.alpha())});
    }
    if (shadow) {
        core::ShadowSpec spec;
        spec.blurRadius = 12;
        spec.offsetX = 0;
        spec.offsetY = 4;
        buffer = core::addShadow(buffer, spec);
    }
    return toQImage(buffer);
}

// ---- 触发角（M1：屏幕角落停留触发截图）----

void TrayController::setupTriggerCorner() {
    if (cornerTimer_ == nullptr) {
        cornerTimer_ = new QTimer(this);
        connect(cornerTimer_, &QTimer::timeout, this, &TrayController::cornerTick);
    }
    // 触发角是"轻量轮询"里最不打扰的一条路径，但仍只在启用时才走定时器。
    if (triggerEnabled_) {
        cornerTimer_->start(120);
    } else {
        cornerTimer_->stop();
    }
}

void TrayController::cornerTick() {
    // 遮罩（或任何模态窗口）打开期间不触发 —— 否则用户在截图时会反复触发。
    if (captureInProgress_ || QApplication::activeModalWidget() != nullptr ||
        QApplication::activePopupWidget() != nullptr || systemMenuIsOpen()) {
        cornerEnteredAt_ = 0;
        return;
    }

    const QPoint pos = QCursor::pos();
    QScreen* screen = QGuiApplication::screenAt(pos);
    if (screen == nullptr) {
        cornerEnteredAt_ = 0;
        return;
    }
    const QRect area = screen->geometry();
    const int size = std::max(2, triggerSize_);

    // 命中判定：指针落在启用的角区内。
    const bool wantsTop = triggerCorners_.contains(QStringLiteral("all")) ||
                          triggerCorners_.contains(QStringLiteral("t"));
    const bool wantsBottom = triggerCorners_.contains(QStringLiteral("all")) ||
                             triggerCorners_.contains(QStringLiteral("b"));
    const bool wantsLeft = triggerCorners_.contains(QStringLiteral("all")) ||
                           triggerCorners_.contains(QStringLiteral("l"));
    const bool wantsRight = triggerCorners_.contains(QStringLiteral("all")) ||
                            triggerCorners_.contains(QStringLiteral("r"));

    const bool inLeft = pos.x() <= area.left() + size;
    const bool inRight = pos.x() >= area.right() - size;
    const bool inTop = pos.y() <= area.top() + size;
    const bool inBottom = pos.y() >= area.bottom() - size;

    const bool inCorner = (inLeft && wantsLeft && ((inTop && wantsTop) || (inBottom && wantsBottom))) ||
                          (inRight && wantsRight && ((inTop && wantsTop) || (inBottom && wantsBottom)));

    if (!inCorner) {
        cornerEnteredAt_ = 0;
        cornerArmed_ = true;
        return;
    }
    if (!cornerArmed_) {
        return;  // 上一次触发后还没离开角落
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (cornerEnteredAt_ == 0) {
        cornerEnteredAt_ = now;
        return;
    }
    if (now - cornerEnteredAt_ >= triggerDwellMs_) {
        cornerArmed_ = false;
        cornerEnteredAt_ = 0;
        qInfo().noquote() << QStringLiteral("[jie2] 触发角触发截图");
        triggerCapture();
    }
}

void TrayController::refreshClothesline() {
    // Wayland 会话：数据走 items 文件 → jie2-line（layer-shell 顶部条）。
    if (waylandLineEnabled_) {
        updateWaylandLine();
        return;
    }
    if (clothesline_ == nullptr) {
        return;
    }
    clothesline_->setEntries(history_.entries());
}

// ---- Wayland 顶部条（B 项：layer-shell）----

QString TrayController::findLineBinary() {
    // ① 环境变量覆盖（测试与自定义安装用）。
    const QByteArray override = qgetenv("JIE2_LINE_PATH");
    if (!override.isEmpty()) {
        const QString path = QString::fromLocal8Bit(override);
        if (QFileInfo::exists(path)) {
            return path;
        }
        qWarning().noquote()
            << QStringLiteral("[jie2] JIE2_LINE_PATH 指向的文件不存在：%1").arg(path);
    }
    // ② 与主程序同目录（安装后的 /usr/bin）。
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString same = appDir.filePath(QStringLiteral("jie2-line"));
    if (QFileInfo::exists(same)) {
        return same;
    }
    // ③ 构建树：可执行在 build-*/src/ui/，jie2-line 在 build-*/src/tools/。
    const QString buildTree =
        appDir.absoluteFilePath(QStringLiteral("../tools/jie2-line"));
    if (QFileInfo::exists(buildTree)) {
        // canonical 化：去掉路径里的 ".."，日志里更干净。
        const QString canonical = QFileInfo(buildTree).canonicalFilePath();
        return canonical.isEmpty() ? buildTree : canonical;
    }
    // ④ PATH。
    return QStandardPaths::findExecutable(QStringLiteral("jie2-line"));
}

void TrayController::updateWaylandLine() {
    if (!waylandLineEnabled_) {
        return;
    }
    // 1) 写 items 文件（jie2-line 轮询它的 mtime）。
    const QString itemsPath =
        appDataDirectory() + QStringLiteral("/line-items.txt");
    {
        QFile file(itemsPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate |
                       QIODevice::Text)) {
            qWarning().noquote() << QStringLiteral(
                                        "[jie2] Wayland 衣架：items 写入失败 %1")
                                        .arg(itemsPath);
            return;
        }
        QTextStream out(&file);
        for (const auto& entry : history_.entries()) {
            out << QString::fromStdString(entry.path) << '\n';
        }
    }

    // 2) 启动 jie2-line（只启动一次；启动失败则明确报告并停用）。
    if (lineProcess_ != nullptr) {
        if (lineProcess_->state() != QProcess::NotRunning) {
            return;  // 已在运行；items 文件变化它会自己重绘
        }
        qInfo().noquote() << QStringLiteral("[jie2] Wayland 衣架：子进程已退出"
                                            "（exitCode=%1），不再重启")
                                 .arg(lineProcess_->exitCode());
        waylandLineEnabled_ = false;
        return;
    }

    const QString binary = findLineBinary();
    if (binary.isEmpty()) {
        qWarning().noquote() << QStringLiteral(
            "[jie2] Wayland 衣架：找不到 jie2-line（可设环境变量 JIE2_LINE_PATH "
            "指定路径）；本次降级为不使用顶部条");
        waylandLineEnabled_ = false;
        return;
    }
    lineProcess_ = new QProcess(this);
    lineProcess_->setProgram(binary);
    lineProcess_->setArguments({QStringLiteral("--items"), itemsPath});
    // 主题（v2.6）：子进程没有读取主进程内存的渠道，用环境变量传递。
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("JIE2_THEME"),
                   darkTheme_ ? QStringLiteral("dark") : QStringLiteral("light"));
        lineProcess_->setProcessEnvironment(env);
    }
    // 子进程的输出并入主进程日志（便于诊断）。
    lineProcess_->setProcessChannelMode(QProcess::ForwardedChannels);
    lineProcess_->start();
    if (!lineProcess_->waitForStarted(3000)) {
        qWarning().noquote() << QStringLiteral(
                                    "[jie2] Wayland 衣架：启动 jie2-line 失败：%1")
                                    .arg(lineProcess_->errorString());
        delete lineProcess_;
        lineProcess_ = nullptr;
        waylandLineEnabled_ = false;
        return;
    }
    qInfo().noquote() << QStringLiteral("[jie2] Wayland 衣架：已启动 %1（layer-shell 顶部条）")
                             .arg(binary);
}

void TrayController::stopWaylandLine() {
    if (lineProcess_ == nullptr) {
        return;
    }
    lineProcess_->terminate();
    if (!lineProcess_->waitForFinished(800)) {
        lineProcess_->kill();
        lineProcess_->waitForFinished(300);
    }
    lineProcess_ = nullptr;
    waylandLineEnabled_ = false;
}

// ---- 剪贴板图片监听（M3 增补）----

QByteArray TrayController::imageDigest(const QImage& image) {
    if (image.isNull()) {
        return {};
    }
    // 缩到 32x32 后做 MD5 —— 快且对"同一张图重复复制/异步重放"稳定。
    const QImage small = image.scaled(32, 32, Qt::IgnoreAspectRatio,
                                      Qt::SmoothTransformation)
                             .convertToFormat(QImage::Format_RGBA8888);
    return QCryptographicHash::hash(
        QByteArray::fromRawData(reinterpret_cast<const char*>(small.constBits()),
                              static_cast<int>(small.sizeInBytes())),
        QCryptographicHash::Md5);
}

void TrayController::writeClipboardImage(const QImage& image) {
    if (image.isNull()) {
        return;
    }
    // 先记摘要再写：X11 的剪贴板事件是**异步**投递的，同步标志
    // （早期方案）拦不住它；内容级去重才是可靠的防线。
    lastClipboardDigest_ = imageDigest(image);
    QApplication::clipboard()->setImage(image);
}

void TrayController::setupClipboardWatcher() {
    QClipboard* clipboard = QApplication::clipboard();
    if (clipboard == nullptr) {
        return;
    }
    // 启动时先记录当前剪贴板摘要：Xvfb / 部分 X server 会在客户端
    // 首次监听时补发一次 selection 事件（内容与启动时相同），
    // 不记初值就会把"上一张遗留的图"当成用户刚复制的挂上去。
    const QImage current = clipboard->image();
    if (!current.isNull()) {
        lastClipboardDigest_ = imageDigest(current);
    }
    connect(clipboard, &QClipboard::dataChanged, this,
            &TrayController::clipboardChanged);
}

// ---- 系统截图监听 ----
//
// 系统截图工具（macOS Cmd+Shift+4、Windows Win+Shift+S 存盘模式、
// GNOME/KDE 截图）**不经过本程序**：它们按系统设置把图直接写到磁盘。
// 只监听剪贴板拿不到这类截图，所以这里再加一路目录监听。

QStringList TrayController::systemCaptureDirectories() {
    QStringList dirs;
    // 显式覆盖（多个目录用路径分隔符隔开）：既方便用户把截图存到自定义
    // 目录后仍能自动挂载，也让这条链路可以被自动化测试驱动。
    const QString override = QString::fromLocal8Bit(
        qgetenv("JIE2_SYSTEM_CAPTURE_DIRS"));
    if (!override.isEmpty()) {
        const QStringList parts =
            override.split(QDir::listSeparator(), kSkipEmptyParts);
        for (const QString& part : parts) {
            if (QFileInfo(part).isDir()) {
                dirs << part;
            }
        }
        dirs.removeDuplicates();
        return dirs;
    }
#ifdef Q_OS_MACOS
    // 系统截图的保存位置由 com.apple.screencapture 的 location 决定，
    // 用户可能改到任意目录；读不到时按默认的桌面处理。
    QProcess defaults;
    defaults.start(QStringLiteral("/usr/bin/defaults"),
                   {QStringLiteral("read"), QStringLiteral("com.apple.screencapture"),
                    QStringLiteral("location")});
    if (defaults.waitForFinished(600) && defaults.exitCode() == 0) {
        QString location =
            QString::fromUtf8(defaults.readAllStandardOutput()).trimmed();
        if (location.startsWith(QLatin1String("~/"))) {
            location = QDir::homePath() + location.mid(1);
        }
        if (QFileInfo(location).isDir()) {
            dirs << location;
        }
    }
    dirs << QDir::homePath() + QStringLiteral("/Desktop");
    dirs << QDir::homePath() + QStringLiteral("/Pictures/Screenshots");
#else
    const QString pictures =
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString desktop =
        QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (!pictures.isEmpty()) {
        dirs << pictures + QStringLiteral("/Screenshots") << pictures;
    }
    if (!desktop.isEmpty()) {
        dirs << desktop;
    }
#endif
    dirs.removeDuplicates();
    // 去掉本程序自己的截图目录：那批文件已经走 recordCapture，
    // 再监听一遍会把同一张图挂两次。
    const QString own = QFileInfo(captureDirectory()).canonicalFilePath();
    for (int i = dirs.size() - 1; i >= 0; --i) {
        const QFileInfo info(dirs[i]);
        if (!info.isDir()) {
            dirs.removeAt(i);
            continue;
        }
        if (!own.isEmpty() && info.canonicalFilePath() == own) {
            dirs.removeAt(i);
        }
    }
    return dirs;
}

bool TrayController::looksLikeSystemCapture(const QString& fileName) {
    // 系统截图的名字随语言变化（"Screen Shot…" / "Screenshot…" /
    // "截屏…" / "屏幕快照…" / "Screenshot (3).png"）。这里只做"像不像"
    // 的宽松判断：不像的文件不自动挂，避免把用户随手放到桌面的图片
    // 当成截图（误挂比漏挂更烦人）。
    const QString name = fileName.toLower();
    static const char* const kMarkers[] = {
        "screen shot", "screenshot", "screen_shot", "screen-shot",
        "snip",  // Windows 截图工具
    };
    for (const char* marker : kMarkers) {
        if (name.contains(QLatin1String(marker))) {
            return true;
        }
    }
    const QStringList chineseMarkers = {QStringLiteral("截屏"),
                                        QStringLiteral("屏幕快照"),
                                        QStringLiteral("截图")};
    for (const QString& marker : chineseMarkers) {
        if (fileName.contains(marker)) {
            return true;
        }
    }
    return false;
}

void TrayController::setupSystemCaptureWatcher() {
    if (!systemCaptureWatchEnabled_) {
        return;
    }
    if (systemCaptureWatcher_ == nullptr) {
        systemCaptureWatcher_ = new QFileSystemWatcher(this);
        connect(systemCaptureWatcher_, &QFileSystemWatcher::directoryChanged, this,
                [this]() {
                    // 去抖：系统截图工具常"先建文件再写内容"，一次操作会
                    // 触发多个目录事件；等文件写完再扫（300ms 足够）。
                    if (systemCaptureScanTimer_ != nullptr) {
                        systemCaptureScanTimer_->start();
                    }
                });
    }
    if (systemCaptureScanTimer_ == nullptr) {
        systemCaptureScanTimer_ = new QTimer(this);
        systemCaptureScanTimer_->setSingleShot(true);
        systemCaptureScanTimer_->setInterval(300);
        connect(systemCaptureScanTimer_, &QTimer::timeout, this,
                [this]() { rescanSystemCaptures(false); });
    }
    const QStringList dirs = systemCaptureDirectories();
    if (!dirs.isEmpty()) {
        systemCaptureWatcher_->addPaths(dirs);
    }
    // 启动时先登记已有文件（initial=true 只记录已存在，不挂载）——
    // 否则用户桌面上的历史截图会被一次性全部挂上衣架。
    rescanSystemCaptures(true);
    qInfo().noquote()
        << QStringLiteral("[jie2] 系统截图监听：%1 个目录（%2）")
               .arg(dirs.size())
               .arg(dirs.join(QStringLiteral("、")));
}

void TrayController::rescanSystemCaptures(bool initial) {
    if (!systemCaptureWatchEnabled_) {
        return;
    }
    const QDateTime now = QDateTime::currentDateTime();
    for (const QString& directory : systemCaptureDirectories()) {
        QDir dir(directory);
        const QFileInfoList files = dir.entryInfoList(
            {QStringLiteral("*.png"), QStringLiteral("*.PNG"),
             QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"),
             QStringLiteral("*.tiff"), QStringLiteral("*.bmp")},
            QDir::Files, QDir::Time);
        for (const QFileInfo& info : files) {
            const QString path = info.absoluteFilePath();
            if (knownSystemCaptures_.contains(path)) {
                continue;
            }
            // 只关心"刚刚出现"的文件：2 分钟窗口足够覆盖慢速写盘，
            // 又不会把旧文件当成新截图。
            const qint64 ageSeconds = info.lastModified().secsTo(now);
            const bool fresh = ageSeconds >= -5 && ageSeconds <= 120;
            if (initial || !fresh) {
                knownSystemCaptures_.insert(path);
                continue;
            }
            if (!looksLikeSystemCapture(info.fileName())) {
                knownSystemCaptures_.insert(path);
                continue;
            }
            QImage image(path);
            if (image.isNull() || image.width() < 8 || image.height() < 8) {
                // 还没写完（或不是真图片）：**不登记**，等下一个目录事件重试。
                continue;
            }
            knownSystemCaptures_.insert(path);
            // 已在历史里就不重复挂（用户可能又重新截了同名文件）。
            bool alreadyKnown = false;
            for (const auto& entry : history_.entries()) {
                if (QString::fromStdString(entry.path) == path) {
                    alreadyKnown = true;
                    break;
                }
            }
            if (alreadyKnown) {
                continue;
            }
            // 直接挂原文件（不复制）：用户删掉文件即从衣架消失，
            // 与本程序自有截图的"移入废纸篓"语义区分开。
            lastClipboardDigest_ = imageDigest(image);
            recordCapture(path, image);
            qInfo().noquote()
                << QStringLiteral("[jie2] 系统截图已挂上衣架：%1").arg(path);
            note(QStringLiteral("截图暂存"),
                 info.fileName());
        }
    }
}

void TrayController::clipboardChanged() {
    if (!clipboardWatchEnabled_) {
        return;
    }
    if (clipboardProcessing_) {
        return;  // 防重入：本函数会重建衣架数据，重入会破坏调用栈
    }
    clipboardProcessing_ = true;
    // 用作用域守护保证任何提前 return 都能复位。
    struct Guard {
        bool* flag;
        ~Guard() { *flag = false; }
    } guard{&clipboardProcessing_};

    const QClipboard* clipboard = QApplication::clipboard();
    const QMimeData* mime = clipboard->mimeData();
    if (mime == nullptr || !mime->hasImage()) {
        return;
    }
    // tendedero 语义：同时带文本或文件的内容不算"复制了一张图"（跳过）。
    // 例：从浏览器复制图片往往附带 URL 文本 —— 这类会造成误挂。
    if (mime->hasText() || mime->hasUrls()) {
        return;
    }

    const QImage image = clipboard->image();
    if (image.isNull() || image.width() < 8 || image.height() < 8) {
        return;  // 过小的图像多半是图标/噪声
    }

    const QByteArray digest = imageDigest(image);
    if (digest.isEmpty() || digest == lastClipboardDigest_) {
        return;  // 与上次写入/挂上的是同一张
    }
    lastClipboardDigest_ = digest;

    QString error;
    const QString path =
        saveImageToDirectory(image, QStringLiteral("copied"), &error);
    if (path.isEmpty()) {
        qWarning().noquote()
            << QStringLiteral("[jie2] 剪贴板图像落盘失败：%1").arg(error);
        return;
    }
    // 挂上衣架（记入历史 + 刷新）。刻意不写剪贴板 —— 内容本来就在剪贴板上。
    recordCapture(path, image);
    qInfo().noquote() << QStringLiteral("[jie2] 剪贴板图像已挂上衣架：%1")
                             .arg(path);
    note(QStringLiteral("图片暂存"), QFileInfo(path).fileName());
}

QString TrayController::saveImageToDirectory(const QImage& image,
                                             const QString& prefix,
                                             QString* error) {
    const auto setError = [error](const QString& text) {
        if (error != nullptr) {
            *error = text;
        }
        return QString{};
    };
    if (image.isNull()) {
        return setError(QStringLiteral("空图像"));
    }
    const QString directory = captureDirectory();
    if (!QDir().mkpath(directory)) {
        return setError(QStringLiteral("无法创建截图目录：%1").arg(directory));
    }
    const QString path = directory + QLatin1Char('/') + QStringLiteral("jie2-") +
                         prefix + QLatin1Char('-') +
                         QDateTime::currentDateTime().toString(
                             QStringLiteral("yyyyMMdd-HHmmss-zzz")) +
                         QStringLiteral(".png");
    if (!image.save(path, "PNG")) {
        return setError(QStringLiteral("保存 PNG 失败：%1").arg(path));
    }
    return path;
}

// ---- 贴图（M4）----

void TrayController::triggerPin() {
    QString error;
    PinWindow* pin = PinWindow::fromClipboard(&error);
    if (pin == nullptr) {
        qWarning().noquote() << QStringLiteral("[jie2] 贴图失败：%1").arg(error);
        note(QStringLiteral("无法贴图"), error, /*warning=*/true);
        return;
    }

    // 生命周期：关闭时从列表移除（不 delete —— close 后由我们自己 delete）。
    connect(pin, &PinWindow::closed, this, [this, pin]() {
        pins_.erase(std::remove(pins_.begin(), pins_.end(), pin), pins_.end());
        pin->deleteLater();
        rebuildMenu();
    });

    // Ctrl+A 全选/取消全选（专业版 v2.4）。
    connect(pin, &PinWindow::selectAllRequested, this,
            &TrayController::toggleSelectAllPins);

    // 拖动联动：拖动某张**选中**贴图时，其余选中的一起移动。
    connect(pin, &PinWindow::movedBy, this, [this, pin](const QPoint& delta) {
        if (!pin->isSelected()) {
            return;  // 拖动的是未选中窗口：不联动
        }
        for (PinWindow* other : pins_) {
            if (other != nullptr && other != pin && other->isSelected()) {
                other->move(other->pos() + delta);
            }
        }
    });

    // 新贴图归入当前分组（专业版对齐项）。
    pin->setGroupCount(pinGroupCount_);
    connect(pin, &PinWindow::pasteRequested, this, &TrayController::triggerPin);
    connect(pin, &PinWindow::groupChanged, this, [this, pin] {
        pin->setVisible(!pinsHidden_ && pin->group() == currentPinGroup_);
        rebuildMenu();
    });
    pin->setGroup(currentPinGroup_);

    // Esc / Shift+Esc 动作（专业版 v2.8：可自定义）。
    pin->setDismissActions(pinEscAction_, pinShiftEscAction_);
    // 缩略图修饰键（专业版 v2.11：可自定义）。
    pin->setThumbnailModifier(pinThumbnailModifier_);

    pins_.push_back(pin);
    pinsHidden_ = false;
    pin->show();
    pin->raise();
    note(QStringLiteral("已贴图"),
         QStringLiteral("滚轮缩放 · 1/2 旋转 · 3/4 翻转 · Ctrl+滚轮透明度 "
                        "· 6 反色 · 双击查看局部 · Esc 关闭"));
    rebuildMenu();
}

void TrayController::triggerSaveClipboardImage() {
    // 专业版对齐项 v2.5：Ctrl+Alt+C 将剪贴板中的图像保存为文件
    // （与"贴图"共享同一套输出逻辑：装饰 / --size / 色彩标记 / 落盘，
    //   但不回写剪贴板 —— 源本来就是剪贴板内容）。
    const QClipboard* clipboard = QApplication::clipboard();
    QImage image;
    if (clipboard != nullptr && clipboard->mimeData() != nullptr &&
        clipboard->mimeData()->hasImage()) {
        image = clipboard->image();
    }
    if (image.isNull()) {
        note(QStringLiteral("没有图片"),
             QStringLiteral("请先复制图片，再使用保存图片"), /*warning=*/true);
        return;
    }
    QString error;
    const QString path = saveAndCopy(image, &error, /*writeClipboard=*/false);
    if (path.isEmpty() || path == QStringLiteral("(clipboard)")) {
        note(QStringLiteral("保存失败"), error, /*warning=*/true);
        return;
    }
    qInfo().noquote() << QStringLiteral("[jie2] 已保存剪贴板图像：%1").arg(path);
    note(QStringLiteral("保存完成"), path);
}

void TrayController::toggleHidePins() {
    if (pins_.empty()) {
        note(QStringLiteral("没有贴图"), QStringLiteral("请先用“贴到屏幕”添加贴图"));
        return;
    }
    pinsHidden_ = !pinsHidden_;
    for (PinWindow* pin : pins_) {
        // 显示时仍然遵守分组：只显示当前组的（分组是"看哪一组"，
        // 隐藏全部是"全都不看"，两者正交）。
        pin->setVisible(!pinsHidden_ && pin->group() == currentPinGroup_);
    }
    qInfo().noquote() << QStringLiteral("[jie2] 贴图%1（共 %2 张）")
                             .arg(pinsHidden_ ? QStringLiteral("已隐藏")
                                              : QStringLiteral("已显示"))
                             .arg(pins_.size());
    rebuildMenu();
}

void TrayController::closeAllPins() {
    if (pins_.empty()) {
        return;
    }
    // 复制一份再遍历：closed 信号会就地修改 pins_。
    const std::vector<PinWindow*> closing = pins_;
    for (PinWindow* pin : closing) {
        pin->close();
    }
    qInfo().noquote() << QStringLiteral("[jie2] 已关闭全部贴图（%1 张）")
                             .arg(closing.size());
}

int TrayController::createTestPins(int count) {
    int created = 0;
    for (int i = 0; i < count; ++i) {
        // 每张用不同颜色，便于人工核对；尺寸固定小图。
        QImage image(80, 60, QImage::Format_ARGB32);
        const int shade = 40 + (created * 60) % 200;
        image.fill(QColor(shade, 120, 255 - shade % 200));
        auto* pin = new PinWindow(image);
        connect(pin, &PinWindow::closed, this, [this, pin]() {
            pins_.erase(std::remove(pins_.begin(), pins_.end(), pin), pins_.end());
            pin->deleteLater();
            rebuildMenu();
        });
        pin->setGroup(currentPinGroup_);
        pins_.push_back(pin);
        pin->show();
        ++created;
    }
    return created;
}

int TrayController::visiblePinCount() const {
    int count = 0;
    for (PinWindow* pin : pins_) {
        if (pin != nullptr && pin->isVisible()) {
            ++count;
        }
    }
    return count;
}

int TrayController::currentGroupPinCount() const {
    int count = 0;
    for (PinWindow* pin : pins_) {
        if (pin != nullptr && pin->group() == currentPinGroup_) {
            ++count;
        }
    }
    return count;
}

int TrayController::emptyCurrentPinGroup() {
    // 先收集再关闭：close 会触发 closed 信号就地修改 pins_。
    std::vector<PinWindow*> closing;
    for (PinWindow* pin : pins_) {
        if (pin != nullptr && pin->group() == currentPinGroup_) {
            closing.push_back(pin);
        }
    }
    for (PinWindow* pin : closing) {
        pin->close();
    }
    qInfo().noquote() << QStringLiteral("[jie2] 已清空贴图第 %1 组（%2 张）")
                             .arg(currentPinGroup_ + 1)
                             .arg(closing.size());
    if (!closing.empty()) {
    note(QStringLiteral("已清空"),
             QStringLiteral("第 %1 组关闭了 %2 张")
                 .arg(currentPinGroup_ + 1)
                 .arg(closing.size()));
    }
    rebuildMenu();
    return static_cast<int>(closing.size());
}

void TrayController::showPinGroupManager() {
    qInfo().noquote() << QStringLiteral("[jie2] 打开贴图分组管理");
    // 管理对话框放栈上：exec() 期间主事件循环继续运行（嵌套事件循环），
    // 单实例通道与其它菜单动作不受影响。
    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("贴图分组"));
    auto* layout = new QVBoxLayout(&dialog);

    auto* hint = new QLabel(
        QStringLiteral("切换分组后，只显示该组的贴图。"), &dialog);
    hint->setStyleSheet(QStringLiteral("color: #888;"));
    layout->addWidget(hint);

    const int groupCount = std::max(1, pinGroupCount_);
    for (int group = 0; group < groupCount; ++group) {
        int count = 0;
        for (PinWindow* pin : pins_) {
            if (pin != nullptr && pin->group() == group) {
                ++count;
            }
        }
        auto* row = new QHBoxLayout();
        auto* label = new QLabel(
            QStringLiteral("第 %1 组：%2 张%3")
                .arg(group + 1)
                .arg(count)
                .arg(group == currentPinGroup_ ? QStringLiteral("（当前）")
                                               : QString()),
            &dialog);
        row->addWidget(label, /*stretch=*/1);

        auto* switchButton = new QPushButton(QStringLiteral("切换分组"), &dialog);
        switchButton->setEnabled(group != currentPinGroup_);
        connect(switchButton, &QPushButton::clicked, &dialog,
                [this, &dialog, group]() {
                    switchPinGroup(group);
                    dialog.accept();
                });
        row->addWidget(switchButton);

        auto* emptyButton = new QPushButton(QStringLiteral("清空该组"), &dialog);
        emptyButton->setEnabled(count > 0);
        connect(emptyButton, &QPushButton::clicked, &dialog,
                [this, &dialog, group, count]() {
                    // 清空任意组（不限于当前组）：临时切到该组再清空再切回，
                    // 复用同一条清空链路；若本来就是当前组则无需切换。
                    const int previous = currentPinGroup_;
                    if (previous != group) {
                        switchPinGroup(group);
                    }
                    emptyCurrentPinGroup();
                    if (previous != group && !pins_.empty()) {
                        switchPinGroup(previous);
                    }
                    dialog.accept();
                    qInfo().noquote() << QStringLiteral("[jie2] 管理窗口：清空第 %1 组（%2 张）")
                                             .arg(group + 1)
                                             .arg(count);
                });
        row->addWidget(emptyButton);

        layout->addLayout(row);
    }

    auto* closeRow = new QHBoxLayout();
    closeRow->addStretch(1);
    auto* closeButton = new QPushButton(QStringLiteral("关闭"), &dialog);
    connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    closeRow->addWidget(closeButton);
    layout->addLayout(closeRow);

    dialog.exec();
}

void TrayController::activateExistingInstance() {
    qInfo().noquote() << QStringLiteral("[jie2] 唤起已有实例");
    note(QStringLiteral("已经启动"),
         QStringLiteral("点击菜单栏图标，即可截图或贴图"));
}

void TrayController::toggleSelectAllPins() {
    if (pins_.empty()) {
        return;
    }
    // toggle 语义：只要还有未选中的就全选，否则全部取消。
    const bool anyUnselected = std::any_of(
        pins_.begin(), pins_.end(),
        [](PinWindow* pin) { return pin != nullptr && !pin->isSelected(); });
    for (PinWindow* pin : pins_) {
        if (pin != nullptr) {
            pin->setSelected(anyUnselected);
        }
    }
    qInfo().noquote() << QStringLiteral("[jie2] 贴图%1（%2 张）%3")
                             .arg(anyUnselected ? QStringLiteral("全选")
                                                : QStringLiteral("取消全选"))
                             .arg(pins_.size())
                             .arg(anyUnselected
                                      ? QStringLiteral("—— 拖动任一即整体移动")
                                      : QString());
    note(anyUnselected ? QStringLiteral("全部选中")
                       : QStringLiteral("取消全选"),
         QStringLiteral("%1 张").arg(pins_.size()));
}

void TrayController::switchPinGroup(int group) {
    if (pins_.empty()) {
        note(QStringLiteral("没有贴图"), QStringLiteral("先贴一张再切分组"));
        return;
    }
    // -1 = 下一组（循环）。
    const int count = std::max(1, pinGroupCount_);
    if (group < 0) {
        currentPinGroup_ = (currentPinGroup_ + 1) % count;
    } else {
        currentPinGroup_ = group % count;
    }
    // 只显示当前组的贴图；隐藏其余组（保留在内存与列表）。
    // 若用户之前按了"隐藏全部"，切分组不解除那个状态。
    int shown = 0;
    for (PinWindow* pin : pins_) {
        if (pin == nullptr) {
            continue;
        }
        const bool visible =
            !pinsHidden_ && pin->group() == currentPinGroup_;
        pin->setVisible(visible);
        if (visible) {
            ++shown;
        }
    }
    qInfo().noquote() << QStringLiteral("[jie2] 贴图分组：切到第 %1 组"
                                        "（本组 %2 张 / 共 %3 张）")
                             .arg(currentPinGroup_ + 1)
                             .arg(shown)
                             .arg(pins_.size());
    note(QStringLiteral("贴图分组"),
         QStringLiteral("当前第 %1 组，显示 %2 张")
             .arg(currentPinGroup_ + 1)
             .arg(shown));
    rebuildMenu();
}

void TrayController::saveAllPins() {
    if (pins_.empty()) {
        note(QStringLiteral("没有贴图"), QStringLiteral("先贴一张再保存"));
        return;
    }
    const QString directory = captureDirectory();
    if (!QDir().mkpath(directory)) {
        note(QStringLiteral("保存失败"),
             QStringLiteral("无法创建目录：%1").arg(directory), /*warning=*/true);
        return;
    }
    int saved = 0;
    for (PinWindow* pin : pins_) {
        if (pin == nullptr) {
            continue;
        }
        // 文件名带序号，避免同一秒内的多张互相覆盖。
        const QString path =
            directory +
            QStringLiteral("/jie2-pin-%1-%2.png")
                .arg(QDateTime::currentDateTime().toString(
                    QStringLiteral("yyyyMMdd-HHmmss-zzz")))
                .arg(saved + 1, 2, 10, QLatin1Char('0'));
        if (pin->saveTo(path)) {
            ++saved;
        }
    }
    qInfo().noquote() << QStringLiteral("[jie2] 批量保存贴图：%1/%2 张")
                             .arg(saved)
                             .arg(pins_.size());
    note(QStringLiteral("保存完成"),
         QStringLiteral("已保存 %1 张到 %2").arg(saved).arg(directory));
}

// ---- 文字识别（M5）----

core::OcrResult TrayController::runOcrOnImage(const QImage& image) {
    if (image.isNull()) {
        return core::OcrResult::failure("图像为空");
    }
    const core::ImageBuffer buffer = fromQImage(image);
    core::OcrResult result =
        OcrRunner::run(ocrConfig_, buffer, platform_.get());
    qInfo().noquote() << QStringLiteral("[jie2] OCR 后端=%1 结果=%2 文本长度=%3")
                             .arg(QString::fromStdString(result.backend))
                             .arg(result.ok ? QStringLiteral("成功")
                                            : QStringLiteral("失败"))
                             .arg(result.text.size());
    if (!result.ok) {
        qWarning().noquote() << QStringLiteral("[jie2] OCR 失败原因：%1")
                                    .arg(QString::fromStdString(result.error));
    }
    return result;
}

void TrayController::triggerBarcodeScan() {
    // 来源：剪贴板图像（截图后内容即在此），与 OCR 同一入口习惯。
    const QImage image = QApplication::clipboard()->image();
    if (image.isNull()) {
        note(QStringLiteral("无法识别"),
             QStringLiteral("请先截取或复制一张带码的图片"),
             /*warning=*/true);
        return;
    }
    const core::OcrResult result = OcrRunner::scanBarcode(fromQImage(image));
    if (!result.ok) {
        note(QStringLiteral("扫码失败"), QString::fromStdString(result.error),
             /*warning=*/true);
        return;
    }
    if (result.text.empty()) {
        note(QStringLiteral("未找到码"),
             QStringLiteral("图片中未找到二维码或条形码"));
        return;
    }
    QApplication::clipboard()->setText(QString::fromStdString(result.text));
    qInfo().noquote() << QStringLiteral("[jie2] 扫码成功（%1 条）")
                             .arg(QString::fromStdString(result.text)
                                      .count(QLatin1Char('\n')) +
                                  1);
    note(QStringLiteral("扫码完成"),
         QString::fromStdString(result.text).left(160));
}

void TrayController::triggerOcr() {
    if (serviceInProgress_) return;
    QScopedValueRollback<bool> busy(serviceInProgress_, true);
    if (ocrConfig_.isCloud() && (ocrConfig_.endpoint.empty() || ocrConfig_.model.empty())) {
        note("连接服务", "请打开“设置 → 提取文字”连接服务", true);
        return;
    }
    // 来源：剪贴板图像（截图后内容即在此）。
    const QImage image = QApplication::clipboard()->image();
    if (image.isNull()) {
        note(QStringLiteral("无法识别"),
             QStringLiteral("请先截图或复制一张图片"),
             /*warning=*/true);
        return;
    }

    // 云端后端必须显式确认（评估报告 4.4 第二条原则：联网需显式同意）。
    if (ocrConfig_.isCloud()) {
        QMessageBox confirmation(QMessageBox::Question, QStringLiteral("发送图片"),
            QStringLiteral("将图片发送到以下服务以提取文字：\n%1")
                .arg(QString::fromStdString(ocrConfig_.endpoint)),
            QMessageBox::Yes | QMessageBox::No);
        confirmation.setTextFormat(Qt::PlainText);
        confirmation.button(QMessageBox::Yes)->setText(QStringLiteral("发送图片"));
        confirmation.button(QMessageBox::No)->setText(QStringLiteral("取消"));
        confirmation.setDefaultButton(QMessageBox::No);
        if (confirmation.exec() != QMessageBox::Yes) {
            qInfo().noquote() << QStringLiteral("[jie2] 云端 OCR 被用户取消");
            return;
        }
    }

    const core::OcrResult result = runOcrOnImage(image);
    if (!result.ok) {
        note(QStringLiteral("识别失败"),
             QString::fromStdString(result.error), /*warning=*/true);
        return;
    }
    if (result.text.empty()) {
        note(QStringLiteral("识别完成"),
             QStringLiteral("这张图片中未识别到文字"));
        return;
    }

    QApplication::clipboard()->setText(QString::fromStdString(result.text));
    const QString preview = QString::fromStdString(result.text).left(120);
    note(QStringLiteral("复制完成"),
         preview);
}

void TrayController::note(const QString& title, const QString& body, bool warning) {
    if (warning) {
        // 常驻应用的系统通知可能被权限或专注模式隐藏，错误必须可见。
        auto* box = new QMessageBox(QMessageBox::Warning, title, body,
                                    QMessageBox::Ok);
        box->setTextFormat(Qt::PlainText);
        box->button(QMessageBox::Ok)->setText(QStringLiteral("知道了"));
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->setWindowModality(Qt::ApplicationModal);
        box->show();
        box->raise();
        box->activateWindow();
        return;
    }
    if (tray_ != nullptr && tray_->isSystemTrayAvailable()) {
        tray_->showMessage(title, body,
                           warning ? QSystemTrayIcon::Warning
                                   : QSystemTrayIcon::Information,
                           warning ? 5000 : 3000);
    }
}

void TrayController::showCapabilities() {
    // 重新探测一次再展示，避免展示过期的缓存状态。
    capabilities_ = platform_->probe();
    auto status = [](core::CapabilityStatus value) -> QString {
        switch (value) {
        case core::CapabilityStatus::Available: return QStringLiteral("可用");
        case core::CapabilityStatus::Degraded: return QStringLiteral("部分可用");
        case core::CapabilityStatus::Unavailable: return QStringLiteral("暂不可用");
        case core::CapabilityStatus::NotApplicable: return QStringLiteral("无需检查");
        default: return QStringLiteral("尚未确认");
        }
    };
    QString text = QStringLiteral("查看当前能使用的功能。\n\n屏幕截图：%1\n快捷按键：%2\n贴图置顶：%3\n复制图片：%4")
        .arg(status(capabilities_.screenCapture.status), status(capabilities_.globalHotkey.status),
             status(capabilities_.alwaysOnTop.status), status(capabilities_.clipboardImage.status));
    if (capabilities_.session == core::SessionType::Wayland)
        text += QStringLiteral("\n图片暂存：%1").arg(status(capabilities_.layerShell.status));
    const auto elementAccess = platform_->createElementLocator();
    if (elementAccess.accessibilityRequired || elementAccess.rectanglesAt)
        text += elementAccess.accessibilityRequired ? QStringLiteral("\n按钮识别：需允许辅助功能")
                                                    : QStringLiteral("\n按钮识别：可用");
    text += QStringLiteral("\n\n系统：%1").arg(QString::fromStdString(platform_->name()));
    auto* box = new QMessageBox(QMessageBox::Information, QStringLiteral("功能检查"), text, QMessageBox::Ok);
    box->button(QMessageBox::Ok)->setText(QStringLiteral("完成"));
    if (elementAccess.accessibilityRequired) {
        auto* allow = box->addButton(QStringLiteral("允许识别"), QMessageBox::ActionRole);
        connect(allow, &QPushButton::clicked, this, [this] { platform_->requestElementAccess(); });
    }
    box->setTextFormat(Qt::PlainText);
    box->setDetailedText(QString::fromStdString(capabilities_.describe()));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::ApplicationModal);
    box->show();
    box->raise();
    box->activateWindow();
}

void TrayController::showSettings() {
    if (settingsOpen_) return;
    QScopedValueRollback<bool> opened(settingsOpen_, true);
    saveConfig();
    platform_->releaseAllHotkeys();
    const auto restoreHotkeys = qScopeGuard([this] { registerHotkey(); });
    SettingsDialog dialog(SettingsDialog::readConfig(configPath_));
    for (;;) {
        if (dialog.exec() != QDialog::Accepted) return;
        QString error;
        if (SettingsDialog::writeConfig(configPath_, dialog.configuration(), &error)) break;
        QMessageBox::warning(&dialog, "保存失败", "无法保存设置：" + error);
    }
    loadConfig();
    setupTriggerCorner();
    if (clothesline_) {
        clothesline_->setDarkTheme(darkTheme_);
        clothesline_->setWatchEnabled(clotheslineEnabled_);
    } else if (capabilities_.session == core::SessionType::Wayland) {
        waylandLineEnabled_ = clotheslineEnabled_;
        if (waylandLineEnabled_) updateWaylandLine(); else stopWaylandLine();
    }
    for (auto* pin : pins_) {
        pin->setDismissActions(pinEscAction_, pinShiftEscAction_);
        pin->setThumbnailModifier(pinThumbnailModifier_);
    }
    // 全局快捷键可能被改过：注销后按新组合重新注册（loadConfig 已解析新值）。
    // 系统截图监听开关也可能变了：关闭时摘掉目录监听，重新开启时重建。
    if (systemCaptureWatcher_ != nullptr) {
        const QStringList watched = systemCaptureWatcher_->directories();
        if (!watched.isEmpty()) {
            systemCaptureWatcher_->removePaths(watched);
        }
    }
    if (systemCaptureWatchEnabled_) {
        setupSystemCaptureWatcher();
    }
    rebuildMenu();
}

void TrayController::triggerTranslation() {
    if (serviceInProgress_) return;
    QScopedValueRollback<bool> busy(serviceInProgress_, true);
    auto config = SettingsDialog::readConfig(configPath_);
    core::OcrConfig service;
    service.endpoint = config.get("translation.endpoint");
    service.model = config.get("translation.model");
    service.apiKey = config.get("translation.api_key");
    service.timeoutMs = std::clamp(config.getInt("translation.timeout_ms", 60000), 1000, 300000);
    if (service.endpoint.empty() || service.model.empty()) {
        note("连接服务", "请打开“设置 → 翻译内容”连接服务", true); return;
    }
    auto* clipboard = QApplication::clipboard();
    const QImage image = clipboard->image();
    const QString text = image.isNull() ? clipboard->text() : QString();
    if (image.isNull() && text.trimmed().isEmpty()) {
        note("无法翻译", "请先复制文字或图片", true); return;
    }
    QMessageBox confirmation(QMessageBox::Question, QStringLiteral("发送内容"),
        QStringLiteral("将已复制的%1发送到以下服务翻译：\n%2")
            .arg(image.isNull() ? "文字" : "图片", QString::fromStdString(service.endpoint)),
        QMessageBox::Yes | QMessageBox::No);
    confirmation.setTextFormat(Qt::PlainText);
    confirmation.button(QMessageBox::Yes)->setText(QStringLiteral("发送内容"));
    confirmation.button(QMessageBox::No)->setText(QStringLiteral("取消"));
    confirmation.setDefaultButton(QMessageBox::No);
    if (confirmation.exec() != QMessageBox::Yes) return;
    const auto result = OcrRunner::translate(service, image, text,
        QString::fromStdString(config.get("translation.target_language", "简体中文")));
    if (!result.ok) { note("翻译失败", QString::fromStdString(result.error), true); return; }
    clipboard->setText(QString::fromStdString(result.text));
    auto* box = new QMessageBox(QMessageBox::Information, "翻译完成",
        QString::fromStdString(result.text), QMessageBox::Ok);
    box->setInformativeText(QStringLiteral("译文已自动复制"));
    box->button(QMessageBox::Ok)->setText(QStringLiteral("完成"));
    box->setTextFormat(Qt::PlainText);
    box->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    box->setAttribute(Qt::WA_DeleteOnClose); box->show(); box->raise(); box->activateWindow();
}

void TrayController::registerHotkey() {
    // 三个全局热键都走可自定义配置（设置 → 快捷键 → 全局快捷键）。
    // 注册前先注销：改完快捷键后重新注册，必须避免旧组合继续占着。
    platform_->releaseAllHotkeys();

    // 截图热键。
    std::string error;
    hotkeyRegistered_ = platform_->grabHotkey(
        snipHotkeySpec_, &TrayController::hotkeyThunk, this, &error);
    hotkeyError_ = QString::fromStdString(error);
    if (hotkeyRegistered_) {
        qInfo().noquote() << QStringLiteral("[jie2] 截图热键已注册：%1")
                                 .arg(QString::fromStdString(
                                     platform::displayHotkey(snipHotkeySpec_)));
    } else {
        qWarning().noquote()
            << QStringLiteral("[jie2] 截图热键注册失败：%1").arg(hotkeyError_);
#if defined(Q_OS_LINUX)
        // 银河麒麟/Linux 容错：若配置的快捷键（如 Ctrl+Alt+T）被系统终端占用，
        // 自动尝试回退注册 F1 兜底，避免用户完全无法唤起截图。
        if (snipHotkeySpec_.key != platform::Key::F1) {
            platform::HotkeySpec fallbackSpec;
            fallbackSpec.id = snipHotkeySpec_.id;
            fallbackSpec.modifiers = 0;
            fallbackSpec.key = platform::Key::F1;
            std::string fallbackErr;
            if (platform_->grabHotkey(fallbackSpec, &TrayController::hotkeyThunk, this, &fallbackErr)) {
                snipHotkeySpec_ = fallbackSpec;
                hotkeyRegistered_ = true;
                hotkeyError_.clear();
                qInfo().noquote() << QStringLiteral("[jie2] 原截图热键被占用，已自动启用备用热键 F1");
                if (tray_ != nullptr && tray_->isSystemTrayAvailable()) {
                    tray_->showMessage(QStringLiteral("按键已更换"),
                                       QStringLiteral("原按键被占用，已改用 F1 截图"),
                                       QSystemTrayIcon::Information, 5000);
                }
            }
        }
#endif
        if (!hotkeyRegistered_ && tray_ != nullptr && tray_->isSystemTrayAvailable()) {
            tray_->showMessage(QStringLiteral("按键不可用"),
                               QStringLiteral("快捷键被占用，请在设置中更换"),
                               QSystemTrayIcon::Warning, 5000);
        }
    }

    // 贴图热键。
    std::string pinError;
    pinHotkeyRegistered_ = platform_->grabHotkey(
        pinHotkeySpec_, &TrayController::hotkeyThunk, this, &pinError);
    pinHotkeyError_ = QString::fromStdString(pinError);
    if (pinHotkeyRegistered_) {
        qInfo().noquote() << QStringLiteral("[jie2] 贴图热键已注册：%1")
                                 .arg(QString::fromStdString(
                                     platform::displayHotkey(pinHotkeySpec_)));
    } else {
        qWarning().noquote()
            << QStringLiteral("[jie2] 贴图热键注册失败：%1").arg(pinHotkeyError_);
    }

    // 保存剪贴板图像热键（专业版对齐项 v2.5）。
    std::string saveClipError;
    saveClipboardHotkeyRegistered_ = platform_->grabHotkey(
        saveClipboardHotkeySpec_, &TrayController::hotkeyThunk, this,
        &saveClipError);
    if (saveClipboardHotkeyRegistered_) {
        qInfo().noquote()
            << QStringLiteral("[jie2] 保存剪贴板热键已注册：%1")
                   .arg(QString::fromStdString(
                       platform::displayHotkey(saveClipboardHotkeySpec_)));
    } else {
        qWarning().noquote()
            << QStringLiteral("[jie2] 保存剪贴板热键注册失败：%1")
                   .arg(QString::fromStdString(saveClipError));
    }

    if (menu_ != nullptr) {
        rebuildMenu();
    }
}

void TrayController::hotkeyThunk(const std::string& id, void* userData) {
    auto* self = static_cast<TrayController*>(userData);
    if (self == nullptr) {
        return;
    }
    // 回调可能来自平台事件线程（Linux X11 后端即是），必须投递回主线程。
    const QString idCopy = QString::fromStdString(id);
    QMetaObject::invokeMethod(
        self,
        [self, idCopy]() { self->handleHotkey(idCopy.toStdString()); },
        Qt::QueuedConnection);
}

void TrayController::handleHotkey(const std::string& id) {
    if (id == kPinHotkeyId) {
        qInfo().noquote() << QStringLiteral("[jie2] 热键触发贴图");
        triggerPin();
        return;
    }
    if (id == kSaveClipboardHotkeyId) {
        qInfo().noquote() << QStringLiteral("[jie2] 热键触发：保存剪贴板图像为文件");
        triggerSaveClipboardImage();
        return;
    }
    if (id != kHotkeyId) {
        return;
    }
    ++hotkeyCount_;
    saveConfig();
    qInfo().noquote()
        << QStringLiteral("[jie2] 热键触发截图（第 %1 次）").arg(hotkeyCount_);
    triggerCapture();
}

void TrayController::applyDefaultGlobalHotkeys() {
    const auto apply = [](const char* keyName, const char* id,
                          platform::HotkeySpec* spec, QString* text) {
        const QString fallback = defaultGlobalHotkey(keyName);
        platform::HotkeySpec parsed;
        parsed.id = id;
        platform::parseHotkeyText(fallback.toStdString(), &parsed);
        *spec = parsed;
        *text = fallback;
    };
    apply("t", kHotkeyId, &snipHotkeySpec_, &snipHotkeyText_);
    apply("p", kPinHotkeyId, &pinHotkeySpec_, &pinHotkeyText_);
    apply("c", kSaveClipboardHotkeyId, &saveClipboardHotkeySpec_,
          &saveClipboardHotkeyText_);
}

void TrayController::loadConfig() {
    // 全局快捷键先按出厂默认填好，再由配置文件覆盖。
    // 必须放在读文件**之前**：首次运行时配置文件还不存在，函数会在下面
    // 提前 return —— 那时若规格仍是空值，注册出去的就是"无主键"组合，
    // 表现为热键直接不可用（这个缺陷在改成可配置时引入过）。
    applyDefaultGlobalHotkeys();
    QFile file(configPath_);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }
    const QByteArray raw = file.readAll();
    const core::Config config =
        core::Config::parse(std::string(raw.constData(), raw.size()));
    hotkeyCount_ = config.getInt("hotkey.count", 0);
    captureCount_ = config.getInt("capture.count", 0);

    // 全局快捷键（设置 → 快捷键 → 全局快捷键，可自定义）。
    // 解析失败（用户手改配置写错）时如实告警并回落到出厂默认 ——
    // 不能因为一行配置写错就让截图热键彻底失效。
    const auto loadGlobalHotkey =
        [&config](const char* key, const char* fallbackKeyName,
                  const char* id, platform::HotkeySpec* spec,
                  QString* text) {
            const QString fallback = defaultGlobalHotkey(fallbackKeyName);
            const std::string value =
                config.get(key, fallback.toStdString());
            platform::HotkeySpec parsed;
            parsed.id = id;
            if (platform::parseHotkeyText(value, &parsed)) {
                *spec = parsed;
                *text = QString::fromStdString(value);
                return;
            }
            qWarning().noquote()
                << QStringLiteral("[jie2] 全局快捷键配置无效（%1=%2），改用默认 %3")
                       .arg(QString::fromUtf8(key))
                       .arg(QString::fromStdString(value), fallback);
            platform::parseHotkeyText(fallback.toStdString(), &parsed);
            parsed.id = id;
            *spec = parsed;
            *text = fallback;
        };
    loadGlobalHotkey("hotkey.snip", "t", kHotkeyId, &snipHotkeySpec_,
                     &snipHotkeyText_);
    loadGlobalHotkey("hotkey.pin", "p", kPinHotkeyId, &pinHotkeySpec_,
                     &pinHotkeyText_);
    loadGlobalHotkey("hotkey.save_clipboard", "c", kSaveClipboardHotkeyId,
                     &saveClipboardHotkeySpec_, &saveClipboardHotkeyText_);

    // 输出装饰（M1）
    roundEnabled_ = config.getBool("output.round.enabled", false);
    roundRadius_ = config.getInt("output.round.radius", 8);
    borderEnabled_ = config.getBool("output.border.enabled", false);
    borderWidth_ = config.getInt("output.border.width", 1);
    const QString borderColor =
        QString::fromStdString(config.get("output.border.color", "#d2d2d2"));
    if (QColor(borderColor).isValid()) {
        borderColor_ = QColor(borderColor);
    }
    shadowEnabled_ = config.getBool("output.shadow.enabled", false);

    // 触发角（M1）
    triggerEnabled_ = config.getBool("trigger.enabled", false);
    triggerSize_ = config.getInt("trigger.size", 6);
    triggerDwellMs_ = config.getInt("trigger.dwell_ms", 350);
    triggerCorners_ =
        QString::fromStdString(config.get("trigger.corners", "all"));

    // 衣架（M3）
    clotheslineEnabled_ = config.getBool("clothesline.enabled", true);
    clipboardWatchEnabled_ = config.getBool("clothesline.hang_copied", true);
    // 系统截图（Cmd+Shift+4 等）直接落盘，需要单独一路目录监听。
    systemCaptureWatchEnabled_ =
        config.getBool("clothesline.hang_system_capture", true);

    // 标注调色板（M2 增补，专业版对齐项）
    paletteSize_ = config.getInt("annotation.palette_size", 6);

    // 主题（v2.6：主题色跟随系统主题）：解析一次，全 UI 复用。
    // 优先级：配置显式值 > 环境变量探针（测试）> 系统探测 > 深色默认。
    themeSetting_ =
        QString::fromStdString(config.get("appearance.theme", "system"));
    const ThemeResolution theme = resolveTheme(themeSetting_);
    darkTheme_ = theme.dark;
    qInfo().noquote() << QStringLiteral("[jie2] 主题：%1（来源：%2）")
                             .arg(darkTheme_ ? QStringLiteral("dark")
                                             : QStringLiteral("light"))
                             .arg(theme.source);

    // 自定义应用内快捷键（v2.11.3）。
    std::string shortcutWarning;
    shortcutMap_ = core::ShortcutMap::fromConfig(config, &shortcutWarning);
    if (!shortcutWarning.empty()) {
        qWarning().noquote()
            << QStringLiteral("[jie2] %1")
                   .arg(QString::fromStdString(shortcutWarning));
    }

    // 橡皮擦删除可撤销（v2.6：annotation.eraser_disable_reedit =
    // 对橡皮擦禁用二次编辑）。
    eraserUndoable_ =
        !config.getBool("annotation.eraser_disable_reedit", false);

    // 贴图 Esc 行为（专业版对齐项 v2.8：可自定义 Esc / Shift+Esc 的功能）。
    // 值："close"（关闭，从列表移除）| "hide"（隐藏，可由托盘恢复）。
    {
        const std::string escAction = config.get("pin.esc_action", "close");
        const std::string shiftEscAction =
            config.get("pin.shift_esc_action", "hide");
        pinEscAction_ = escAction == "hide" ? PinWindow::DismissAction::Hide
                                            : PinWindow::DismissAction::Close;
        pinShiftEscAction_ =
            shiftEscAction == "hide" ? PinWindow::DismissAction::Hide
                                     : PinWindow::DismissAction::Close;
    }

    // 缩略图修饰键（专业版对齐项 v2.11：可自定义缩略图快捷键）。
    // 值："shift"（默认）| "ctrl" | "alt"。
    {
        const std::string modifier = config.get("pin.thumbnail_modifier", "shift");
        if (modifier == "ctrl") {
            pinThumbnailModifier_ = Qt::ControlModifier;
        } else if (modifier == "alt") {
            pinThumbnailModifier_ = Qt::AltModifier;
        } else {
            pinThumbnailModifier_ = Qt::ShiftModifier;
        }
    }

    // OCR（M5）
    std::string ocrWarning;
    ocrConfig_ = core::parseOcrConfig(config, &ocrWarning);
    ocrConfig_.backend = core::OcrBackendKind::OpenAiCompatible;
    if (!ocrWarning.empty()) {
        qWarning().noquote() << QStringLiteral("[jie2] %1")
                                    .arg(QString::fromStdString(ocrWarning));
    }
}

void TrayController::saveConfig() {
    core::Config config = SettingsDialog::readConfig(configPath_);
    config.set("hotkey.count", std::to_string(hotkeyCount_));
    config.set("capture.count", std::to_string(captureCount_));
    config.set("hotkey.id", kHotkeyId);
    config.set("platform", platform_->name());
    // 全局快捷键文本（用户设定值原样回写，不写解析结果）。
    config.set("hotkey.snip", snipHotkeyText_.toStdString());
    config.set("hotkey.pin", pinHotkeyText_.toStdString());
    config.set("hotkey.save_clipboard", saveClipboardHotkeyText_.toStdString());

    // 输出装饰（M1）
    config.set("output.round.enabled", roundEnabled_ ? "true" : "false");
    config.set("output.round.radius", std::to_string(roundRadius_));
    config.set("output.border.enabled", borderEnabled_ ? "true" : "false");
    config.set("output.border.width", std::to_string(borderWidth_));
    config.set("output.border.color", borderColor_.name().toStdString());
    config.set("output.shadow.enabled", shadowEnabled_ ? "true" : "false");

    // 触发角（M1）
    config.set("trigger.enabled", triggerEnabled_ ? "true" : "false");
    config.set("trigger.size", std::to_string(triggerSize_));
    config.set("trigger.dwell_ms", std::to_string(triggerDwellMs_));
    config.set("trigger.corners", triggerCorners_.toStdString());

    // 衣架（M3）
    config.set("clothesline.enabled", clotheslineEnabled_ ? "true" : "false");
    config.set("clothesline.hang_copied",
               clipboardWatchEnabled_ ? "true" : "false");
    config.set("clothesline.hang_system_capture",
               systemCaptureWatchEnabled_ ? "true" : "false");

    // 标注调色板（M2 增补）
    config.set("annotation.palette_size", std::to_string(paletteSize_));

    // 主题（v2.6）：回写用户设定值（system/dark/light），不写解析结果。
    config.set("appearance.theme", themeSetting_.toStdString());

    // 从已有配置合并，保留密钥、翻译和快捷键等设置；空值也要正确覆盖旧值。
    config.set("ocr.backend", core::toString(ocrConfig_.backend));
    config.set("ocr.language", ocrConfig_.language);
    config.set("ocr.timeout_ms", std::to_string(ocrConfig_.timeoutMs));
    config.set("ocr.endpoint", ocrConfig_.endpoint);
    config.set("ocr.model", ocrConfig_.model);
    config.set("ocr.api_key", ocrConfig_.apiKey);
    QString error;
    if (!SettingsDialog::writeConfig(configPath_, config, &error))
        qWarning().noquote() << QStringLiteral("[jie2] 配置写入失败：%1").arg(error);

}

// ---- 截图历史（M1：, / . 回放）----

void TrayController::loadHistory() {
    QFile file(historyFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }
    const QByteArray raw = file.readAll();
    history_ = core::CaptureHistory::parse(std::string(raw.constData(), raw.size()),
                                           history_.capacity());
}

void TrayController::saveHistory() {
    QFile file(historyFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        qWarning().noquote()
            << QStringLiteral("[jie2] 历史写入失败：%1").arg(historyFilePath());
        return;
    }
    const std::string text = history_.serialize();
    file.write(text.data(), static_cast<qint64>(text.size()));
}

void TrayController::recordCapture(const QString& path, const QImage& image) {
    if (path.isEmpty() || image.isNull()) {
        return;
    }
    core::HistoryEntry entry;
    entry.path = path.toStdString();
    entry.timestamp =
        QDateTime::currentDateTime()
            .toString(QStringLiteral("yyyyMMdd-HHmmss"))
            .toStdString();
    entry.width = image.width();
    entry.height = image.height();
    history_.append(std::move(entry));
    saveHistory();
    // 新截图立刻挂上衣架（tendedero 的核心体验：截完就在线上）。
    refreshClothesline();
    // 飞入动效（tendedero CaptureFlight）：从被截取的区域飞上衣架，
    // 面板未展开时会先探出承接。无源区域的路径（如剪贴板挂载）
    // 走挂载掉落动画，不飞。
    if (clothesline_ != nullptr && !pendingFlyRect_.isEmpty()) {
        clothesline_->flyCapture(path, pendingFlyRect_);
    }
    pendingFlyRect_ = QRect();
}

void TrayController::setCaptureOverrides(const CaptureOverrides& overrides) {
    overrides_ = overrides;
    if (!overrides.outputPath.isEmpty() || overrides.noSave) {
        qInfo().noquote() << QStringLiteral("[jie2] 命令行覆盖：输出=%1 仅剪贴板=%2")
                                 .arg(overrides.outputPath.isEmpty()
                                          ? QStringLiteral("(默认)")
                                          : overrides.outputPath)
                                 .arg(overrides.noSave ? QStringLiteral("是")
                                                       : QStringLiteral("否"));
    }
}

} // namespace jie2::ui
