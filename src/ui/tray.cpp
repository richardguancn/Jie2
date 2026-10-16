#include "ui/tray.h"

#include <algorithm>
#include <cstdint>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QColorSpace>
#include <QCryptographicHash>
#include <QCursor>
#include <QDateTime>
#include <QDebug>
#include <QMimeData>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

#include "core/config.h"
#include "core/image_buffer.h"
#include "core/image_ops.h"
#include "core/ocr.h"
#include "ui/clothesline.h"
#include "ui/ocr_runner.h"
#include "ui/pin_window.h"
#include "ui/selection_overlay.h"

namespace jie2::ui {

namespace {

constexpr const char* kHotkeyId = "toggle-capture";
// 贴图热键（M4）。Snipaste 默认 F3，但 F3 在多数系统上被占用概率高
// （浏览器搜索、IDE 查找），改用 Ctrl+Alt+P：P = Pin。
constexpr const char* kPinHotkeyId = "pin-clipboard";

/// 代码绘制托盘图标。M0 不引入资源文件；正式图标与品牌资源在 M7 设计。
QIcon makeTrayIcon() {
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    // 取景框 + 对角线，示意"截图"
    painter.setPen(QPen(QColor(64, 158, 255), 6));
    painter.drawRect(QRectF(10, 10, 44, 44));
    painter.drawLine(QPointF(23, 41), QPointF(41, 23));
    painter.end();
    return QIcon(pixmap);
}

QString configFilePath() {
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/jie2.conf");
}

/// 截图历史文件（与配置同目录）。
QString historyFilePath() {
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/history.txt");
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
    if (platform_) {
        platform_->releaseAllHotkeys();
    }
    delete menu_;
}

bool TrayController::start() {
    // 配置与历史已在构造函数里加载（CLI 路径不经过这里）。
    capabilities_ = platform_->probe();

    // 衣架（M3）：只在托盘常驻模式下创建 —— CLI 一次性路径不需要它，
    // 也不该为了它去启动指针轮询。
    if (clotheslineEnabled_) {
        clothesline_ = new Clothesline();
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
                    note(QStringLiteral("已复制到剪贴板"),
                         QFileInfo(path).fileName());
                });
        connect(clothesline_, &Clothesline::entryDropped, this,
                [this](const QString& path) {
                    history_.remove(path.toStdString());
                    saveHistory();
                    refreshClothesline();
                });
        refreshClothesline();
        clothesline_->setWatchEnabled(true);
    }

    // 剪贴板图片监听（M3 增补）：复制图像自动挂上衣架。
    setupClipboardWatcher();

    tray_ = new QSystemTrayIcon(makeTrayIcon(), this);
    tray_->setToolTip(QStringLiteral("截2 — %1")
                          .arg(QString::fromStdString(platform_->name())));

    menu_ = new QMenu();
    rebuildMenu();
    tray_->setContextMenu(menu_);

    connect(tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger) {
                    showCapabilities();
                }
            });
    tray_->show();

    registerHotkey();

    // 平台能力受限时第一时间如实提示（评估报告 3.2 要求降级对用户可见）。
    if (capabilities_.hasLimitations() && tray_->isSystemTrayAvailable()) {
        tray_->showMessage(
            QStringLiteral("截2：当前会话能力受限"),
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

    auto* captureAction = menu_->addAction(QStringLiteral("截图（选择区域）"));
    connect(captureAction, &QAction::triggered, this,
            &TrayController::triggerCapture);

    // 贴图（M4）：把剪贴板内容贴成置顶窗口。
    auto* pinAction = menu_->addAction(QStringLiteral("贴图（从剪贴板）"));
    connect(pinAction, &QAction::triggered, this, &TrayController::triggerPin);

    // 文字识别（M5）：对剪贴板图像跑 OCR。
    auto* ocrAction = menu_->addAction(
        QStringLiteral("文字识别（OCR · %1）")
            .arg(QString::fromStdString(core::toString(ocrConfig_.backend))));
    connect(ocrAction, &QAction::triggered, this, &TrayController::triggerOcr);

    // 二维码/条形码识别（M6，专业版对齐项 barcode-scan）。
    auto* barcodeAction =
        menu_->addAction(QStringLiteral("识别二维码/条形码"));
    connect(barcodeAction, &QAction::triggered, this,
            &TrayController::triggerBarcodeScan);

    if (!pins_.empty()) {
        auto* hidePins = menu_->addAction(
            pinsHidden_ ? QStringLiteral("显示全部贴图（%1）").arg(pins_.size())
                        : QStringLiteral("隐藏全部贴图（%1）").arg(pins_.size()));
        connect(hidePins, &QAction::triggered, this,
                &TrayController::toggleHidePins);
        auto* savePins = menu_->addAction(
            QStringLiteral("保存全部贴图（%1）").arg(pins_.size()));
        connect(savePins, &QAction::triggered, this,
                &TrayController::saveAllPins);

        // 贴图分组（专业版对齐项）：只显示当前组的贴图。
        auto* groupMenu = menu_->addMenu(
            QStringLiteral("贴图分组（当前第 %1 组）").arg(currentPinGroup_ + 1));
        for (int g = 0; g < std::max(1, pinGroupCount_); ++g) {
            QAction* item = groupMenu->addAction(
                QStringLiteral("第 %1 组").arg(g + 1));
            item->setCheckable(true);
            item->setChecked(g == currentPinGroup_);
            connect(item, &QAction::triggered, this,
                    [this, g]() { switchPinGroup(g); });
        }
        QAction* nextGroup = groupMenu->addAction(QStringLiteral("下一组"));
        connect(nextGroup, &QAction::triggered, this,
                [this]() { switchPinGroup(-1); });

        auto* closePins = menu_->addAction(QStringLiteral("关闭全部贴图"));
        connect(closePins, &QAction::triggered, this,
                &TrayController::closeAllPins);
    }

    auto* hotkeyStatus = menu_->addAction(
        hotkeyRegistered_
            ? QStringLiteral("截图热键：Ctrl+Alt+T%1")
                  .arg(pinHotkeyRegistered_
                           ? QStringLiteral(" · 贴图热键：Ctrl+Alt+P")
                           : QStringLiteral(" · 贴图热键不可用"))
            : QStringLiteral("全局热键不可用：%1").arg(hotkeyError_));
    hotkeyStatus->setEnabled(false);

    if (captureCount_ > 0) {
        auto* lastCapture = menu_->addAction(
            QStringLiteral("最近截图：%1").arg(QFileInfo(lastCapturePath_).fileName()));
        lastCapture->setEnabled(false);
    }

    menu_->addSeparator();

    // 输出装饰（M1）：边框 / 阴影 / 圆角。参数走配置文件，菜单只做开关。
    auto* decorationMenu = menu_->addMenu(QStringLiteral("输出装饰"));
    auto makeToggle = [this, decorationMenu](const QString& text, bool checked,
                                             auto apply) {
        QAction* action = decorationMenu->addAction(text);
        action->setCheckable(true);
        action->setChecked(checked);
        connect(action, &QAction::triggered, this, [this, apply](bool on) {
            apply(on);
            saveConfig();
            rebuildMenu();
        });
    };
    makeToggle(QStringLiteral("圆角（半径 %1）").arg(roundRadius_), roundEnabled_,
               [this](bool on) { roundEnabled_ = on; });
    makeToggle(QStringLiteral("边框（%1px）").arg(borderWidth_), borderEnabled_,
               [this](bool on) { borderEnabled_ = on; });
    makeToggle(QStringLiteral("阴影"), shadowEnabled_,
               [this](bool on) { shadowEnabled_ = on; });

    // 触发角（M1）：屏幕角落停留触发截图。
    QAction* triggerAction =
        menu_->addAction(QStringLiteral("触发角（停留 %1ms 触发截图）")
                             .arg(triggerDwellMs_));
    triggerAction->setCheckable(true);
    triggerAction->setChecked(triggerEnabled_);
    connect(triggerAction, &QAction::triggered, this, [this](bool on) {
        triggerEnabled_ = on;
        saveConfig();
        setupTriggerCorner();
        rebuildMenu();
    });

    // 衣架（M3，tendedero 合并）：指针停在屏幕顶边展开。
    QAction* clotheslineAction =
        menu_->addAction(QStringLiteral("衣架（顶边停留展开）"));
    clotheslineAction->setCheckable(true);
    clotheslineAction->setChecked(clothesline_ != nullptr);
    clotheslineAction->setEnabled(clothesline_ != nullptr);
    connect(clotheslineAction, &QAction::triggered, this, [this](bool on) {
        clotheslineEnabled_ = on;
        saveConfig();
        if (clothesline_ != nullptr) {
            clothesline_->setWatchEnabled(on);
        }
    });

    // 剪贴板图像自动挂载（M3 增补，对齐 tendedero 的 "Hang copied images"）。
    QAction* clipWatch =
        menu_->addAction(QStringLiteral("复制图像时自动挂上衣架"));
    clipWatch->setCheckable(true);
    clipWatch->setChecked(clipboardWatchEnabled_);
    connect(clipWatch, &QAction::triggered, this, [this](bool on) {
        clipboardWatchEnabled_ = on;
        saveConfig();
        rebuildMenu();
    });

    menu_->addSeparator();

    auto* capabilityAction = menu_->addAction(QStringLiteral("平台能力…"));
    connect(capabilityAction, &QAction::triggered, this,
            &TrayController::showCapabilities);

    menu_->addSeparator();

    auto* quitAction = menu_->addAction(QStringLiteral("退出截2"));
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);
}

QString TrayController::saveAndCopy(const QImage& image, QString* error) {
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
    QImage output = decorateForOutput(image);
    // --size W H（M6）：强制输出尺寸（在装饰之后，得到的就是最终尺寸）。
    if (overrides_.hasSize && overrides_.sizeWidth > 0 &&
        overrides_.sizeHeight > 0 &&
        (output.width() != overrides_.sizeWidth ||
         output.height() != overrides_.sizeHeight)) {
        output = output.scaled(overrides_.sizeWidth, overrides_.sizeHeight,
                               Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    // 色彩管理（M1 收尾项）：像素语义本就是 sRGB（实测与系统工具差异≤1），
    // 这里补上颜色空间标记，让严格做色彩管理的程序正确解释本文件。
    output.setColorSpace(QColorSpace::SRgb);

    // 与 Snipaste 的默认行为一致：截图落点之一是剪贴板。
    // 剪贴板始终写入（--no-save 也只跳过落盘，不影响剪贴板）。
    // 走 writeClipboardImage：它会记录摘要，避免这次写入引发的
    // （异步）剪贴板事件被当成"用户复制"再挂一次。
    writeClipboardImage(output);

    // --no-save（M6）：只写剪贴板，不落盘、不进历史。
    if (overrides_.noSave) {
        qInfo().noquote() << QStringLiteral("[jie2] --no-save：仅写入剪贴板");
        ++captureCount_;
        saveConfig();
        return QStringLiteral("(clipboard)");
    }

    // 输出路径：命令行显式指定优先（M6），否则按时间自动命名。
    QString path;
    if (!overrides_.outputPath.isEmpty()) {
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
        path = directory + QLatin1Char('/') + captureFileName();
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

    return saveAndCopy(toQImage(cropped), error);
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

    // 窗口吸附候选（M1）：平台层的窗口矩形（虚拟桌面坐标）→ 本屏逻辑坐标。
    // 注意 macOS 的窗口 bounds 与显示器 bounds 同在"逻辑点"空间；
    // X11 两者同在像素空间 —— 换算只需减去屏幕原点，两种平台一致。
    const QRect screenGeometryForSnap = screen->geometry();
    std::vector<core::Rect> snapCandidates;
    for (const auto& windowRect : platform_->windowRects()) {
        const int x = windowRect.x - screenGeometryForSnap.x();
        const int y = windowRect.y - screenGeometryForSnap.y();
        const core::Rect local = core::Rect{x, y, windowRect.width, windowRect.height}
                                     .intersected(core::Rect{0, 0,
                                                             screenGeometryForSnap.width(),
                                                             screenGeometryForSnap.height()});
        if (!local.isEmpty()) {
            snapCandidates.push_back(local);
        }
    }
    if (!snapCandidates.empty()) {
        qInfo().noquote() << QStringLiteral("[jie2] 吸附候选窗口数：%1")
                                 .arg(snapCandidates.size());
    }

    const SelectionOverlay::Outcome outcome = SelectionOverlay::run(
        QPixmap::fromImage(image), scale, screen, historyPaths, recapture,
        snapCandidates, paletteSize_);
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
    return saveAndCopy(outcome.outputImage, error);
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
    const SelectionOverlay::Outcome outcome =
        SelectionOverlay::run(QPixmap::fromImage(board), scale, screen);
    if (!outcome.accepted) {
        return {};  // 用户取消：静默
    }
    if (outcome.outputImage.isNull()) {
        return setError(QStringLiteral("白板输出为空"));
    }
    qInfo().noquote() << QStringLiteral("[jie2] 白板模式：输出 %1x%2")
                             .arg(outcome.outputImage.width())
                             .arg(outcome.outputImage.height());
    return saveAndCopy(outcome.outputImage, error);
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
    qInfo().noquote() << QStringLiteral("[jie2] 截图已保存：%1").arg(path);
    note(QStringLiteral("截图已保存"),
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
    if (captureInProgress_ || QApplication::activeModalWidget() != nullptr) {
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
    if (clothesline_ == nullptr) {
        return;
    }
    clothesline_->setEntries(history_.entries());
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
        QByteArrayView(reinterpret_cast<const char*>(small.constBits()),
                       static_cast<qsizetype>(small.sizeInBytes())),
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

void TrayController::clipboardChanged() {
    if (!clipboardWatchEnabled_ || clothesline_ == nullptr) {
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
    note(QStringLiteral("已挂上衣架"), QFileInfo(path).fileName());
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

    // 新贴图归入当前分组（专业版对齐项）。
    pin->setGroup(currentPinGroup_);

    pins_.push_back(pin);
    pinsHidden_ = false;
    pin->show();
    pin->raise();
    note(QStringLiteral("已贴图"),
         QStringLiteral("滚轮缩放 · 1/2 旋转 · 3/4 翻转 · Ctrl+滚轮透明度 "
                        "· 6 反色 · 双击关闭"));
    rebuildMenu();
}

void TrayController::toggleHidePins() {
    if (pins_.empty()) {
        note(QStringLiteral("没有贴图"), QStringLiteral("先用 Ctrl+Alt+P 贴一张出来"));
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
    note(QStringLiteral("贴图已保存"),
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
             QStringLiteral("剪贴板里没有图像。先截一张带码的图（Ctrl+Alt+T）再试。"),
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
        note(QStringLiteral("未发现二维码/条形码"),
             QStringLiteral("图像里没有可识别的码（后端：%1）")
                 .arg(QString::fromStdString(result.backend)));
        return;
    }
    QApplication::clipboard()->setText(QString::fromStdString(result.text));
    qInfo().noquote() << QStringLiteral("[jie2] 扫码成功（%1 条）")
                             .arg(QString::fromStdString(result.text)
                                      .count(QLatin1Char('\n')) +
                                  1);
    note(QStringLiteral("扫码完成（已复制）"),
         QString::fromStdString(result.text).left(160));
}

void TrayController::triggerOcr() {
    // 来源：剪贴板图像（截图后内容即在此）。
    const QImage image = QApplication::clipboard()->image();
    if (image.isNull()) {
        note(QStringLiteral("无法识别"),
             QStringLiteral("剪贴板里没有图像。先截一张图（Ctrl+Alt+T）再试。"),
             /*warning=*/true);
        return;
    }

    // 云端后端必须显式确认（评估报告 4.4 第二条原则：联网需显式同意）。
    if (ocrConfig_.isCloud()) {
        const auto answer = QMessageBox::warning(
            nullptr, QStringLiteral("文字识别将上传图像"),
            QStringLiteral("当前 OCR 后端是云端接口：\n%1\n\n识别时会把这张图像"
                           "上传到该服务。是否继续？")
                .arg(QString::fromStdString(ocrConfig_.endpoint)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            qInfo().noquote() << QStringLiteral("[jie2] 云端 OCR 被用户取消");
            return;
        }
    }

    const core::OcrResult result = runOcrOnImage(image);
    if (!result.ok) {
        note(QStringLiteral("文字识别失败"),
             QString::fromStdString(result.error), /*warning=*/true);
        return;
    }
    if (result.text.empty()) {
        note(QStringLiteral("文字识别完成"),
             QStringLiteral("没有识别到文字（后端：%1）")
                 .arg(QString::fromStdString(result.backend)));
        return;
    }

    QApplication::clipboard()->setText(QString::fromStdString(result.text));
    const QString preview = QString::fromStdString(result.text).left(120);
    note(QStringLiteral("文字识别完成（已复制）"),
         QStringLiteral("%1\n\n后端：%2")
             .arg(preview, QString::fromStdString(result.backend)));
}

void TrayController::note(const QString& title, const QString& body, bool warning) {
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
    const QString text =
        QStringLiteral("实现：%1\n\n%2")
            .arg(QString::fromStdString(platform_->name()),
                 QString::fromStdString(capabilities_.describe()));
    QMessageBox box(QMessageBox::Information, QStringLiteral("截2 平台能力"), text);
    box.exec();
}

void TrayController::registerHotkey() {
    platform::HotkeySpec spec;
    spec.id = kHotkeyId;
    spec.modifiers = platform::kModCtrl | platform::kModAlt;
    spec.key = platform::Key::T;

    std::string error;
    hotkeyRegistered_ =
        platform_->grabHotkey(spec, &TrayController::hotkeyThunk, this, &error);
    hotkeyError_ = QString::fromStdString(error);

    if (hotkeyRegistered_) {
        qInfo().noquote() << QStringLiteral("[jie2] 全局热键已注册：%1")
                                 .arg(QString::fromStdString(
                                     platform::describeHotkey(spec)));
    } else {
        qWarning().noquote()
            << QStringLiteral("[jie2] 全局热键注册失败：%1").arg(hotkeyError_);
    }

    // 贴图热键（M4）：Ctrl+Alt+P。
    platform::HotkeySpec pinSpec;
    pinSpec.id = kPinHotkeyId;
    pinSpec.modifiers = platform::kModCtrl | platform::kModAlt;
    pinSpec.key = platform::Key::P;
    std::string pinError;
    pinHotkeyRegistered_ =
        platform_->grabHotkey(pinSpec, &TrayController::hotkeyThunk, this, &pinError);
    pinHotkeyError_ = QString::fromStdString(pinError);
    if (pinHotkeyRegistered_) {
        qInfo().noquote() << QStringLiteral("[jie2] 贴图热键已注册：%1")
                                 .arg(QString::fromStdString(
                                     platform::describeHotkey(pinSpec)));
    } else {
        qWarning().noquote()
            << QStringLiteral("[jie2] 贴图热键注册失败：%1").arg(pinHotkeyError_);
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
    if (id != kHotkeyId) {
        return;
    }
    ++hotkeyCount_;
    saveConfig();
    qInfo().noquote()
        << QStringLiteral("[jie2] 热键触发截图（第 %1 次）").arg(hotkeyCount_);
    triggerCapture();
}

void TrayController::loadConfig() {
    QFile file(configPath_);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }
    const QByteArray raw = file.readAll();
    const core::Config config =
        core::Config::parse(std::string(raw.constData(), raw.size()));
    hotkeyCount_ = config.getInt("hotkey.count", 0);
    captureCount_ = config.getInt("capture.count", 0);

    // 输出装饰（M1）
    roundEnabled_ = config.getBool("output.round.enabled", false);
    roundRadius_ = config.getInt("output.round.radius", 8);
    borderEnabled_ = config.getBool("output.border.enabled", false);
    borderWidth_ = config.getInt("output.border.width", 1);
    const QString borderColor =
        QString::fromStdString(config.get("output.border.color", "#d2d2d2"));
    if (QColor::isValidColorName(borderColor)) {
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

    // 标注调色板（M2 增补，专业版对齐项）
    paletteSize_ = config.getInt("annotation.palette_size", 6);

    // OCR（M5）
    std::string ocrWarning;
    ocrConfig_ = core::parseOcrConfig(config, &ocrWarning);
    if (!ocrWarning.empty()) {
        qWarning().noquote() << QStringLiteral("[jie2] %1")
                                    .arg(QString::fromStdString(ocrWarning));
    }
}

void TrayController::saveConfig() {
    core::Config config;
    config.set("hotkey.count", std::to_string(hotkeyCount_));
    config.set("capture.count", std::to_string(captureCount_));
    config.set("hotkey.id", kHotkeyId);
    config.set("platform", platform_->name());

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

    // 标注调色板（M2 增补）
    config.set("annotation.palette_size", std::to_string(paletteSize_));

    // OCR（M5）：只回写非机密项；api_key 由用户在配置里自填，
    // 程序不主动覆写（避免把已配置的密钥清空）。
    config.set("ocr.backend", core::toString(ocrConfig_.backend));
    config.set("ocr.language", ocrConfig_.language);
    config.set("ocr.timeout_ms", std::to_string(ocrConfig_.timeoutMs));
    if (!ocrConfig_.endpoint.empty()) {
        config.set("ocr.endpoint", ocrConfig_.endpoint);
    }
    if (!ocrConfig_.model.empty()) {
        config.set("ocr.model", ocrConfig_.model);
    }

    QFile file(configPath_);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        qWarning().noquote()
            << QStringLiteral("[jie2] 配置写入失败：%1").arg(configPath_);
        return;
    }
    const std::string text = config.serialize();
    file.write(text.data(), static_cast<qint64>(text.size()));
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
