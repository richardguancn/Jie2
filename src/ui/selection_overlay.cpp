#include "ui/selection_overlay.h"

#include <algorithm>
#include <cmath>
#include <string>

#include <QApplication>
#include <QClipboard>
#include <QDebug>
#include <QEventLoop>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QMouseEvent>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QTimer>
#include <QToolTip>
#include <QWindow>
#include <QWheelEvent>

#include "ui/annotation_render.h"

namespace jie2::ui {

namespace {

constexpr int kMinDragLogical = 3;   // 小于此距离不算一次有效选择
constexpr int kSnapThresholdLogical = 8;  // 窗口吸附触发距离（逻辑点）
constexpr int kMagnifierSize = 130;  // 放大镜边长（逻辑点）
constexpr int kMagnifierZoom = 4;    // 放大倍数（物理像素）
constexpr int kMagnifierGap = 18;    // 放大镜与光标间距（逻辑点）
constexpr int kChipHeight = 22;      // 色值条高度（逻辑点）

// 工具条几何（逻辑点）。三个按钮等宽。
constexpr int kToolbarHeight = 24;
constexpr int kToolbarGap = 0;

const QColor kAccent(40, 123, 255);
const QColor kSnapColor(120, 230, 150);  // 吸附指示色（区别于普通选区边框）
const QColor kDim(0, 0, 0, 110);
// 提示条/工具条底色已改为按主题解析（SelectionOverlay::ChromeColors），
// 不再用文件级常量 —— 这里保留注释说明迁移原因，避免误加回常量。

QFont labelFont() {
    QFont font;
    font.setPointSize(11);
    font.setBold(true);
    return font;
}

QFont smallFont() {
    QFont font;
    font.setPointSize(9);
    return font;
}

/// Qt 修饰键 → core::ShortcutMap 的位组合。
#ifdef Q_OS_MACOS
unsigned shortcutModifiers(Qt::KeyboardModifiers mods) {
    unsigned mask = 0;
    if (mods.testFlag(Qt::MetaModifier)) {
        mask |= core::kScCtrl;
    }
    if (mods.testFlag(Qt::AltModifier)) {
        mask |= core::kScAlt;
    }
    if (mods.testFlag(Qt::ShiftModifier)) {
        mask |= core::kScShift;
    }
    if (mods.testFlag(Qt::ControlModifier)) {
        mask |= core::kScMeta;
    }
    return mask;
}

#else
unsigned shortcutModifiers(Qt::KeyboardModifiers mods) {
    unsigned mask = 0;
    if (mods.testFlag(Qt::ControlModifier)) {
        mask |= core::kScCtrl;
    }
    if (mods.testFlag(Qt::AltModifier)) {
        mask |= core::kScAlt;
    }
    if (mods.testFlag(Qt::ShiftModifier)) {
        mask |= core::kScShift;
    }
    if (mods.testFlag(Qt::MetaModifier)) {
        mask |= core::kScMeta;
    }
    return mask;
}

#endif

/// 由拖拽两点得到选区矩形。
///
/// 语义是**半开区间**：[left, right) × [top, bottom)。
/// 即"拖了 400 像素宽就是 400 像素"，而不是 QRect(topLeft, bottomRight)
/// 的闭区间语义（那会得到 401）。截图工具里 1:1 的直觉最重要 ——
/// 用户拖多远，就该裁多大。
QRect rectBetween(const QPoint& a, const QPoint& b) {
    const int left = std::min(a.x(), b.x());
    const int top = std::min(a.y(), b.y());
    return QRect(left, top, std::abs(a.x() - b.x()), std::abs(a.y() - b.y()));
}

/// 画一个圆角底 + 文本的小标签，返回其矩形。

} // namespace

SelectionOverlay::SelectionOverlay(QPixmap frozen, double scaleFactor,
                                   QScreen* screen, Options options,
                                   QWidget* parent)
    : QDialog(parent),
      frozen_(std::move(frozen)),
      scaleFactor_(scaleFactor > 0.0 ? scaleFactor : 1.0),
      screen_(screen),
      historyPaths_(std::move(options.historyPaths)),
      recapture_(std::move(options.recapture)),
      snapCandidates_(std::move(options.snapCandidates)),
      paletteSize_(options.paletteSize),
      shortcuts_(options.shortcuts),
      promoteWindow_(std::move(options.promoteWindow)) {
    // 主题：深色 = 现有观感（默认）；浅色给自绘面板整套浅色板。
    if (options.darkTheme) {
        chrome_ = ChromeColors{QColor(28, 28, 32, 235), QColor(70, 70, 78),
                               QColor(232, 232, 232), QColor(52, 52, 58),
                               QColor(130, 130, 138), QColor(235, 235, 235)};
    } else {
        chrome_ = ChromeColors{QColor(244, 245, 248, 238), QColor(224, 227, 234),
                               QColor(32, 35, 42), QColor(235, 236, 240),
                               QColor(150, 152, 158), QColor(28, 30, 36)};
    }

    // 橡皮擦删除是否可撤销（v2.6："对橡皮擦禁用二次编辑"配置）。
    annotations_.setEraserUndoable(options.eraserUndoable);

    frozenImage_ = frozen_.toImage();
    screenImage_ = frozenImage_;

    // 具名窗口标题：空标题在 macOS 的辅助功能树里显示为占位符，
    // 会让自动化工具（含测试）无法稳定引用这个窗口。
    setWindowTitle(QStringLiteral("J2 选择截图区域"));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint |
                   Qt::WindowStaysOnTopHint | Qt::NoDropShadowWindowHint);
    setWindowModality(Qt::ApplicationModal);
    setCursor(Qt::CrossCursor);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);

    if (screen_ != nullptr) {
        screenGeometry_ = screen_->geometry();
        if (windowHandle() != nullptr) {
            windowHandle()->setScreen(screen_);
        }
    } else {
        screenGeometry_ = QRect(0, 0, 640, 480);
    }
    setGeometry(screenGeometry_);

    // 当前抓屏画面铺满屏幕：逻辑点 → 像素的比例即 scaleFactor。
    frozenLogicalRect_ = QRect(0, 0, screenGeometry_.width(), screenGeometry_.height());
    frozenPixelScale_ = scaleFactor_;
    if (options.selectImage) {
        toolChosen_ = true;
        setFrozenImage(frozenImage_, false);
        selectionRect_ = frozenLogicalRect_;
        hasSelection_ = true;
    }
}

SelectionOverlay::Outcome SelectionOverlay::run(const QPixmap& frozen,
                                                double scaleFactor,
                                                QScreen* screen,
                                                Options options) {
    SelectionOverlay overlay(frozen, scaleFactor, screen, std::move(options));
    // 不用 QDialog::exec()：**exec() 期间对话框被 hide() 会让它立刻返回
    // Rejected**（实测 Qt 6.4/X11，已用最小 Qt 程序单独复现）。而 F5 刷新
    // 必须临时隐藏遮罩才能拍到干净屏幕 —— 旧实现里按 F5 等于"取消截图"
    // （该缺陷因旧版没有 F5 自动化测试而长期潜伏，本轮补测试时暴露）。
    // 自管事件循环下 hide()/show() 只是普通事件；accept/reject 仍经
    // finished 信号正常结束循环，返回值语义与 exec() 一致。
    QEventLoop loop;
    QObject::connect(&overlay, &QDialog::finished, &loop, &QEventLoop::quit);
    overlay.show();
    loop.exec();
    const int code = overlay.result();
    Outcome result = overlay.outcome();
    result.accepted = (code == QDialog::Accepted) && !result.selection.isEmpty();
    return result;
}

void SelectionOverlay::setFrozenImage(const QImage& image, bool isCurrentScreen) {
    frozenImage_ = image;
    frozen_ = QPixmap::fromImage(image);
    if (isCurrentScreen) {
        frozenLogicalRect_ =
            QRect(0, 0, screenGeometry_.width(), screenGeometry_.height());
        frozenPixelScale_ = scaleFactor_;
    } else {
        // 历史图：等比缩放到适应屏幕、居中；只缩不放（小图按原尺寸显示）。
        if (image.width() > 0 && image.height() > 0 &&
            screenGeometry_.width() > 0 && screenGeometry_.height() > 0) {
            const double fit = std::min(
                {1.0, double(screenGeometry_.width()) / double(image.width()),
                 double(screenGeometry_.height()) / double(image.height())});
            const int w = qMax(1, qRound(image.width() * fit));
            const int h = qMax(1, qRound(image.height() * fit));
            frozenLogicalRect_ =
                QRect((screenGeometry_.width() - w) / 2,
                      (screenGeometry_.height() - h) / 2, w, h);
            frozenPixelScale_ = 1.0 / fit;
        }
    }
}

void SelectionOverlay::switchHistory(int index) {
    if (index < 0) {
        // 回到当前抓屏画面
        historyIndex_ = -1;
        setFrozenImage(screenImage_, true);
        annotations_.clear();
        composedDirty_ = true;
        hasSelection_ = false;
        selectionRect_ = QRect();
        snapActive_ = false;
        dragging_ = false;
        showToast(QStringLiteral("当前截图（按 , 查看历史）"));
        qInfo().noquote() << QStringLiteral("[jie2] 回放切回当前画面");
        update();
        return;
    }
    if (index >= static_cast<int>(historyPaths_.size())) {
        return;
    }
    QImage image(historyPaths_[index]);
    if (image.isNull()) {
        showToast(QStringLiteral("历史截图无法读取"));
        return;
    }
    historyIndex_ = index;
    setFrozenImage(image, false);
    // 画面换了，标注坐标基于旧图 → 清空（避免错位）。
    annotations_.clear();
    composedDirty_ = true;
    hasSelection_ = false;
    selectionRect_ = QRect();
    snapActive_ = false;
    dragging_ = false;
    showToast(QStringLiteral("历史 %1/%2（, 更旧 / . 更新）")
                  .arg(index + 1)
                  .arg(historyPaths_.size()));
    qInfo().noquote() << QStringLiteral("[jie2] 回放历史 %1/%2：%3")
                             .arg(index + 1)
                             .arg(historyPaths_.size())
                             .arg(historyPaths_[index]);
    update();
}

void SelectionOverlay::recapture() {
    if (!recapture_) {
        return;
    }
    // 关键：遮罩此刻正盖在屏幕上，直接抓屏会把遮罩自己拍进去。
    // 必须先隐藏，等合成器真正移除窗口后再抓屏，然后恢复显示。
    hide();
    {
        // 嵌套事件循环等待：让隐藏事件被处理、合成器完成一次合成。
        // 150ms 是实测经验值（macOS 上小于 ~80ms 时偶发抓到旧帧）。
        QEventLoop loop;
        QTimer::singleShot(150, &loop, &QEventLoop::quit);
        loop.exec();
    }
    const QPixmap fresh = recapture_();
    show();
    raise();
    activateWindow();
    setFocus();

    if (fresh.isNull()) {
        showToast(QStringLiteral("刷新失败：抓屏返回空图像"));
        update();
        return;
    }
    // 记录刷新前的几何：同一屏的实时刷新几何不变 → 按 v2.7.2
    // （"刷新截图，保持已有标注不变"）**保留标注与选区**；
    // 历史图 → 实时屏的切换可能改变显示几何（图像尺寸/缩放不同）→
    // 坐标语义已变，此时才清空。
    const QRect oldRect = frozenLogicalRect_;
    const double oldScale = frozenPixelScale_;
    const bool wasHistory = historyIndex_ >= 0;

    screenImage_ = fresh.toImage();
    historyIndex_ = -1;
    setFrozenImage(screenImage_, true);

    const bool geometryPreserved =
        !wasHistory && frozenLogicalRect_ == oldRect &&
        frozenPixelScale_ == oldScale;
    if (geometryPreserved) {
        rebuildComposed();
        showToast(QStringLiteral("已刷新截图（标注保留 %1 条）")
                      .arg(annotations_.size()));
        qInfo().noquote()
            << QStringLiteral("[jie2] F5 刷新截图完成（几何不变，保留标注 %1 条）")
                   .arg(annotations_.size());
    } else {
        annotations_.clear();
        selectedIndex_ = -1;
        composedDirty_ = true;
        hasSelection_ = false;
        selectionRect_ = QRect();
        snapActive_ = false;
        dragging_ = false;
        showToast(QStringLiteral("已刷新截图（画面几何已变，标注已清空）"));
        qInfo().noquote()
            << QStringLiteral("[jie2] F5 刷新截图完成（几何变化，清空标注）");
    }
    update();
}

void SelectionOverlay::showToast(const QString& text) {
    // toast 是瞬态绘制，测试与排障读不到 —— 同步写一行日志作为锚点。
    qInfo().noquote() << QStringLiteral("[jie2] %1").arg(text);
    toast_ = text;
    QTimer::singleShot(1800, this, [this]() {
        toast_.clear();
        update();
    });
}

QRect SelectionOverlay::selectionToPixelRegion(const QRect& logical) const {
    if (frozenImage_.isNull() || frozenLogicalRect_.isEmpty() ||
        frozenPixelScale_ <= 0.0 || logical.isEmpty()) {
        return {};
    }
    // 逻辑点 → 相对冻结图左上角 → 乘像素比例
    const QRectF inImage(
        (logical.x() - frozenLogicalRect_.x()) * frozenPixelScale_,
        (logical.y() - frozenLogicalRect_.y()) * frozenPixelScale_,
        logical.width() * frozenPixelScale_,
        logical.height() * frozenPixelScale_);
    const QRect pixel = inImage.toAlignedRect().intersected(
        QRect(0, 0, frozenImage_.width(), frozenImage_.height()));
    return pixel;
}

QRect SelectionOverlay::currentSelection() const {
    if (dragging_) {
        // 拖拽中：以吸附结果为准（若有命中），否则用原始拖拽矩形。
        if (snapActive_) {
            const core::Rect& snapped = lastSnap_.rect;
            return QRect(snapped.x, snapped.y, snapped.width, snapped.height)
                .intersected(rect());
        }
        return rectBetween(dragOrigin_, dragCurrent_).intersected(rect());
    }
    if (hasSelection_) {
        // 结算后的选区：必须用存储值而不是重算拖拽点 ——
        // 吸附过的选区一旦重算就会丢掉吸附结果。
        return selectionRect_;
    }
    return {};
}

void SelectionOverlay::commit(bool accepted) {
    QToolTip::hideText();
    outcome_.accepted = accepted;
    outcome_.selection = accepted ? currentSelection() : QRect();
    if (accepted) {
        // 裁剪信息在这里一次算好：调用方不需要再做缩放换算。
        const bool isHistory =
            historyIndex_ >= 0 &&
            historyIndex_ < static_cast<int>(historyPaths_.size());
        outcome_.sourcePath = isHistory ? historyPaths_[historyIndex_] : QString();
        outcome_.currentImage = isHistory ? QImage{} : frozenImage_;
        outcome_.pixelRegion = selectionToPixelRegion(outcome_.selection);
        // 最终输出在这里合成：源图 → 标注（矢量 + 像素效果）→ 裁剪。
        // 调用方拿到即可直接保存/写剪贴板，杜绝"两处各做一次合成导致不一致"。
        outcome_.outputImage = buildFinalImage();
        qInfo().noquote()
            << QStringLiteral("[jie2] 提交选区 逻辑=%1,%2 %3x%4 像素=%5,%6 %7x%8 源=%9 标注=%10 输出=%11x%12")
                   .arg(outcome_.selection.x())
                   .arg(outcome_.selection.y())
                   .arg(outcome_.selection.width())
                   .arg(outcome_.selection.height())
                   .arg(outcome_.pixelRegion.x())
                   .arg(outcome_.pixelRegion.y())
                   .arg(outcome_.pixelRegion.width())
                   .arg(outcome_.pixelRegion.height())
                   .arg(isHistory ? QStringLiteral("历史") : QStringLiteral("当前屏"))
                   .arg(annotations_.size())
                   .arg(outcome_.outputImage.width())
                   .arg(outcome_.outputImage.height());
        accept();
    } else {
        reject();
    }
}

QColor SelectionOverlay::pixelColorAt(const QPoint& logicalPos) const {
    if (frozenImage_.isNull() || frozenLogicalRect_.isEmpty()) {
        return {};
    }
    // 逻辑点 → 冻结画面的物理像素坐标（冻结画面未设 DPR，坐标即物理像素）。
    // 历史图可能小于屏幕且居中，必须经 frozenLogicalRect_ 换算，
    // 不能直接乘 scaleFactor_。
    const int x = qBound(0,
                         qRound((logicalPos.x() - frozenLogicalRect_.x()) *
                                frozenPixelScale_),
                         frozenImage_.width() - 1);
    const int y = qBound(0,
                         qRound((logicalPos.y() - frozenLogicalRect_.y()) *
                                frozenPixelScale_),
                         frozenImage_.height() - 1);
    return frozenImage_.pixelColor(x, y);
}

// ---- 标注（M2）----

QPoint SelectionOverlay::imagePos(const QPoint& logical) const {
    if (frozenLogicalRect_.isEmpty()) {
        return {};
    }
    // 与 pixelColorAt 同一套换算：逻辑点 → 源图像素。
    const double x = (logical.x() - frozenLogicalRect_.x()) * frozenPixelScale_;
    const double y = (logical.y() - frozenLogicalRect_.y()) * frozenPixelScale_;
    return QPoint(qRound(x), qRound(y));
}

core::Rgba SelectionOverlay::currentColor() const {
    // 调色板：对截图对比度最好的颜色（专业版对齐项：颜色数量可自定义）。
    // 前几个是"最常用"的，配置调小数量时保留的正是这些。
    static const core::Rgba palette[] = {
        {239, 68, 68, 255},   // 红
        {250, 204, 21, 255},  // 黄
        {34, 197, 94, 255},   // 绿
        {59, 130, 246, 255},  // 蓝
        {255, 255, 255, 255}, // 白
        {20, 20, 20, 255},    // 黑
        {168, 85, 247, 255},  // 紫
        {249, 115, 22, 255},  // 橙
        {6, 182, 212, 255},   // 青
        {236, 72, 153, 255},  // 粉
    };
    constexpr int kPaletteSize =
        static_cast<int>(sizeof(palette) / sizeof(palette[0]));
    // 生效数量：配置值夹在 [2, kPaletteSize]（少于 2 个颜色无法切换）。
    const int count = std::clamp(paletteSize_, 2, kPaletteSize);
    // 取**当前工具**自己的颜色索引（专业版 v2.4：形状分别记忆颜色）。
    const int index =
        colorIndexByTool_[static_cast<int>(currentTool_) % 16];
    return palette[((index % count) + count) % count];
}

void SelectionOverlay::rebuildComposed() {
    if (frozenImage_.isNull()) {
        composedBase_ = QImage();
        displayPixmap_ = QPixmap();
        composedDirty_ = false;
        return;
    }
    // 底图 = 源图 + 已提交的马赛克/模糊（矢量标注在 paintEvent 里实时绘制，
    // 因为它们是"可撤销、可重绘"的轻量对象；像素效果一旦烘焙就与底图一体）。
    composedBase_ = composedAnnotationsBase(frozenImage_, annotations_.items());
    displayPixmap_ = QPixmap::fromImage(composedBase_);
    composedDirty_ = false;
}

QImage SelectionOverlay::buildFinalImage() const {
    // 完整合成：像素效果 + 矢量标注 → 按选区裁剪。
    const QImage flattened =
        composeAnnotations(frozenImage_, annotations_.items());
    const QRect pixelRegion = selectionToPixelRegion(currentSelection());
    if (pixelRegion.isEmpty()) {
        return {};
    }
    return flattened.copy(pixelRegion);
}

void SelectionOverlay::paintAnnotations(QPainter& painter,
                                        const QRect& selection) const {
    // painter 当前为逻辑坐标系；把标注按"逻辑点 = 源图像素 / 缩放"呈现。
    painter.save();
    painter.setClipRect(selection);
    painter.translate(frozenLogicalRect_.x(), frozenLogicalRect_.y());
    const double scale = frozenPixelScale_ > 0.0 ? frozenPixelScale_ : 1.0;
    painter.scale(1.0 / scale, 1.0 / scale);

    for (const core::Annotation& item : annotations_.items()) {
        paintAnnotation(painter, item);
    }
    if (drawing_) {
        if (draft_.tool == core::AnnotationTool::Mosaic ||
            draft_.tool == core::AnnotationTool::Blur) {
            paintPixelOpHint(painter, draft_);
        } else if (draft_.points.size() >= 2) {
            paintAnnotation(painter, draft_);
        }
    }

    // 自由选中（专业版 v2.8）：给选中的标注描虚线包围盒 + 四角手柄。
    // 坐标即源图像素（与上方标注同一变换）。
    // 四角手柄只在可缩放的矩形/椭圆上绘制（它们是缩放拖拽的把手，
    // v2.3；画在其他工具上会给出错误的交互暗示）。
    if (selectedIndex_ >= 0 &&
        static_cast<std::size_t>(selectedIndex_) < annotations_.size()) {
        const core::Annotation& selectedItem =
            annotations_.items()[static_cast<std::size_t>(selectedIndex_)];
        const core::Rect bounds = selectedItem.boundingRect();
        painter.setBrush(Qt::NoBrush);
        QPen pen(QColor(64, 158, 255), 0);  // cosmetic：屏显恒定 1px
        pen.setStyle(Qt::DashLine);
        painter.setPen(pen);
        const QRectF box(bounds.x - 4.0, bounds.y - 4.0,
                         bounds.width + 8.0, bounds.height + 8.0);
        painter.drawRect(box);

        const bool scalable =
            selectedItem.tool == core::AnnotationTool::Rect ||
            selectedItem.tool == core::AnnotationTool::Ellipse;
        if (scalable) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(64, 158, 255));
            constexpr double kMark = 6.0;
            const QPointF corners[] = {box.topLeft(), box.topRight(),
                                       box.bottomLeft(), box.bottomRight()};
            for (const QPointF& corner : corners) {
                painter.drawRect(
                    QRectF(corner.x() - kMark / 2.0, corner.y() - kMark / 2.0,
                           kMark, kMark));
            }
        }
    }
    painter.restore();
}

void SelectionOverlay::performUndo() {
    if (!annotations_.undo()) {
        showToast(QStringLiteral("没有可撤销的标注"));
        return;
    }
    selectedIndex_ = -1;  // 条目集合变了，选中失效
    rebuildComposed();
    update();
    showToast(QStringLiteral("已撤销"));
    qInfo().noquote() << QStringLiteral("[jie2] 撤销（剩 %1 条）")
                             .arg(annotations_.size());
}

void SelectionOverlay::performRedo() {
    if (!annotations_.redo()) {
        showToast(QStringLiteral("没有可重做的标注"));
        return;
    }
    selectedIndex_ = -1;  // 条目集合变了，选中失效
    rebuildComposed();
    update();
    showToast(QStringLiteral("已重做"));
    qInfo().noquote() << QStringLiteral("[jie2] 重做（共 %1 条）")
                             .arg(annotations_.size());
}

void SelectionOverlay::handleSelectClick(const QPoint& logicalPos) {
    const QPoint img = imagePos(logicalPos);
    const auto& items = annotations_.items();

    // 从最上层（最后画的）往下找命中。包围盒加 4px 宽容：
    // 细线类标注（箭头/铅笔）的包围盒只有线宽那么宽，精确命中太难。
    int hit = -1;
    constexpr int kHitPad = 4;
    for (int i = static_cast<int>(items.size()) - 1; i >= 0; --i) {
        const core::Rect bounds =
            items[static_cast<std::size_t>(i)].boundingRect();
        if (img.x() >= bounds.x - kHitPad &&
            img.x() <= bounds.x + bounds.width + kHitPad &&
            img.y() >= bounds.y - kHitPad &&
            img.y() <= bounds.y + bounds.height + kHitPad) {
            hit = i;
            break;
        }
    }

    selectedIndex_ = hit;
    if (hit >= 0) {
        const QString tool = QString::fromUtf8(
            toolDisplayName(items[static_cast<std::size_t>(hit)].tool));
        qInfo().noquote()
            << QStringLiteral("[jie2] 自由选中：标注 %1/%2（%3）")
                   .arg(hit + 1)
                   .arg(items.size())
                   .arg(tool);
        showToast(QStringLiteral("已选中「%1」 · 拖动移动 · 角柄缩放 · Delete 删除")
                      .arg(tool));
    } else if (!items.empty()) {
        qInfo().noquote() << QStringLiteral("[jie2] 自由选中：未命中（点击处无标注）");
        showToast(QStringLiteral("点击处没有标注"));
    }
    update();
}

// ---- 选中标注的移动 / 缩放手势（专业版对齐项 v2.3 / v2.11）----

std::array<QPoint, 4> SelectionOverlay::selectedHandles() const {
    if (selectedIndex_ < 0 ||
        static_cast<std::size_t>(selectedIndex_) >= annotations_.size()) {
        return {};
    }
    const core::Rect b = annotations_
                             .items()[static_cast<std::size_t>(selectedIndex_)]
                             .boundingRect();
    // 与绘制一致：包围盒外扩 4px（图像像素）。
    return {QPoint(b.x - 4, b.y - 4),
            QPoint(b.x + b.width + 4, b.y - 4),
            QPoint(b.x - 4, b.y + b.height + 4),
            QPoint(b.x + b.width + 4, b.y + b.height + 4)};
}

bool SelectionOverlay::hitSelectedHandle(const QPoint& logicalPos,
                                         QPoint* corner) const {
    if (selectedIndex_ < 0 ||
        static_cast<std::size_t>(selectedIndex_) >= annotations_.size()) {
        return false;
    }
    const auto handles = selectedHandles();
    const QPoint img = imagePos(logicalPos);
    const double scale = frozenPixelScale_ > 0.0 ? frozenPixelScale_ : 1.0;
    const int pad = static_cast<int>(std::lround(12.0 * scale));
    for (const QPoint& handle : handles) {
        if (std::abs(img.x() - handle.x()) <= pad &&
            std::abs(img.y() - handle.y()) <= pad) {
            if (corner != nullptr) {
                *corner = handle;
            }
            return true;
        }
    }
    return false;
}

void SelectionOverlay::beginMoveSelection(const QPoint& logicalPos) {
    movingSelection_ = true;
    gestureStartLogical_ = logicalPos;
    const core::Annotation& item =
        annotations_.items()[static_cast<std::size_t>(selectedIndex_)];
    gestureBasePoints_ = item.points;
    gestureBaseBounds_ = item.boundingRect();  // 日志用（移动前后对照）
}

void SelectionOverlay::beginResizeSelection(const QPoint& corner) {
    resizingSelection_ = true;
    gestureGrabCorner_ = corner;
    const core::Annotation& item =
        annotations_.items()[static_cast<std::size_t>(selectedIndex_)];
    gestureBasePoints_ = item.points;
    gestureBaseBounds_ = item.boundingRect();
    gestureCenterAnchor_ = false;
    gestureProportional_ = false;
}

void SelectionOverlay::updateMoveSelection(const QPoint& logicalPos) {
    if (selectedIndex_ < 0 ||
        static_cast<std::size_t>(selectedIndex_) >= annotations_.size()) {
        return;
    }
    // 位移按图像像素计算（手势起点到当前点），从快照重新推导全部点集。
    const QPoint from = imagePos(gestureStartLogical_);
    const QPoint to = imagePos(logicalPos);
    const int dx = to.x() - from.x();
    const int dy = to.y() - from.y();
    std::vector<core::Point> points = gestureBasePoints_;
    for (core::Point& point : points) {
        point.x += dx;
        point.y += dy;
    }
    annotations_.replacePoints(
        static_cast<std::size_t>(selectedIndex_), std::move(points),
        /*record=*/false);
    rebuildComposed();
    update();
}

void SelectionOverlay::updateResizeSelection(const QPoint& logicalPos,
                                             Qt::KeyboardModifiers mods) {
    if (selectedIndex_ < 0 ||
        static_cast<std::size_t>(selectedIndex_) >= annotations_.size()) {
        return;
    }
    const QPoint m = imagePos(logicalPos);
    // box 坐标（与手柄绘制一致：包围盒外扩 4px）。
    const double left = gestureBaseBounds_.x - 4;
    const double top = gestureBaseBounds_.y - 4;
    const double right =
        gestureBaseBounds_.x + gestureBaseBounds_.width + 4;
    const double bottom =
        gestureBaseBounds_.y + gestureBaseBounds_.height + 4;
    const double c0x = gestureGrabCorner_.x();
    const double c0y = gestureGrabCorner_.y();
    // 缩放基准（v2.3）：默认 = 对角（拖哪个角，对角固定）；
    // 按住 Ctrl = 中心点缩放。
    gestureCenterAnchor_ = mods.testFlag(Qt::ControlModifier);
    double ax = 0.0;
    double ay = 0.0;
    if (gestureCenterAnchor_) {
        ax = (left + right) / 2.0;
        ay = (top + bottom) / 2.0;
    } else {
        ax = (c0x <= left + 0.5) ? right : left;
        ay = (c0y <= top + 0.5) ? bottom : top;
    }
    double sx = 1.0;
    double sy = 1.0;
    if (std::abs(c0x - ax) >= 4.0) {
        sx = (static_cast<double>(m.x()) - ax) / (c0x - ax);
    }
    if (std::abs(c0y - ay) >= 4.0) {
        sy = (static_cast<double>(m.y()) - ay) / (c0y - ay);
    }
    // 防止缩到零/负（不允许翻转）；上限防止失控放大。
    sx = std::clamp(sx, 0.05, 50.0);
    sy = std::clamp(sy, 0.05, 50.0);
    // 等比例缩放（v2.3）：Shift → 取主导轴的比例，两轴一致（保纵横比）。
    gestureProportional_ = mods.testFlag(Qt::ShiftModifier);
    if (gestureProportional_) {
        const double factor =
            std::abs(sx - 1.0) >= std::abs(sy - 1.0) ? sx : sy;
        sx = factor;
        sy = factor;
    }
    std::vector<core::Point> points = gestureBasePoints_;
    for (core::Point& point : points) {
        point.x = static_cast<int>(std::lround(ax + (point.x - ax) * sx));
        point.y = static_cast<int>(std::lround(ay + (point.y - ay) * sy));
    }
    annotations_.replacePoints(
        static_cast<std::size_t>(selectedIndex_), std::move(points),
        /*record=*/false);
    rebuildComposed();
    update();
}

void SelectionOverlay::finishSelectionGesture() {
    const bool wasMove = movingSelection_;
    const bool wasResize = resizingSelection_;
    movingSelection_ = false;
    resizingSelection_ = false;
    if (selectedIndex_ < 0 ||
        static_cast<std::size_t>(selectedIndex_) >= annotations_.size()) {
        return;
    }
    // 收尾：把手势中的实时修改一次性入历史（中间帧不进撤销栈）。
    annotations_.commitPending();
    const core::Rect now = annotations_
                               .items()[static_cast<std::size_t>(selectedIndex_)]
                               .boundingRect();
    if (wasMove) {
        qInfo().noquote()
            << QStringLiteral("[jie2] 拖动选中标注：(%1,%2) %3x%4 → (%5,%6) %7x%8")
                   .arg(gestureBaseBounds_.x)
                   .arg(gestureBaseBounds_.y)
                   .arg(gestureBaseBounds_.width)
                   .arg(gestureBaseBounds_.height)
                   .arg(now.x)
                   .arg(now.y)
                   .arg(now.width)
                   .arg(now.height);
        showToast(QStringLiteral("已移动选中标注"));
    } else if (wasResize) {
        const QString mode =
            gestureCenterAnchor_
                ? (gestureProportional_ ? QStringLiteral("中心+等比")
                                        : QStringLiteral("中心"))
                : (gestureProportional_ ? QStringLiteral("等比")
                                        : QStringLiteral("对角"));
        qInfo().noquote()
            << QStringLiteral("[jie2] 拖动缩放：%1x%2 → %3x%4（基准：%5）")
                   .arg(gestureBaseBounds_.width)
                   .arg(gestureBaseBounds_.height)
                   .arg(now.width)
                   .arg(now.height)
                   .arg(mode);
        showToast(QStringLiteral("已缩放选中标注"));
    }
    update();
}

// ---- 文字与序号（M2 增补）----

void SelectionOverlay::startTextInput(const QPoint& logicalPos) {
    cancelTextInput();
    const QPoint imagePoint = imagePos(logicalPos);
    textAnchorImage_ = imagePoint;

    textInput_ = new QLineEdit(this);
    textInput_->setPlaceholderText(QStringLiteral("输入文字，回车确认"));
    QFont font;
    font.setPixelSize(16);
    textInput_->setFont(font);
    textInput_->setStyleSheet(
        QStringLiteral("QLineEdit { background: rgba(28,28,32,240); color: white; "
                       "border: 2px solid rgb(64,158,255); border-radius: 4px; "
                       "padding: 2px 6px; }"));
    textInput_->setMinimumWidth(220);
    // 定位在点击点右下（避免被文字遮挡）；贴边时回退。
    int x = logicalPos.x() + 8;
    int y = logicalPos.y() + 8;
    if (x + 240 > width()) {
        x = std::max(4, logicalPos.x() - 248);
    }
    if (y + 34 > height()) {
        y = std::max(4, logicalPos.y() - 42);
    }
    textInput_->setGeometry(x, y, 240, 32);
    textInput_->installEventFilter(this);
    textInput_->show();
    textInput_->setFocus();
    connect(textInput_, &QLineEdit::returnPressed, this,
            &SelectionOverlay::commitTextInput);
    qInfo().noquote() << QStringLiteral("[jie2] 文字输入开始 @ 像素(%1,%2)")
                             .arg(imagePoint.x())
                             .arg(imagePoint.y());
}

void SelectionOverlay::commitTextInput() {
    if (textInput_ == nullptr) {
        return;
    }
    const QString text = textInput_->text().trimmed();
    cancelTextInput();
    if (text.isEmpty()) {
        showToast(QStringLiteral("文字为空，未添加"));
        update();
        return;
    }
    core::Annotation item;
    item.tool = core::AnnotationTool::Text;
    item.style.color = currentColor();
    item.style.width = currentWidth_;
    // 文字大小随"粗细"调节（与线宽同一个手感维度），范围 12..72。
    item.style.fontSize = 12 + currentWidth_ * 3;
    item.points.push_back(
        core::Point{textAnchorImage_.x(), textAnchorImage_.y()});
    item.text = text.toStdString();
    if (annotations_.add(std::move(item))) {
        rebuildComposed();
        qInfo().noquote() << QStringLiteral("[jie2] 标注已添加：text（共 %1 条）")
                                 .arg(annotations_.size());
        showToast(QStringLiteral("已添加文字"));
    }
    update();
}

void SelectionOverlay::cancelTextInput() {
    if (textInput_ != nullptr) {
        textInput_->removeEventFilter(this);
        textInput_->deleteLater();
        textInput_ = nullptr;
    }
}

int SelectionOverlay::nextNumberValue() const {
    // 基数是"已有序号数 + 1"（撤销后数字重用 —— 与 nextNumberLabel 同语义），
    // 再叠加用户用 +/- 做的偏移，下限 1。
    return std::max(1, std::stoi(annotations_.nextNumberLabel()) + numberOffset_);
}

void SelectionOverlay::placeNumberMarker(const QPoint& logicalPos) {
    const QPoint imagePoint = imagePos(logicalPos);
    core::Annotation item;
    item.tool = core::AnnotationTool::Number;
    item.style.color = currentColor();
    item.style.width = currentWidth_;
    item.points.push_back(core::Point{imagePoint.x(), imagePoint.y()});
    item.text = std::to_string(nextNumberValue());
    // 先留副本再 move：move 之后 item.text 已空，日志会显示成 "number="。
    const QString label = QString::fromStdString(item.text);
    if (annotations_.add(std::move(item))) {
        rebuildComposed();
        qInfo().noquote() << QStringLiteral("[jie2] 标注已添加：number=%1（共 %2 条）")
                                 .arg(label)
                                 .arg(annotations_.size());
        showToast(QStringLiteral("序号 %1 · 点击继续编号").arg(label));
    }
    update();
}

bool SelectionOverlay::eventFilter(QObject* watched, QEvent* event) {
    if (watched == textInput_ && event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Escape) {
            cancelTextInput();
            showToast(QStringLiteral("已取消文字输入"));
            update();
            return true;  // 吞掉 Esc，不让它关闭整个遮罩
        }
        if (keyEvent->key() == Qt::Key_Return ||
            keyEvent->key() == Qt::Key_Enter) {
            // 关键：必须在这里吞掉回车。
            //
            // 实测（容器内 Xvfb + xdotool 复现）：若只在 returnPressed 里提交，
            // QLineEdit 处理后同一个回车还会冒泡到对话框的 keyPressEvent，
            // 命中"回车 = 确认截图"分支 —— 结果文字刚提交，整个截图就被确认
            // 保存了，用户根本没机会继续画序号。表现为"文字之后的点击全部失效"。
            commitTextInput();
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void SelectionOverlay::paintEvent(QPaintEvent*) {
    // 像素类标注（马赛克/模糊）烘焙进底图后作为画面基准；
    // 矢量标注实时绘制（可撤销、可重绘）。
    if (composedDirty_) {
        rebuildComposed();
    }

    QPainter painter(this);
    // 冻结画面：不做平滑缩放 —— 逻辑点与物理像素成整数倍时是 1:1 直通。
    // 当前屏画面铺满屏幕；历史图按 frozenLogicalRect_ 居中显示（图外留黑）。
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.fillRect(rect(), Qt::black);
    painter.drawPixmap(frozenLogicalRect_,
                       displayPixmap_.isNull() ? frozen_ : displayPixmap_);

    const QRect selection = currentSelection();
    if (selection.isEmpty()) {
        painter.fillRect(rect(), kDim);
    } else {
        // 选区外变暗，选区内保持原样。
        QPainterPath full;
        full.addRect(QRectF(rect()));
        QPainterPath hole;
        hole.addRect(QRectF(selection));
        painter.fillPath(full.subtracted(hole), kDim);
        // 标注画在遮罩之上、边框之下（裁剪到选区，所见即所存）。
        paintAnnotations(painter, selection);
        paintSelectionChrome(painter, selection);
        // 工具条只在选区确定后出现，避免拖拽过程中跟随抖动。
        if (hasSelection_) {
            paintToolbar(painter, selection);
        }
    }

    if (!hasSelection_) paintHint(painter);

    if ((hasCursor_ || dragging_) && toolbarHover_ < 0) {
        paintMagnifier(painter);
    }

    if (!toast_.isEmpty()) {
        painter.setFont(labelFont());
        const QFontMetrics metrics(labelFont());
        const QSize textSize = metrics.size(Qt::TextSingleLine, toast_);
        const QRect toastRect(QPoint((width() - textSize.width()) / 2,
                                     height() - 72),
                              QSize(textSize.width() + 24, textSize.height() + 12));
        painter.setPen(Qt::NoPen);
        painter.setBrush(chrome_.panelBg);
        painter.drawRoundedRect(toastRect, 6, 6);
        painter.setPen(chrome_.textFg);
        painter.drawText(toastRect, Qt::AlignCenter, toast_);
    }
}

void SelectionOverlay::paintHint(QPainter& painter) const {
    // 提示随可用功能变化：无历史时不提回放，无刷新回调时不提 F5。
    // 快捷键标签**动态取自 ShortcutMap**（v2.11.3 可自定义）——
    // 用户改了绑定，提示条同步反映；禁用（none）显示为 "—"。
    const auto keyLabel = [this](const char* action) {
        const core::ShortcutSpec spec = shortcuts_.get(action);
        if (!spec.enabled()) {
            return QStringLiteral("—");
        }
        QString label;
        if ((spec.modifiers & core::kScCtrl) != 0) {
            label += QStringLiteral("Ctrl+");
        }
        if ((spec.modifiers & core::kScAlt) != 0) {
            label += QStringLiteral("Alt+");
        }
        if ((spec.modifiers & core::kScShift) != 0) {
            label += QStringLiteral("Shift+");
        }
        label += QChar(static_cast<char16_t>(spec.key));
        return label;
    };

    QStringList parts;
    if (hasSelection_) {
        parts << QStringLiteral("拖动八手柄调整选区 · 选区内拖拽绘制 · 工具见工具条");
        parts << QStringLiteral("快捷键：%1 虚线 / %2 圆角 / %3 椭圆 / %4 双向箭头")
                     .arg(keyLabel("style.dashed"), keyLabel("style.corner"),
                          keyLabel("shape.ellipse"), keyLabel("arrow.both"));
        parts << QStringLiteral("[ ] 粗细 · Ctrl+Z 撤销");
        parts << QStringLiteral("回车 / 双击确认");
    } else {
        parts << QStringLiteral("拖拽选择截图区域");
    }
    parts << QStringLiteral("Esc 取消");
    parts << QStringLiteral("C 复制色值");
    if (!historyPaths_.isEmpty()) {
        parts << QStringLiteral(", / . 回放历史");
    }
    if (recapture_) {
        parts << QStringLiteral("F5 刷新");
    }
    const QString text = parts.join(QStringLiteral(" · "));
    painter.setFont(smallFont());
    const QFontMetrics metrics(smallFont());
    const int textWidth = qMax(1, width() - 44);
    const QRect textBounds = metrics.boundingRect(QRect(0, 0, textWidth, 1000),
                                                  Qt::TextWordWrap, text);
    const QSize hintSize(textBounds.width() + 20, textBounds.height() + 10);
    const QRect hintRect(QPoint((width() - hintSize.width()) / 2, 16), hintSize);
    painter.setPen(Qt::NoPen);
    painter.setBrush(chrome_.panelBg);
    painter.drawRoundedRect(hintRect, 5, 5);
    painter.setPen(chrome_.textFg);
    painter.drawText(hintRect.adjusted(10, 5, -10, -5), Qt::AlignCenter | Qt::TextWordWrap, text);
}

void SelectionOverlay::paintSelectionChrome(QPainter& painter,
                                            const QRect& selection) const {
    // 吸附提示：被吸附的边画一条更亮的实线（拖拽中才显示），
    // 让用户明确知道"这条边贴上了窗口边缘"，而不是疑惑为何拖不动。
    if (dragging_ && snapActive_) {
        painter.setPen(QPen(kSnapColor, 2));
        if (lastSnap_.snappedLeft) {
            painter.drawLine(selection.topLeft(), selection.bottomLeft());
        }
        if (lastSnap_.snappedRight) {
            painter.drawLine(QPoint(selection.right(), selection.top()),
                             QPoint(selection.right(), selection.bottom()));
        }
        if (lastSnap_.snappedTop) {
            painter.drawLine(selection.topLeft(), selection.topRight());
        }
        if (lastSnap_.snappedBottom) {
            painter.drawLine(QPoint(selection.left(), selection.bottom()),
                             QPoint(selection.right(), selection.bottom()));
        }
    }

    // 边框
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(kAccent, 1));
    painter.drawRect(selection.adjusted(0, 0, -1, -1));

    // 八个圆形手柄：边中点与角点均可拖动。
    painter.setPen(QPen(Qt::white, 1.2));
    painter.setBrush(kAccent);
    for (const auto& point : cropHandles()) painter.drawEllipse(QPointF(point), 5.5, 5.5);

    // 与参考图一致，默认只显示尺寸；macOS 使用逻辑点并标注 pt。
#ifdef Q_OS_MACOS
    const QString text = QStringLiteral("%1 × %2  pt").arg(selection.width()).arg(selection.height());
#else
    // 变量名不能叫 size：下方还有一块同名的作用域内局部变量。
    // 这处重复声明只会在非 macOS 分支被编译，因此 macOS 上发现不了
    // （Linux 的 GCC 直接报 redeclaration 编译失败）。
    const QSize pixelSize = selectionToPixelRegion(selection).size();
    const QString text =
        QStringLiteral("%1 × %2  px").arg(pixelSize.width()).arg(pixelSize.height());
#endif
    QFont font = smallFont();
    font.setPointSize(12);
    font.setWeight(QFont::Medium);
    const QSize size = QFontMetrics(font).size(Qt::TextSingleLine, text) + QSize(14, 8);
    const int x = qBound(6, selection.left(), qMax(6, width() - size.width() - 6));
    const int y = selection.top() >= size.height() + 10 ?
        selection.top() - size.height() - 6 : selection.top() + 10;
    const QRect label(QPoint(x, y), size);
    painter.setPen(QPen(QColor(120, 125, 140, 180), .8));
    painter.setBrush(QColor(22, 24, 32, 240));
    painter.drawRoundedRect(label, 6, 6);
    painter.setPen(QColor(240, 240, 245));
    painter.setFont(font);
    painter.drawText(label, Qt::AlignCenter, text);
}

std::array<QPoint, 8> SelectionOverlay::cropHandles() const {
    const QRect r = currentSelection();
    return {r.topLeft(), QPoint(r.center().x(), r.top()), r.topRight(),
            QPoint(r.right(), r.center().y()), r.bottomRight(),
            QPoint(r.center().x(), r.bottom()), r.bottomLeft(), QPoint(r.left(), r.center().y())};
}

int SelectionOverlay::cropHandleAt(const QPoint& pos) const {
    if (!hasSelection_) return -1;
    const auto handles = cropHandles();
    // 命中区 14（曼哈顿距离）：手柄圆点本身只有 ~5px，命中区太小会让用户
    // 以为"选区不能调整"（实测反馈即如此）——命中区必须明显大于视觉尺寸。
    for (int i = 0; i < 8; ++i)
        if ((handles[i] - pos).manhattanLength() <= 14) return i;
    return -1;
}

Qt::CursorShape SelectionOverlay::cursorForCropHandle(int handle) {
    // 手柄序号：0 TL, 1 上中, 2 TR, 3 右中, 4 BR, 5 下中, 6 BL, 7 左中。
    switch (handle) {
        case 0: case 4: return Qt::SizeFDiagCursor;
        case 2: case 6: return Qt::SizeBDiagCursor;
        case 1: case 5: return Qt::SizeVerCursor;
        case 3: case 7: return Qt::SizeHorCursor;
        default: return Qt::CrossCursor;
    }
}

void SelectionOverlay::updateCropGesture(const QPoint& pos) {
    const QPoint delta = pos - cropPress_;
    if (movingCrop_) {
        QRect r = cropStart_.translated(delta);
        r.moveLeft(qBound(0, r.left(), qMax(0, width() - r.width())));
        r.moveTop(qBound(0, r.top(), qMax(0, height() - r.height())));
        selectionRect_ = r;
    } else {
        int left = cropStart_.left(), right = cropStart_.right();
        int top = cropStart_.top(), bottom = cropStart_.bottom();
        if (cropHandle_ == 0 || cropHandle_ == 6 || cropHandle_ == 7) left += delta.x();
        if (cropHandle_ == 2 || cropHandle_ == 3 || cropHandle_ == 4) right += delta.x();
        if (cropHandle_ == 0 || cropHandle_ == 1 || cropHandle_ == 2) top += delta.y();
        if (cropHandle_ == 4 || cropHandle_ == 5 || cropHandle_ == 6) bottom += delta.y();
        selectionRect_ = QRect(QPoint(left, top), QPoint(right, bottom)).normalized().intersected(rect());
    }
    update();
}

const std::vector<SelectionOverlay::ToolbarItem>&
SelectionOverlay::toolbarItems() {
    // 图标按钮保持紧凑；标签用于悬停提示。
    static const std::vector<ToolbarItem> items = {
        {ToolbarButton::Tool, core::AnnotationTool::Rect, "形状（右键选择矩形、椭圆）", 32},
        {ToolbarButton::Tool, core::AnnotationTool::Arrow, "箭头（右键调整样式和颜色）", 32},
        {ToolbarButton::Tool, core::AnnotationTool::Pencil, "画笔（右键选择粗细和颜色）", 32},
        {ToolbarButton::Tool, core::AnnotationTool::Marker, "荧光笔", 32},
        {ToolbarButton::Tool, core::AnnotationTool::Mosaic, "马赛克（右键选择模糊、放大镜）", 32},
        {ToolbarButton::Tool, core::AnnotationTool::Text, "文字（右键选择序号）", 32},
        {ToolbarButton::Tool, core::AnnotationTool::Eraser, "橡皮擦", 32},
        {ToolbarButton::Undo, core::AnnotationTool::Rect, "撤销", 32},
        {ToolbarButton::Redo, core::AnnotationTool::Rect, "重做", 32},
        {ToolbarButton::Cancel, core::AnnotationTool::Rect, "取消截图", 32},
        {ToolbarButton::Pin, core::AnnotationTool::Rect, "贴到屏幕", 32},
        {ToolbarButton::Save, core::AnnotationTool::Rect, "保存到文件", 32},
        {ToolbarButton::Copy, core::AnnotationTool::Rect, "复制图像", 32},
    };
    return items;
}

QRect SelectionOverlay::toolbarRect(const QRect& selection) const {
    const int count = static_cast<int>(toolbarItems().size());
    const int columns = qMax(1, qMin(count, (width() - 32) / 32));
    const int rows = (count + columns - 1) / columns;
    const int totalWidth = columns * 32 + (columns == count ? 12 : 0);
    const int totalHeight = rows * (kToolbarHeight + kToolbarGap) - kToolbarGap;
    int x = qBound(10, selection.right() - totalWidth + 1,
                   qMax(10, width() - totalWidth - 10));
    int y = selection.bottom() + 12;
    if (y + totalHeight > height() - 10) {
        y = selection.top() - totalHeight - 44;
    }
    y = qBound(10, y, qMax(10, height() - totalHeight - 10));
    return QRect(x, y, totalWidth, totalHeight);
}

QRect SelectionOverlay::toolbarButtonRect(const QRect& selection, int index) const {
    const QRect bar = toolbarRect(selection);
    const int count = static_cast<int>(toolbarItems().size());
    const int columns = qMin(count, qMax(1, (width() - 32) / 32));
    const int gap = columns == count ? (index >= 7 ? 6 : 0) + (index >= 9 ? 6 : 0) : 0;
    return QRect(bar.left() + (index % columns) * 32 + gap,
                 bar.top() + (index / columns) * (kToolbarHeight + kToolbarGap),
                 32, kToolbarHeight);
}

SelectionOverlay::ToolbarButton SelectionOverlay::toolbarHitTest(
    const QPoint& pos, int* index) const {
    if (index != nullptr) {
        *index = -1;
    }
    if (!hasSelection_) {
        return ToolbarButton::None;
    }
    const QRect selection = currentSelection();
    if (selection.isEmpty()) {
        return ToolbarButton::None;
    }
    const auto& items = toolbarItems();
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        if (toolbarButtonRect(selection, i).contains(pos)) {
            if (index != nullptr) {
                *index = i;
            }
            return items[static_cast<std::size_t>(i)].action;
        }
    }
    return ToolbarButton::None;
}

void SelectionOverlay::paintToolbar(QPainter& painter,
                                    const QRect& selection) const {
    const QRect bar = toolbarRect(selection);

    // 底板（比按钮群外扩 4pt，形成留白）。主题色（v2.6）。
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(39, 40, 43, 250));
    painter.drawRoundedRect(bar.adjusted(-4, -4, 4, 4), 6, 6);
    if (bar.width() >= 428) {
        painter.setPen(QPen(QColor(190, 190, 194, 150), .8));
        for (int index : {7, 9}) {
            const int x = toolbarButtonRect(selection, index).left() - 3;
            painter.drawLine(x, bar.top() + 7, x, bar.bottom() - 7);
        }
    }

    painter.setFont(labelFont());
    const auto& items = toolbarItems();
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const ToolbarItem& item = items[static_cast<std::size_t>(i)];
        const QRect rect = toolbarButtonRect(selection, i);

        QColor foreground(238, 238, 240);
        const bool enabled = item.action == ToolbarButton::Undo ? annotations_.canUndo() :
                             item.action == ToolbarButton::Redo ? annotations_.canRedo() : true;
        if (!enabled) foreground = QColor(130, 131, 135);
        const bool selected = toolChosen_ && item.action == ToolbarButton::Tool &&
            (currentTool_ == item.tool || (i == 0 && currentTool_ == core::AnnotationTool::Ellipse) ||
             (i == 4 && (currentTool_ == core::AnnotationTool::Blur || currentTool_ == core::AnnotationTool::Magnifier)) ||
             (i == 5 && currentTool_ == core::AnnotationTool::Number));
        if (enabled && (selected || (hasCursor_ && rect.contains(cursor_)))) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(selected ? QColor(255, 255, 255, 28) : QColor(255, 255, 255, 18));
            painter.drawRoundedRect(rect.adjusted(2, 2, -2, -2), 4, 4);
        }
        // 统一线条图标，不依赖字体中的特殊字符。
        painter.save();
        painter.translate(rect.center() - QPoint(10, 10));
        painter.setPen(QPen(foreground, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        if (item.action == ToolbarButton::Cancel) {
            painter.drawLine(5, 5, 15, 15); painter.drawLine(15, 5, 5, 15);
        } else if (item.action == ToolbarButton::Save) {
            painter.drawRect(QRectF(3, 2, 14, 16));
            painter.drawRect(QRectF(6, 2, 7, 5));
            painter.drawRect(QRectF(6, 12, 8, 6));
        } else if (item.action == ToolbarButton::Copy) {
            painter.drawRoundedRect(QRectF(5, 5, 11, 13), 1.5, 1.5);
            painter.drawPolyline(QPolygonF{QPointF(13, 2), QPointF(2, 2), QPointF(2, 14)});
        } else if (item.action == ToolbarButton::Pin) {
            painter.save(); painter.translate(10, 10); painter.rotate(42);
            painter.setBrush(foreground);
            painter.drawPolygon(QPolygonF{QPointF(-4,-8),QPointF(4,-8),QPointF(3,-5),QPointF(3,1),QPointF(6,4),QPointF(-6,4),QPointF(-3,1),QPointF(-3,-5)});
            painter.drawLine(0, 4, 0, 12); painter.restore();
        } else if (item.action == ToolbarButton::Undo || item.action == ToolbarButton::Redo) {
            if (item.action == ToolbarButton::Redo) { painter.translate(20, 0); painter.scale(-1, 1); }
            QPainterPath arrow;
            arrow.moveTo(8, 2); arrow.lineTo(1, 8); arrow.lineTo(8, 14); arrow.lineTo(8, 10);
            arrow.cubicTo(14, 9, 17, 13, 16, 18);
            arrow.cubicTo(21, 10, 16, 5, 8, 6); arrow.closeSubpath();
            painter.setPen(Qt::NoPen); painter.setBrush(foreground); painter.drawPath(arrow);
        } else {
            switch (item.tool) {
                case core::AnnotationTool::Rect:
                    painter.drawRoundedRect(QRectF(2, 3, 12, 11), 1, 1);
                    painter.setBrush(QColor(39, 40, 43));
                    painter.drawEllipse(QRectF(8, 9, 10, 7)); break;
                case core::AnnotationTool::Ellipse: painter.drawEllipse(QRectF(3, 4, 14, 12)); break;
                case core::AnnotationTool::Arrow:
                    painter.drawPolyline(QPolygonF{QPointF(2, 15), QPointF(7, 9), QPointF(12, 12), QPointF(17, 4)});
                    painter.setBrush(foreground); painter.drawPolygon(QPolygonF{QPointF(13, 5), QPointF(19, 2), QPointF(17, 8)}); break;
                case core::AnnotationTool::Text:
                    painter.drawLine(4, 4, 16, 4); painter.drawLine(10, 4, 10, 17); painter.drawLine(7, 17, 13, 17); break;
                case core::AnnotationTool::Number:
                    painter.drawEllipse(QRectF(2, 2, 16, 16)); painter.drawText(QRect(2, 1, 16, 18), Qt::AlignCenter, "1"); break;
                case core::AnnotationTool::Magnifier:
                    painter.drawEllipse(QRectF(2, 2, 11, 11)); painter.drawLine(12, 12, 18, 18); painter.drawLine(5, 7, 10, 7); painter.drawLine(7, 5, 7, 10); break;
                case core::AnnotationTool::Mosaic:
                    painter.setBrush(foreground); painter.setPen(Qt::NoPen);
                    painter.drawRoundedRect(QRectF(3, 3, 14, 14), 1.5, 1.5);
                    painter.setBrush(QColor(39, 40, 43));
                    painter.drawRect(QRectF(10, 3, 7, 7)); painter.drawRect(QRectF(3, 10, 7, 7));
                    break;
                case core::AnnotationTool::Blur:
                    for (int row=0; row<3; ++row) { QPen pen(foreground, 3-row*.7); painter.setPen(pen); painter.drawLine(4, 5+row*5, 16, 5+row*5); } break;
                case core::AnnotationTool::Eraser:
                    painter.setBrush(foreground);
                    painter.drawPolygon(QPolygonF{QPointF(3, 12), QPointF(11, 3), QPointF(18, 9), QPointF(11, 17), QPointF(8, 17)}); painter.drawLine(7, 8, 14, 14); break;
                case core::AnnotationTool::Marker:
                    painter.setPen(QPen(foreground, 5, Qt::SolidLine, Qt::FlatCap)); painter.drawLine(6, 13, 15, 4);
                    painter.setPen(QPen(foreground, 1.6)); painter.drawLine(3, 18, 17, 18); break;
                case core::AnnotationTool::Pencil:
                    painter.drawPolygon(QPolygonF{QPointF(3, 17), QPointF(5, 11), QPointF(14, 2), QPointF(18, 6), QPointF(9, 15)}); break;
            }
        }
        painter.restore();
    }
}

void SelectionOverlay::advanceToolColor() {
    // 只推进**当前工具**的颜色索引（专业版 v2.4：形状分别记忆颜色）。
    int& index = colorIndexByTool_[static_cast<int>(currentTool_) % 16];
    index += 1;
    const core::Rgba rgba = currentColor();
    const QString hex = QStringLiteral("#%1%2%3")
                            .arg(rgba.r, 2, 16, QLatin1Char('0'))
                            .arg(rgba.g, 2, 16, QLatin1Char('0'))
                            .arg(rgba.b, 2, 16, QLatin1Char('0'))
                            .toUpper();
    // 也写日志：调色板大小可配置、颜色按工具记忆 —— 日志是自动化验证与排障的依据。
    qInfo().noquote() << QStringLiteral("[jie2] 颜色：%1（工具=%2，调色板 %3 色，该工具第 %4 次切换）")
                             .arg(hex)
                             .arg(QString::fromUtf8(toolDisplayName(currentTool_)))
                             .arg(std::clamp(paletteSize_, 2, 10))
                             .arg(index);
    showToast(QStringLiteral("颜色 %1").arg(hex));
    update();

}

void SelectionOverlay::showToolMenu(int index, const QPoint& globalPos) {
    QMenu menu(this);
    std::vector<core::AnnotationTool> choices;
    using Tool = core::AnnotationTool;
    if (index == 0) choices = {Tool::Rect, Tool::Ellipse};
    else if (index == 4) choices = {Tool::Mosaic, Tool::Blur, Tool::Magnifier};
    else if (index == 5) choices = {Tool::Text, Tool::Number};
    else choices = {toolbarItems()[index].tool};
    if (std::find(choices.begin(), choices.end(), currentTool_) == choices.end()) currentTool_ = choices.front();
    toolChosen_ = true;
    for (const auto tool : choices) {
        auto* action = menu.addAction(QString::fromUtf8(toolDisplayName(tool)));
        action->setCheckable(true); action->setChecked(toolChosen_ && currentTool_ == tool);
        connect(action, &QAction::triggered, this, [this, tool] { currentTool_ = tool; toolChosen_ = true; update(); });
    }
    menu.addSeparator();
    for (int color = 0; color < std::clamp(paletteSize_, 2, 10); ++color) {
        auto* action = menu.addAction(QStringLiteral("颜色 %1").arg(color + 1));
        const int previous = colorIndexByTool_[static_cast<int>(currentTool_) % 16];
        colorIndexByTool_[static_cast<int>(currentTool_) % 16] = color;
        const auto rgba = currentColor();
        colorIndexByTool_[static_cast<int>(currentTool_) % 16] = previous;
        QPixmap swatch(16, 16); swatch.fill(QColor(rgba.r, rgba.g, rgba.b));
        action->setIcon(QIcon(swatch));
        connect(action, &QAction::triggered, this, [this, color, index] {
            if (!toolChosen_) currentTool_ = toolbarItems()[index].tool;
            colorIndexByTool_[static_cast<int>(currentTool_) % 16] = color;
            toolChosen_ = true; update();
        });
    }
    auto* widthMenu = menu.addMenu(QStringLiteral("粗细"));
    for (int width : {1, 2, 3, 5, 8, 12}) {
        auto* action = widthMenu->addAction(QString::number(width));
        connect(action, &QAction::triggered, this, [this, width] { currentWidth_ = width; update(); });
    }
    menu.exec(globalPos);
}

void SelectionOverlay::paintMagnifier(QPainter& painter) {
    const QPoint pos = cursor_;

    // 目标矩形（逻辑点）：优先右下，贴边时翻转到左上。
    const int totalHeight = kMagnifierSize + kChipHeight + 6;
    int x = pos.x() + kMagnifierGap;
    int y = pos.y() + kMagnifierGap;
    if (x + kMagnifierSize > width() - 8) {
        x = pos.x() - kMagnifierGap - kMagnifierSize;
    }
    if (y + totalHeight > height() - 8) {
        y = pos.y() - kMagnifierGap - totalHeight;
    }
    x = qBound(4, x, qMax(4, width() - kMagnifierSize - 4));
    y = qBound(4, y, qMax(4, height() - totalHeight - 4));
    const QRect target(x, y, kMagnifierSize, kMagnifierSize);

    // 源矩形（物理像素）：以光标处物理像素为中心，取 1/zoom 大小。
    const int srcSize = qMax(1, qRound(kMagnifierSize / double(kMagnifierZoom) *
                                       scaleFactor_));
    const int centerX = qRound(pos.x() * scaleFactor_);
    const int centerY = qRound(pos.y() * scaleFactor_);
    QRect src(centerX - srcSize / 2, centerY - srcSize / 2, srcSize, srcSize);

    const QRect bounds(0, 0, frozen_.width(), frozen_.height());
    src = src.intersected(bounds);
    if (src.isEmpty()) {
        return;
    }

    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawPixmap(target, frozen_, src);

    // 放大镜边框
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(255, 255, 255, 220), 1));
    painter.drawRect(target.adjusted(0, 0, -1, -1));
    painter.setPen(QPen(QColor(0, 0, 0, 120), 1));
    painter.drawRect(target.adjusted(-1, -1, 1, 1));

    // 中心十字（标记光标所在物理像素）
    const QPoint center = target.center();
    painter.setPen(QPen(QColor(255, 80, 80, 200), 1));
    painter.drawLine(center.x() - 5, center.y(), center.x() + 5, center.y());
    painter.drawLine(center.x(), center.y() - 5, center.x(), center.y() + 5);

    // 色值条
    const QColor color = pixelColorAt(pos);
    const QRect chipRect(target.left(), target.bottom() + 6, kMagnifierSize,
                         kChipHeight);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 200));
    painter.drawRoundedRect(chipRect, 4, 4);

    const QRect swatch(chipRect.left() + 5, chipRect.top() + 3, 16,
                       kChipHeight - 6);
    painter.setBrush(color);
    painter.drawRect(swatch);
    painter.setPen(QPen(QColor(255, 255, 255, 140), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(swatch);

    const QString text = color.name(QColor::HexRgb).toUpper();
    painter.setFont(smallFont());
    painter.setPen(QColor(235, 235, 235));
    painter.drawText(chipRect.adjusted(28, 0, -6, 0),
                     Qt::AlignVCenter | Qt::AlignLeft, text);
}

void SelectionOverlay::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::RightButton) {
        int index = -1;
        if (toolbarHitTest(event->pos(), &index) == ToolbarButton::Tool) {
            showToolMenu(index, event->globalPosition().toPoint());
            return;
        }
    }
    if (event->button() == Qt::RightButton) {
        // 绘制中：右键结束当前标注（丢弃草稿）；否则取消整个截图。
        if (drawing_) {
            drawing_ = false;
            draft_ = core::Annotation{};
            showToast(QStringLiteral("已取消当前标注"));
            update();
            return;
        }
        commit(false);
        return;
    }
    if (event->button() != Qt::LeftButton) {
        return;
    }

    // 工具条命中优先于开始拖拽：否则点按钮会变成"拖出一个极小的新选区"，
    // 把用户刚框好的选区弄丢。
    int index = -1;
    const ToolbarButton hit = toolbarHitTest(event->pos(), &index);
    const auto& items = toolbarItems();
    switch (hit) {
        case ToolbarButton::Save:
            outcome_.action = Action::SaveFile;
            commit(true);
            return;
        case ToolbarButton::Copy:
            outcome_.action = Action::Copy;
            commit(true);
            return;
        case ToolbarButton::Pin:
            outcome_.action = Action::Pin;
            commit(true);
            return;
        case ToolbarButton::Cancel:
            commit(false);
            return;
        case ToolbarButton::Tool:
            if (index >= 0 && index < static_cast<int>(items.size())) {
                currentTool_ = items[static_cast<std::size_t>(index)].tool;
                toolChosen_ = true;
                qInfo().noquote() << QStringLiteral("[jie2] 切换工具：%1")
                                         .arg(QString::fromUtf8(
                                             toolDisplayName(currentTool_)));
                showToast(QStringLiteral("工具：%1 · 滚轮调粗细 · Ctrl+Z 撤销")
                              .arg(QString::fromUtf8(
                                  toolDisplayName(currentTool_))));
            }
            update();
            return;
        case ToolbarButton::Undo:
            performUndo();
            return;
        case ToolbarButton::Redo:
            performRedo();
            return;
        case ToolbarButton::Color: {
            advanceToolColor();
            return;
        }

        case ToolbarButton::None:
            break;
    }

    cropHandle_ = cropHandleAt(event->pos());
    if (cropHandle_ >= 0 || (hasSelection_ && !toolChosen_ && selectionRect_.contains(event->pos()))) {
        cropStart_ = selectionRect_;
        cropPress_ = event->pos();
        movingCrop_ = cropHandle_ < 0;
        // 手势期间锁定光标：拖动中不再随位置重算。
        setCursor(movingCrop_ ? Qt::SizeAllCursor
                              : cursorForCropHandle(cropHandle_));
        return;
    }
    // 已有选区且点击落在选区内 → 开始绘制/放置标注（而不是重开选区）。
    // 这是与 Snipaste 一致的心智模型：选完区域后直接在区域内画。
    if (hasSelection_ && !selectionRect_.isEmpty() &&
        selectionRect_.contains(event->pos())) {
        // 选中手势优先（v2.3 / v2.11）：
        //   - 按在四角手柄上 → 缩放手势（含 Ctrl=中心点、Shift=等比，拖动中实时生效）；
        //   - 按在选中标注包围盒内 → 拖动移动。
        // 手柄检查必须放在 Ctrl 分支之前：否则 Ctrl+拖手柄会被"自由选中"吃掉。
        if (selectedIndex_ >= 0 &&
            static_cast<std::size_t>(selectedIndex_) < annotations_.size()) {
            const core::Annotation& selected =
                annotations_.items()[static_cast<std::size_t>(selectedIndex_)];
            const bool scalable = selected.tool == core::AnnotationTool::Rect ||
                                  selected.tool == core::AnnotationTool::Ellipse;
            QPoint corner;
            if (scalable && hitSelectedHandle(event->pos(), &corner)) {
                beginResizeSelection(corner);
                return;
            }
            // 拖动移动的例外：文字/序号是"点一下就要落点"的工具、橡皮擦是
            // 明确的"修改已有标注"工具 —— 这三者下按进包围盒不应被移动劫持，
            // 否则选中着标注就没法再往它上面放文字或擦除。
            const bool hijackableTool =
                currentTool_ != core::AnnotationTool::Text &&
                currentTool_ != core::AnnotationTool::Number &&
                currentTool_ != core::AnnotationTool::Eraser;
            if (hijackableTool &&
                !event->modifiers().testFlag(Qt::ControlModifier)) {
                const core::Rect bounds = selected.boundingRect();
                const QPoint img = imagePos(event->pos());
                constexpr int kMovePad = 4;
                if (img.x() >= bounds.x - kMovePad &&
                    img.x() <= bounds.x + bounds.width + kMovePad &&
                    img.y() >= bounds.y - kMovePad &&
                    img.y() <= bounds.y + bounds.height + kMovePad) {
                    beginMoveSelection(event->pos());
                    return;
                }
            }
        }
        // Ctrl+左键 = 自由选中（专业版 v2.8）：命中已有标注则选中，
        // 未命中则取消选中。选中后可拖动移动、角柄缩放、Delete 删除、
        // 方向键微移。
        if (event->modifiers().testFlag(Qt::ControlModifier)) {
            handleSelectClick(event->pos());
            return;
        }
        // 文字与序号是"点状"工具：一次点击完成，不走拖拽路径。
        if (currentTool_ == core::AnnotationTool::Text) {
            startTextInput(event->pos());
            return;
        }
        if (currentTool_ == core::AnnotationTool::Number) {
            placeNumberMarker(event->pos());
            return;
        }
        // 橡皮擦：按住拖动，擦过即整条删除（不产生新标注）。
        // 区域形状跟随"椭圆形状"（O 键）；区域角度由 Shift+[ ] 调整（v2.6）。
        if (currentTool_ == core::AnnotationTool::Eraser) {
            erasing_ = true;
            const QPoint img = imagePos(event->pos());
            const int half = currentWidth_ * 4;
            const std::size_t removed = annotations_.eraseIntersecting(
                core::Rect{img.x() - half, img.y() - half, half * 2, half * 2},
                eraserAngle_, ellipseShape_);
            if (removed > 0) {
                rebuildComposed();
                qInfo().noquote()
                    << QStringLiteral("[jie2] 橡皮擦删除 %1 条（剩 %2 条）")
                           .arg(removed)
                           .arg(annotations_.size());
            }
            cursor_ = event->pos();
            hasCursor_ = true;
            update();
            return;
        }
        drawing_ = true;
        draft_ = core::Annotation{};
        draft_.tool = currentTool_;
        draft_.style.color = currentColor();
        draft_.style.width = currentWidth_;
        draft_.style.dashed = dashed_;
        // 圆角只对矩形有意义（其他工具给 0）。
        draft_.style.cornerRadius =
            currentTool_ == core::AnnotationTool::Rect ? cornerRadius_ : 0;
        // 椭圆形状（专业版 v2.6）：对马克笔/马赛克/模糊生效。
        draft_.style.ellipseShape =
            ellipseShape_ &&
            (currentTool_ == core::AnnotationTool::Marker ||
             currentTool_ == core::AnnotationTool::Mosaic ||
             currentTool_ == core::AnnotationTool::Blur);
        // 箭头样式（专业版 v2.5）。
        draft_.style.arrowBothEnds = arrowBothEnds_;
        draft_.style.arrowReverse = arrowReverse_;
        // 马赛克块大小与模糊半径跟随"粗细"调节（与线宽一个手感维度）。
        draft_.style.blockSize = 6 + currentWidth_ * 3;
        draft_.style.blurRadius = 4 + currentWidth_ * 3;
        const QPoint img = imagePos(event->pos());
        draft_.points.push_back(core::Point{img.x(), img.y()});
        draft_.points.push_back(core::Point{img.x(), img.y()});
        cursor_ = event->pos();
        hasCursor_ = true;
        update();
        return;
    }

    dragging_ = true;
    hasSelection_ = false;
    selectionRect_ = QRect();
    snapActive_ = false;
    dragOrigin_ = event->pos();
    dragCurrent_ = event->pos();
    cursor_ = event->pos();
    hasCursor_ = true;
    update();
}

void SelectionOverlay::mouseMoveEvent(QMouseEvent* event) {
    if (cropHandle_ >= 0 || movingCrop_) { updateCropGesture(event->pos()); return; }
    cursor_ = event->pos();
    int hover = -1;
    toolbarHitTest(cursor_, &hover);
    if (hover != toolbarHover_) {
        toolbarHover_ = hover;
        QToolTip::hideText();
        if (hover >= 0) {
            QToolTip::showText(event->globalPosition().toPoint(),
                              QString::fromUtf8(toolbarItems()[hover].label), this);
        }
    }
    // 悬停光标反馈：工具条 → 手型；选区手柄 → 对应方向的调整光标；
    // 未选工具时的选区内部 → 移动光标。没有反馈时用户无从发现手柄可拖。
    if (drawing_ || dragging_ || erasing_ || movingSelection_ || resizingSelection_) {
        // 手势进行中：保持手势开始时的光标，不随位置跳动。
    } else if (hover >= 0) {
        setCursor(Qt::PointingHandCursor);
    } else {
        const int hoverHandle = cropHandleAt(cursor_);
        if (hoverHandle >= 0) {
            setCursor(cursorForCropHandle(hoverHandle));
        } else if (hasSelection_ && !toolChosen_ && selectionRect_.contains(cursor_)) {
            setCursor(Qt::SizeAllCursor);
        } else {
            setCursor(Qt::CrossCursor);
        }
    }

    hasCursor_ = true;
    if (erasing_) {
        const QPoint img = imagePos(event->pos());
        const int half = currentWidth_ * 4;
        const std::size_t removed = annotations_.eraseIntersecting(
            core::Rect{img.x() - half, img.y() - half, half * 2, half * 2},
            eraserAngle_, ellipseShape_);
        if (removed > 0) {
            rebuildComposed();
            qInfo().noquote() << QStringLiteral("[jie2] 橡皮擦删除 %1 条（剩 %2 条）")
                                     .arg(removed)
                                     .arg(annotations_.size());
        }
        update();
        return;
    }
    // 移动 / 缩放手势（v2.3）：从手势快照实时推导，帧间无累积误差。
    if (movingSelection_) {
        updateMoveSelection(event->pos());
        return;
    }
    if (resizingSelection_) {
        updateResizeSelection(event->pos(), event->modifiers());
        return;
    }
    if (drawing_) {
        const QPoint img = imagePos(event->pos());
        const core::Point point{img.x(), img.y()};
        switch (draft_.tool) {
            case core::AnnotationTool::Pencil:
            case core::AnnotationTool::Marker:
                // 自由曲线：按距离采样，避免点列过密（每 2 物理像素一个点）。
                if (draft_.points.empty() ||
                    std::abs(draft_.points.back().x - point.x) +
                            std::abs(draft_.points.back().y - point.y) >=
                        2) {
                    draft_.points.push_back(point);
                }
                break;
            default:
                // 两点形状（矩形/椭圆/箭头/马赛克/模糊）：末端跟随光标。
                if (draft_.points.size() >= 2) {
                    draft_.points.back() = point;
                } else {
                    draft_.points.push_back(point);
                }
                break;
        }
    } else if (dragging_) {
        dragCurrent_ = event->pos();
        // 窗口吸附（M1）：四条边各自尝试吸附到候选窗口边缘。
        // 阈值用逻辑点（8pt）—— 与手感相关，与缩放无关。
        const QRect raw = rectBetween(dragOrigin_, dragCurrent_).intersected(rect());
        const core::SnapResult snap =
            core::snapRect(core::Rect{raw.x(), raw.y(), raw.width(), raw.height()},
                           snapCandidates_, kSnapThresholdLogical);
        snapActive_ = snap.snapped();
        lastSnap_ = snap;
    }
    update();
}

void SelectionOverlay::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }

    if (cropHandle_ >= 0 || movingCrop_) {
        updateCropGesture(event->pos());
        cropHandle_ = -1; movingCrop_ = false;
        setCursor(Qt::CrossCursor);  // 手势结束，交还悬停判定（下次 mouseMove 细化）
        return;
    }
    // 移动 / 缩放手势收尾：一次性入历史 + 日志（手势期间的实时帧不进撤销栈）。
    if (movingSelection_ || resizingSelection_) {
        finishSelectionGesture();
        return;
    }

    if (erasing_) {
        erasing_ = false;
        showToast(QStringLiteral("橡皮擦：已删除擦过的标注"));
        update();
        return;
    }

    if (drawing_) {
        // 结束绘制：把终点写入草稿，校验有效性后提交。
        const QPoint img = imagePos(event->pos());
        if (draft_.points.size() >= 2) {
            draft_.points.back() = core::Point{img.x(), img.y()};
        }
        bool valid = draft_.points.size() >= 2;
        if (valid) {
            const core::Rect bounds = draft_.boundingRect();
            switch (draft_.tool) {
                case core::AnnotationTool::Rect:
                case core::AnnotationTool::Ellipse:
                case core::AnnotationTool::Mosaic:
                case core::AnnotationTool::Blur:
                    // 两点形状至少要有可辨识的面积。
                    valid = bounds.width >= 2 && bounds.height >= 2;
                    break;
                case core::AnnotationTool::Arrow:
                    valid = bounds.width >= 2 || bounds.height >= 2;
                    break;
                case core::AnnotationTool::Pencil:
                case core::AnnotationTool::Marker:
                    valid = draft_.points.size() >= 2;
                    break;
                default:
                    break;
            }
        }
        if (valid && annotations_.add(draft_)) {
            rebuildComposed();
            qInfo().noquote() << QStringLiteral("[jie2] 标注已添加：%1（共 %2 条）")
                                     .arg(QString::fromUtf8(core::toString(draft_.tool)))
                                     .arg(annotations_.size());
        }
        drawing_ = false;
        draft_ = core::Annotation{};
        update();
        return;
    }

    if (!dragging_) {
        return;
    }
    // 先取终点再结算选区：currentSelection() 在非拖拽且无选区时返回空，
    // 而 hasSelection_ 此刻还是 false —— 必须先把拖拽状态用于计算，
    // 之后再翻转 dragging_（早期版本顺序写反，选区永远登记不上）。
    dragCurrent_ = event->pos();
    const QRect selection = currentSelection();
    dragging_ = false;
    hasSelection_ = selection.width() >= kMinDragLogical &&
                    selection.height() >= kMinDragLogical;
    // 结算：把（可能已吸附的）选区固定下来，之后各绘制路径都读它。
    selectionRect_ = hasSelection_ ? selection : QRect();
    qInfo().noquote() << QStringLiteral("[jie2] 选区结算 %1,%2 %3x%4 → hasSelection=%5")
                             .arg(selection.x())
                             .arg(selection.y())
                             .arg(selection.width())
                             .arg(selection.height())
                             .arg(hasSelection_ ? 1 : 0);
    update();
}

void SelectionOverlay::mouseDoubleClickEvent(QMouseEvent* event) {
    // 双击确认保存。绘制中不响应（避免误提交）。
    if (event->button() == Qt::LeftButton && hasSelection_ && !drawing_ &&
        currentSelection().contains(event->pos()) &&
        toolbarHitTest(event->pos(), nullptr) == ToolbarButton::None) {
        commit(true);
    }
}

void SelectionOverlay::wheelEvent(QWheelEvent* event) {
    if (hasSelection_) {
        const int delta = event->angleDelta().y();
        if (delta != 0) {
            // 选中矩形/椭圆后 Shift+滚轮缩放（专业版对齐项 v2.11）。
            // 围绕包围盒中心等比缩放；单步即一次可撤销编辑。
            if (event->modifiers().testFlag(Qt::ShiftModifier) &&
                selectedIndex_ >= 0 &&
                static_cast<std::size_t>(selectedIndex_) <
                    annotations_.size()) {
                const std::size_t index =
                    static_cast<std::size_t>(selectedIndex_);
                const core::Annotation& item = annotations_.items()[index];
                if (item.tool == core::AnnotationTool::Rect ||
                    item.tool == core::AnnotationTool::Ellipse) {
                    const double factor = delta > 0 ? 1.1 : 1.0 / 1.1;
                    const core::Rect before = item.boundingRect();
                    const double cx = before.x + before.width / 2.0;
                    const double cy = before.y + before.height / 2.0;
                    std::vector<core::Point> points = item.points;
                    for (core::Point& point : points) {
                        point.x = static_cast<int>(
                            std::lround(cx + (point.x - cx) * factor));
                        point.y = static_cast<int>(
                            std::lround(cy + (point.y - cy) * factor));
                    }
                    annotations_.replacePoints(index, std::move(points),
                                               /*record=*/true);
                    rebuildComposed();
                    const core::Rect now =
                        annotations_.items()[index].boundingRect();
                    const QString factorLabel =
                        QString::number(factor, 'f', 2);
                    qInfo().noquote()
                        << QStringLiteral(
                               "[jie2] 缩放选中标注 ×%1（%2x%3 → %4x%5）")
                               .arg(factorLabel)
                               .arg(before.width)
                               .arg(before.height)
                               .arg(now.width)
                               .arg(now.height);
                    showToast(QStringLiteral("缩放 ×%1").arg(factorLabel));
                    update();
                    event->accept();
                    return;
                }
            }
            // 滚轮调节当前工具的粗细（与 Snipaste 的"滚轮调画笔宽度"一致）。
            currentWidth_ = qBound(1, currentWidth_ + (delta > 0 ? 1 : -1), 24);
            showToast(QStringLiteral("粗细：%1").arg(currentWidth_));
            update();
            event->accept();
            return;
        }
    }
    QDialog::wheelEvent(event);
}

bool SelectionOverlay::event(QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
            const unsigned mods = shortcutModifiers(key->modifiers());
            for (const auto& id : core::ShortcutMap::actionIds()) {
                if (shortcuts_.matches(id, key->key(), mods)) {
                    keyPressEvent(key); return true;
                }
            }
        }
    }
    return QDialog::event(event);
}

void SelectionOverlay::keyPressEvent(QKeyEvent* event) {
    const unsigned scMods = shortcutModifiers(event->modifiers());
    const int shortcutKey = event->key() == Qt::Key_Enter ? Qt::Key_Return : event->key();
    // ---- 应用内快捷键（v2.11.3：可自定义；默认 T/N/E/M/D/R/O/B/Shift+B）----
    //
    // 走 ShortcutMap：用户可用配置 `shortcut.<动作>` 重绑或禁用（"none"），
    // 修饰键要求精确匹配（按了 Ctrl+T 不会触发绑为 "t" 的动作）。
    if (shortcuts_.matches("tool.text", shortcutKey, scMods)) {
        currentTool_ = core::AnnotationTool::Text;
        toolChosen_ = true;
        showToast(QStringLiteral("工具：文字（点击位置输入）"));
        update();
        return;
    }
    if (shortcuts_.matches("tool.number", shortcutKey, scMods)) {
        currentTool_ = core::AnnotationTool::Number;
        toolChosen_ = true;
        showToast(QStringLiteral("工具：序号（点击放置，自动编号）"));
        update();
        return;
    }
    if (shortcuts_.matches("tool.eraser", shortcutKey, scMods)) {
        currentTool_ = core::AnnotationTool::Eraser;
        toolChosen_ = true;
        showToast(QStringLiteral("工具：橡皮擦（拖过即删除该标注）"));
        update();
        return;
    }
    if (shortcuts_.matches("tool.magnifier", shortcutKey, scMods)) {
        currentTool_ = core::AnnotationTool::Magnifier;
        toolChosen_ = true;
        showToast(QStringLiteral("工具：放大镜（拖出要放大的区域）"));
        update();
        return;
    }
    if (shortcuts_.matches("style.dashed", shortcutKey, scMods)) {
        dashed_ = !dashed_;
        showToast(dashed_ ? QStringLiteral("线型：虚线")
                          : QStringLiteral("线型：实线"));
        update();
        return;
    }
    if (shortcuts_.matches("style.corner", shortcutKey, scMods)) {
        cornerRadius_ = (cornerRadius_ + 8) % 32;
        showToast(cornerRadius_ > 0
                      ? QStringLiteral("圆角矩形：半径 %1").arg(cornerRadius_)
                      : QStringLiteral("圆角矩形：关（直角）"));
        update();
        return;
    }
    if (shortcuts_.matches("shape.ellipse", shortcutKey, scMods)) {
        ellipseShape_ = !ellipseShape_;
        showToast(ellipseShape_
                      ? QStringLiteral("形状：椭圆（对马克笔/马赛克/模糊/橡皮擦生效）")
                      : QStringLiteral("形状：矩形"));
        update();
        return;
    }
    if (shortcuts_.matches("arrow.both", shortcutKey, scMods)) {
        arrowBothEnds_ = !arrowBothEnds_;
        showToast(arrowBothEnds_ ? QStringLiteral("箭头：双向")
                                 : QStringLiteral("箭头：单向"));
        update();
        return;
    }
    if (shortcuts_.matches("arrow.reverse", shortcutKey, scMods)) {
        arrowReverse_ = !arrowReverse_;
        showToast(arrowReverse_ ? QStringLiteral("箭头：反向（头在起点）")
                                : QStringLiteral("箭头：正向"));
        update();
        return;
    }
    // 撤销 / 重做（Ctrl+Z / Ctrl+Shift+Z，macOS 上 Cmd 同样有效）。
    if (event->matches(QKeySequence::Undo)) {
        performUndo();
        return;
    }
    if (event->matches(QKeySequence::Redo)) {
        performRedo();
        return;
    }

    // 自由选中（专业版 v2.8）：Delete 删除选中标注；方向键微移（Shift = 10px）。
    if (selectedIndex_ >= 0 &&
        static_cast<std::size_t>(selectedIndex_) < annotations_.size()) {
        if (event->key() == Qt::Key_Delete ||
            event->key() == Qt::Key_Backspace) {
            annotations_.removeAt(static_cast<std::size_t>(selectedIndex_));
            selectedIndex_ = -1;
            rebuildComposed();
            update();
            qInfo().noquote()
                << QStringLiteral("[jie2] 删除选中标注（剩 %1 条）")
                       .arg(annotations_.size());
            showToast(QStringLiteral("已删除选中标注"));
            return;
        }
        int dx = 0;
        int dy = 0;
        switch (event->key()) {
            case Qt::Key_Left: dx = -1; break;
            case Qt::Key_Right: dx = 1; break;
            case Qt::Key_Up: dy = -1; break;
            case Qt::Key_Down: dy = 1; break;
            default: break;
        }
        if (dx != 0 || dy != 0) {
            const int step =
                event->modifiers().testFlag(Qt::ShiftModifier) ? 10 : 1;
            annotations_.translateAt(
                static_cast<std::size_t>(selectedIndex_), dx * step, dy * step);
            rebuildComposed();
            update();
            qInfo().noquote()
                << QStringLiteral("[jie2] 移动选中标注（%1,%2，步长 %3px）")
                       .arg(dx * step)
                       .arg(dy * step)
                       .arg(step);
            return;
        }
    } else if (selectedIndex_ >= 0) {
        selectedIndex_ = -1;  // 越界（例如撤销导致条目减少）：静默重置
    }
    // 序号工具：+ / - 调整下一个序号（专业版对齐项 v2.5.2：
    // "第一个数字可大于 1；+/- 调整序号"）。等号键与加减键盘都接受。
    if (currentTool_ == core::AnnotationTool::Number &&
        (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal ||
         event->key() == Qt::Key_Minus)) {
        const int delta = (event->key() == Qt::Key_Minus) ? -1 : 1;
        numberOffset_ = std::clamp(numberOffset_ + delta, -99, 99);
        showToast(QStringLiteral("下一个序号：%1").arg(nextNumberValue()));
        update();
        return;
    }
    if (event->key() == Qt::Key_C && event->modifiers().testFlag(Qt::ShiftModifier) && hasSelection_) {
        advanceToolColor();
        return;
    }
    // 数字快捷键保持原映射，不随工具条分组改变。
    if (event->key() >= Qt::Key_1 && event->key() <= Qt::Key_9) {
        using Tool = core::AnnotationTool;
        static constexpr Tool tools[] = {Tool::Rect, Tool::Ellipse, Tool::Arrow,
            Tool::Pencil, Tool::Marker, Tool::Mosaic, Tool::Blur, Tool::Eraser, Tool::Magnifier};
        currentTool_ = tools[event->key() - Qt::Key_1];
        toolChosen_ = true;
        update();
        return;
    }
    // ---- 样式与粗细的方括号键 ----
    // [ / ]：画笔粗细（v2.4 对齐项："除了鼠标滚轮和 1 2，也可以使用 [ ]"）；
    // Shift+[ / Shift+]：橡皮擦区域旋转（v2.6 对齐项："矩形/椭圆区域橡皮擦可旋转"）。
    //
    // 注意：X11 下 Shift+] 送达的键值是花括号（} = BraceRight）而不是
    // 方括号 —— 实测教训，早期只认 BracketRight 导致"旋转没反应"。
    // 这里统一把花括号归一化为方括号再处理。
    int bracketKey = event->key();
    if (bracketKey == Qt::Key_BraceLeft) {
        bracketKey = Qt::Key_BracketLeft;
    } else if (bracketKey == Qt::Key_BraceRight) {
        bracketKey = Qt::Key_BracketRight;
    }
    if (bracketKey == Qt::Key_BracketLeft ||
        bracketKey == Qt::Key_BracketRight) {
        const int delta = bracketKey == Qt::Key_BracketRight ? 1 : -1;
        if (event->modifiers().testFlag(Qt::ShiftModifier)) {
            eraserAngle_ = (eraserAngle_ + delta * 15 + 360) % 360;
            // toast 会同步写日志（"[jie2] 橡皮擦区域旋转：N°…"），测试据此断言。
            showToast(QStringLiteral("橡皮擦区域旋转：%1°（Shift+[ ] 调整）")
                          .arg(eraserAngle_));
        } else {
            currentWidth_ = qBound(1, currentWidth_ + delta, 24);
            showToast(QStringLiteral("粗细：%1").arg(currentWidth_));
        }
        update();
        return;
    }


    switch (event->key()) {
        case Qt::Key_Escape:
            // 绘制中：Esc 只取消草稿；否则取消整个截图。
            if (drawing_) {
                drawing_ = false;
                draft_ = core::Annotation{};
                showToast(QStringLiteral("已取消当前标注"));
                update();
                return;
            }
            commit(false);
            return;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (hasSelection_ && !drawing_) {
                commit(true);
            }
            return;
        case Qt::Key_C: {
            if (!hasCursor_) {
                return;
            }
            const QColor color = pixelColorAt(cursor_);
            if (!color.isValid()) {
                return;
            }
            const QString hex = color.name(QColor::HexRgb).toUpper();
            QApplication::clipboard()->setText(hex);
            toast_ = QStringLiteral("已复制色值 %1（继续截图会覆盖剪贴板）").arg(hex);
            QTimer::singleShot(1800, this, [this]() {
                toast_.clear();
                update();
            });
            update();
            return;
        }
        case Qt::Key_Comma:   // , 更旧
        case Qt::Key_Period: {  // . 更新
            // 回放序列：-1=当前抓屏画面，0..n-1=历史（旧的在前）。
            // "更旧"从当前屏进入最旧的一张？不——进入最新的一张（n-1），
            // 再按继续向旧走；"更新"向新走，越过最新回到当前屏。
            if (historyPaths_.isEmpty()) {
                showToast(QStringLiteral("暂无截图历史"));
                return;
            }
            const int last = static_cast<int>(historyPaths_.size()) - 1;
            int next = historyIndex_;
            if (event->key() == Qt::Key_Comma) {
                if (next < 0) {
                    next = last;          // 当前屏 → 最新历史
                } else if (next > 0) {
                    --next;               // 向旧
                } else {
                    next = 0;             // 已在最旧，保持不变
                }
            } else {
                if (next < 0) {
                    return;               // 已在当前屏
                }
                if (next < last) {
                    ++next;               // 向新
                } else {
                    next = -1;            // 越过最新 → 回到当前屏
                }
            }
            if (next != historyIndex_) {
                switchHistory(next);
            }
            return;
        }
        case Qt::Key_F5:
            recapture();
            return;
        default:
            break;
    }
    QDialog::keyPressEvent(event);
}

void SelectionOverlay::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    // 窗口句柄此刻才确定：再次关联目标屏并铺满，避免落到主屏或残留尺寸。
    if (screen_ != nullptr) {
        if (windowHandle() != nullptr) {
            windowHandle()->setScreen(screen_);
        }
        setGeometry(screen_->geometry());
    }
    raise();
    activateWindow();
    setFocus();
    // 层级提升：Qt 的 StaysOnTopHint 只到普通浮动层，盖不住全屏应用与
    // 远程桌面客户端窗口（后者常处在独立 Space / 独占全屏）。
    // 不提升时用户看到的现象就是"按了热键，非要先切一下窗口才出现遮罩"。
    promoteToFront();
    // 某些平台首次显示时焦点建立较晚，下一轮事件循环再抢一次。
    QTimer::singleShot(0, this, [this]() {
        if (screen_ != nullptr) {
            setGeometry(screen_->geometry());
        }
        raise();
        activateWindow();
        setFocus();
        promoteToFront();
    });
}

void SelectionOverlay::promoteToFront() {
    if (!promoteWindow_) {
        return;
    }
    // winId() 此刻才有效：QWidget::winId() 会强制创建原生窗口。
    const WId handle = winId();
    if (handle == 0) {
        return;
    }
    promoteWindow_(reinterpret_cast<void*>(handle));
}

} // namespace jie2::ui
