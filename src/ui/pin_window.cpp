#include "ui/pin_window.h"

#include <algorithm>
#include <cmath>

#include <QApplication>
#include <QClipboard>
#include <QDebug>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QScreen>
#include <QTextDocument>
#include <QWheelEvent>

namespace jie2::ui {

namespace {

constexpr double kMinScale = 0.1;
constexpr double kMaxScale = 8.0;
constexpr double kOpacityStep = 10.0;
constexpr int kThumbMax = 320;  // 缩略图模式的最大边长（逻辑点）

/// 尝试把文本解析为颜色值（Snipaste 支持 RGB 三个数或 #HEX）。
bool parseColorText(const QString& text, QColor* out) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }
    // #RGB / #RRGGBB / #RRGGBBAA
    if (trimmed.startsWith(QLatin1Char('#'))) {
        const QColor color(trimmed);
        if (color.isValid()) {
            *out = color;
            return true;
        }
        return false;
    }
    // "255, 0, 0" / "255 0 0" / "1.0, 0.5, 0"
    static const QRegularExpression separator(QStringLiteral("[,\\s]+"));
    const QStringList parts =
        trimmed.split(separator, Qt::SkipEmptyParts);
    if (parts.size() != 3) {
        return false;
    }
    int channels[3] = {0, 0, 0};
    bool fractional = false;
    for (int i = 0; i < 3; ++i) {
        bool ok = false;
        const double value = parts[i].toDouble(&ok);
        if (!ok) {
            return false;
        }
        if (value >= 0.0 && value <= 1.0 && parts[i].contains(QLatin1Char('.'))) {
            fractional = true;
            channels[i] = static_cast<int>(std::lround(value * 255.0));
        } else {
            channels[i] = static_cast<int>(std::lround(value));
        }
    }
    if (channels[0] < 0 || channels[0] > 255 || channels[1] < 0 ||
        channels[1] > 255 || channels[2] < 0 || channels[2] > 255) {
        return false;
    }
    Q_UNUSED(fractional);
    *out = QColor(channels[0], channels[1], channels[2]);
    return true;
}

/// 颜色 → 色块图片（带色值与 HEX 标注，对齐 Snipaste 的"颜色信息贴图"）。
QImage imageFromColor(const QColor& color) {
    const int width = 220;
    const int height = 150;
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&image);
    painter.fillRect(0, 0, width, height, color);
    // 自动对比色：浅色底用深色字。
    const bool dark = color.lightness() > 128;
    painter.setPen(dark ? QColor(20, 20, 20) : QColor(240, 240, 240));
    QFont font;
    font.setPointSize(13);
    font.setBold(true);
    painter.setFont(font);
    painter.drawText(QRect(0, height / 2 - 26, width, 24), Qt::AlignCenter,
                     color.name(QColor::HexRgb).toUpper());
    const QString rgb = QStringLiteral("RGB %1, %2, %3")
                            .arg(color.red())
                            .arg(color.green())
                            .arg(color.blue());
    font.setBold(false);
    font.setPointSize(10);
    painter.setFont(font);
    painter.drawText(QRect(0, height / 2 + 2, width, 24), Qt::AlignCenter, rgb);
    painter.end();
    return image;
}

/// 纯文本 → 图片（自动换行、浅色底、限宽）。
QImage imageFromText(const QString& text) {
    const int maxWidth = 520;
    QTextDocument document;
    document.setDefaultFont(QFont(QString(), 12));
    document.setPlainText(text);

    // 先按目标宽度排一次，拿到高度。
    document.setTextWidth(maxWidth);
    const double height = document.size().height();
    const int width = static_cast<int>(std::ceil(document.idealWidth()));
    Q_UNUSED(height);

    QImage image(std::max(80, std::min(maxWidth, width + 24)),
                 static_cast<int>(std::ceil(document.size().height())) + 24,
                 QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(250, 250, 250));
    QPainter painter(&image);
    painter.translate(12, 12);
    document.drawContents(&painter);
    painter.end();
    return image;
}

} // namespace

PinWindow::PinWindow(QImage image, QWidget* parent) : QWidget(parent) {
    source_ = std::move(image);

    setWindowTitle(QStringLiteral("截2 贴图"));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint |
                   Qt::WindowStaysOnTopHint | Qt::Tool |
                   Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setMouseTracking(false);
    setFocusPolicy(Qt::StrongFocus);

    // 初始位置：屏幕中心偏左上（不遮挡视线中心），逐张偏移避免完全重叠。
    QScreen* screen = QGuiApplication::primaryScreen();
    if (screen != nullptr && !source_.isNull()) {
        const QRect area = screen->availableGeometry();
        static int cascade = 0;
        const int offset = (cascade++ % 8) * 24;
        const QSize size = source_.size() / (source_.devicePixelRatio() > 0
                                                 ? source_.devicePixelRatio()
                                                 : 1);
        move(area.center().x() - size.width() / 2 + offset,
             area.center().y() - size.height() / 2 + offset);
    }

    rebuild();
    qInfo().noquote() << QStringLiteral("[jie2] 贴图创建 %1x%2")
                             .arg(source_.width())
                             .arg(source_.height());
}

PinWindow* PinWindow::fromClipboard(QString* error) {
    const auto setError = [error](const QString& text) {
        if (error != nullptr) {
            *error = text;
        }
        return nullptr;
    };

    const QClipboard* clipboard = QApplication::clipboard();
    if (clipboard == nullptr) {
        return setError(QStringLiteral("剪贴板不可用"));
    }
    const QMimeData* mime = clipboard->mimeData();
    if (mime == nullptr) {
        return setError(QStringLiteral("剪贴板为空"));
    }

    // 1) 图像优先（最常见来源）。
    if (mime->hasImage()) {
        const QImage image = clipboard->image();
        if (!image.isNull()) {
            return new PinWindow(image);
        }
    }

    // 2) 文件路径：若是图像文件，加载（对齐 Snipaste 的"文件是图片会把图像贴出"）。
    if (mime->hasUrls()) {
        const auto urls = mime->urls();
        if (!urls.isEmpty() && urls.first().isLocalFile()) {
            const QString path = urls.first().toLocalFile();
            const QImage image(path);
            if (!image.isNull()) {
                return new PinWindow(image);
            }
            // 非图像文件 → 当作文本贴出（Snipaste 语义）。
            return new PinWindow(imageFromText(path));
        }
    }

    // 3) 文本：颜色值 → 色块；否则渲染为文本图。
    if (mime->hasText()) {
        const QString text = clipboard->text();
        if (text.trimmed().isEmpty()) {
            return setError(QStringLiteral("剪贴板内容为空"));
        }
        QColor color;
        if (parseColorText(text, &color)) {
            return new PinWindow(imageFromColor(color));
        }
        return new PinWindow(imageFromText(text));
    }

    return setError(QStringLiteral("剪贴板中没有可贴出的内容（需要图像、文本或颜色值）"));
}

QImage PinWindow::transformed() const {
    if (source_.isNull()) {
        return source_;
    }
    QImage image = source_;
    // 反色（6 键）：只反转 RGB，保持 alpha。
    if (inverted_) {
        image = image.convertToFormat(QImage::Format_ARGB32);
        image.invertPixels(QImage::InvertRgb);
    }
    // 先翻转后旋转，保证 1/2 与 3/4 的组合符合直觉。
    if (flipH_ || flipV_) {
        image = image.mirrored(flipH_, flipV_);
    }
    if (rotation_ != 0) {
        QTransform transform;
        transform.rotate(rotation_);
        image = image.transformed(transform, Qt::SmoothTransformation);
    }
    return image;
}

void PinWindow::rebuild() {
    const QImage base = transformed();
    if (base.isNull()) {
        return;
    }

    // 缩略图模式：把基准尺寸限制到固定最大边长。
    double effectiveScale = scale_;
    if (thumbnail_) {
        const int longest = std::max(base.width(), base.height());
        if (longest > 0) {
            effectiveScale = std::min(scale_,
                                      static_cast<double>(kThumbMax) / longest);
        }
    }

    // 显示尺寸（保持设备的 DPR 语义：按逻辑点呈现）。
    const double dpr = base.devicePixelRatio() > 0 ? base.devicePixelRatio() : 1.0;
    const int width = std::max(1, static_cast<int>(std::lround(
                                      base.width() / dpr * effectiveScale)));
    const int height = std::max(1, static_cast<int>(std::lround(
                                       base.height() / dpr * effectiveScale)));

    resize(width, height);
    setWindowOpacity(std::clamp(opacityPercent_ / 100.0, 0.05, 1.0));
    update();
}

void PinWindow::applyScaleFactor(double factor) {
    const double next = std::clamp(scale_ * factor, kMinScale, kMaxScale);
    if (std::abs(next - scale_) < 1e-6) {
        return;
    }
    scale_ = next;
    rebuild();
    qInfo().noquote() << QStringLiteral("[jie2] 贴图缩放 %1%")
                             .arg(static_cast<int>(std::lround(scale_ * 100)));
}

void PinWindow::applyOpacityDelta(double delta) {
    const double next = std::clamp(opacityPercent_ + delta, 10.0, 100.0);
    if (std::abs(next - opacityPercent_) < 1e-6) {
        return;
    }
    opacityPercent_ = next;
    setWindowOpacity(std::clamp(opacityPercent_ / 100.0, 0.05, 1.0));
    qInfo().noquote() << QStringLiteral("[jie2] 贴图透明度 %1%")
                             .arg(static_cast<int>(std::lround(opacityPercent_)));
}

void PinWindow::resetTo100() {
    scale_ = 1.0;
    opacityPercent_ = 100.0;
    thumbnail_ = false;
    rebuild();
    qInfo().noquote() << QStringLiteral("[jie2] 贴图重置为 100%");
}

void PinWindow::toggleThumbnail() {
    thumbnail_ = !thumbnail_;
    rebuild();
    qInfo().noquote() << QStringLiteral("[jie2] 贴图%1")
                             .arg(thumbnail_ ? QStringLiteral("缩略图模式")
                                             : QStringLiteral("恢复原尺寸"));
}

void PinWindow::setInverted(bool on) {
    inverted_ = on;
    rebuild();
}

void PinWindow::rotateClockwise() {
    rotation_ = (rotation_ + 90) % 360;
    rebuild();
}

void PinWindow::rotateCounterClockwise() {
    rotation_ = (rotation_ + 270) % 360;
    rebuild();
}

void PinWindow::flipHorizontal() {
    flipH_ = !flipH_;
    rebuild();
}

void PinWindow::flipVertical() {
    flipV_ = !flipV_;
    rebuild();
}

void PinWindow::toggleInvert() {
    inverted_ = !inverted_;
    rebuild();
    qInfo().noquote() << QStringLiteral("[jie2] 贴图反色：%1")
                             .arg(inverted_ ? QStringLiteral("开")
                                            : QStringLiteral("关"));
}

void PinWindow::toggleMouseThrough() {
    mouseThrough_ = !mouseThrough_;
    // Qt::WindowTransparentForInput 让窗口对输入完全透明（点击穿透到下层）。
    // 需要 setWindowFlag + show 才能在某些平台上生效。
    const bool visible = isVisible();
    setWindowFlag(Qt::WindowTransparentForInput, mouseThrough_);
    if (visible) {
        show();
    }
    qInfo().noquote()
        << QStringLiteral("[jie2] 贴图鼠标穿透：%1")
               .arg(mouseThrough_ ? QStringLiteral("开") : QStringLiteral("关"));
}

void PinWindow::copyToClipboard() {
    const QImage image = transformed();
    if (!image.isNull()) {
        QApplication::clipboard()->setImage(image);
        qInfo().noquote() << QStringLiteral("[jie2] 贴图已复制到剪贴板");
    }
}

bool PinWindow::saveTo(const QString& path) const {
    const QImage image = transformed();
    if (image.isNull() || path.isEmpty()) {
        return false;
    }
    // 按扩展名选格式（与截图落点同一约定）。
    const bool jpeg = path.endsWith(QStringLiteral(".jpg"), Qt::CaseInsensitive) ||
                      path.endsWith(QStringLiteral(".jpeg"), Qt::CaseInsensitive);
    const bool ok = jpeg ? image.save(path, "JPEG", 95) : image.save(path, "PNG");
    if (ok) {
        qInfo().noquote() << QStringLiteral("[jie2] 贴图已保存：%1").arg(path);
    } else {
        qWarning().noquote() << QStringLiteral("[jie2] 贴图保存失败：%1").arg(path);
    }
    return ok;
}

void PinWindow::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const QImage base = transformed();
    if (base.isNull()) {
        return;
    }
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(rect(), base);
    // 细边框：让贴图在浅色背景上也看得出边界（Snipaste 的贴图有阴影，
    // 我们 M4 先用 1px 边框，阴影留待后续迭代）。
    painter.setPen(QPen(QColor(0, 0, 0, 60), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(rect().adjusted(0, 0, -1, -1));

    // 选中态：高亮边框（Ctrl+A 全选后可见，拖动联动时提示哪些在动）。
    if (selected_) {
        painter.setPen(QPen(QColor(64, 158, 255), 2));
        painter.drawRect(rect().adjusted(1, 1, -2, -2));
    }
}

void PinWindow::closeEvent(QCloseEvent* event) {
    // 所有关闭路径的公共出口（Esc / 双击 / 程序化 close）。
    emit closed();
    QWidget::closeEvent(event);
}

void PinWindow::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        dragging_ = true;
        dragOffset_ = event->pos();
        raise();
        return;
    }
    if (event->button() == Qt::MiddleButton) {
        resetTo100();
        return;
    }
    if (event->button() == Qt::RightButton) {
        // 右键菜单：关闭 / 复制 / 穿透 / 反色（简化版，完整分组管理在 M4 增补）。
        close();
        return;
    }
    QWidget::mousePressEvent(event);
}

void PinWindow::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_) {
        const QPoint newPos = event->globalPosition().toPoint() - dragOffset_;
        const QPoint delta = newPos - pos();
        move(newPos);
        // 报告位移：托盘控制器让其余**选中**贴图一起动
        // （专业版 v2.4：全选后整体拖动）。
        if (!delta.isNull()) {
            emit movedBy(delta);
        }
    }
}

void PinWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        dragging_ = false;
    }
}

void PinWindow::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    // 修饰键+双击 = 缩略图模式（修饰键可配置，专业版 v2.11）；
    // 裸双击 = 关闭（对齐 Snipaste）。
    if (thumbnailModifier_ != Qt::NoModifier &&
        event->modifiers().testFlag(thumbnailModifier_)) {
        toggleThumbnail();
        return;
    }
    qInfo().noquote() << QStringLiteral("[jie2] 贴图关闭（双击）");
    close();
}

void PinWindow::wheelEvent(QWheelEvent* event) {
    const int delta = event->angleDelta().y();
    if (delta == 0) {
        return;
    }
    const double steps = delta / 120.0;
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        applyOpacityDelta(steps * kOpacityStep);
    } else {
        applyScaleFactor(std::pow(1.1, steps));
    }
    event->accept();
}

void PinWindow::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
        case Qt::Key_Escape: {
            // 专业版对齐项 v2.8：Esc / Shift+Esc 的动作可自定义
            // （关闭 = 从列表移除；隐藏 = 保留，可由托盘"显示全部贴图"恢复）。
            const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
            const DismissAction action = shift ? shiftEscAction_ : escAction_;
            if (action == DismissAction::Hide) {
                qInfo().noquote()
                    << QStringLiteral("[jie2] 贴图隐藏（%1，可由托盘「显示全部贴图」恢复）")
                           .arg(shift ? QStringLiteral("Shift+Esc")
                                      : QStringLiteral("Esc"));
                hide();
            } else {
                qInfo().noquote() << QStringLiteral("[jie2] 贴图关闭（%1）")
                                         .arg(shift ? QStringLiteral("Shift+Esc")
                                                    : QStringLiteral("Esc"));
                close();
            }
            return;
        }
        case Qt::Key_1:
            rotateCounterClockwise();
            return;
        case Qt::Key_2:
            rotateClockwise();
            return;
        case Qt::Key_3:
            flipHorizontal();
            return;
        case Qt::Key_4:
            flipVertical();
            return;
        case Qt::Key_6:
            toggleInvert();
            return;
        case Qt::Key_Plus:
        case Qt::Key_Equal:
            applyScaleFactor(1.1);
            return;
        case Qt::Key_Minus:
            applyScaleFactor(1.0 / 1.1);
            return;
        case Qt::Key_0:
            resetTo100();
            return;
        case Qt::Key_A:
            // Ctrl+A：全选/取消全选所有贴图（专业版对齐项 v2.4）。
            // 具体动作在托盘控制器（它掌握全部贴图），这里只请求。
            if (event->modifiers().testFlag(Qt::ControlModifier)) {
                emit selectAllRequested();
                return;
            }
            break;
        case Qt::Key_C:
            if (event->modifiers().testFlag(Qt::ControlModifier)) {
                copyToClipboard();
                return;
            }
            break;
        default:
            break;
    }
    QWidget::keyPressEvent(event);
}

} // namespace jie2::ui
