#include "ui/clothesline.h"

#include <algorithm>
#include <cmath>

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDebug>
#include <QDrag>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QTimer>
#include <QUrl>

namespace jie2::ui {

namespace {

/// 由路径哈希推出固定倾斜角（度，[-2.5, 2.5] —— tendedero 的取值范围）。
/// tendedero 是创建时随机一次；用哈希让同一文件每次挂载角度一致，
/// 重绘与测试都确定。
double tiltForPath(const std::string& path) {
    quint32 hash = 0;
    for (const char c : path) {
        hash = hash * 31 + static_cast<quint8>(c);
    }
    return static_cast<double>(hash % 501) / 100.0 - 2.5;
}

/// 绳子端头淡出：0.08..0.92 区间内完整，两端线性降到透明
/// （tendedero 的 mask："仿佛从屏幕外延伸过来"）。
QGradientStops ropeFade(const QColor& color) {
    QColor transparent = color;
    transparent.setAlpha(0);
    return {{0.0, transparent}, {0.08, color}, {0.92, color}, {1.0, transparent}};
}

} // namespace

Clothesline::Clothesline(QWidget* parent) : QWidget(parent) {
    setWindowTitle(QStringLiteral("截2 衣架"));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint |
                   Qt::WindowStaysOnTopHint | Qt::Tool |
                   Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    // 透明面板（tendedero 形态）：只有绳子、衣夹和卡片可见，
    // 桌面壁纸从面板其余区域透出来。
    setAttribute(Qt::WA_TranslucentBackground, true);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);

    setDarkTheme(true);  // 默认深色（与既有观感一致）

    pollTimer_ = new QTimer(this);
    connect(pollTimer_, &QTimer::timeout, this, &Clothesline::tick);
    pollTimer_->start(120);

    // 滑入滑出动画：用固定步长的位移代替 QPropertyAnimation ——
    // 窗口位置动画在部分合成器上被忽略（Wayland 不允许应用定位窗口），
    // 而这里动画的其实是"窗口内的可视条带"，用定时器驱动最可控。
    //
    // 注：Wayland 下本窗口根本不创建（改走 jie2-line 子进程的 layer-shell
    // 顶部条，见 tray.cpp 的 waylandLineEnabled_ 分支），因此这里只有
    // "窗口位移"一种形态。
    animTimer_ = new QTimer(this);
    connect(animTimer_, &QTimer::timeout, this, [this]() {
        const int current = y();
        const int delta = animTargetY_ - current;
        if (std::abs(delta) <= kAnimPxPerStep) {
            move(x(), animTargetY_);
            animTimer_->stop();
            animating_ = false;
            if (animHideAtEnd_) {
                hide();
            }
            return;
        }
        const int step = delta > 0 ? kAnimPxPerStep : -kAnimPxPerStep;
        move(x(), current + step);
    });

    conceal();  // 初始收起
}

void Clothesline::setDarkTheme(bool dark) {
    darkTheme_ = dark;
    if (dark) {
        colors_ = StripColors{QColor(30, 30, 36, 150), QColor(40, 40, 46, 150),
                              QColor(225, 225, 232), QColor(170, 170, 178)};
    } else {
        colors_ = StripColors{QColor(245, 246, 251, 165), QColor(250, 250, 253, 165),
                              QColor(52, 54, 62), QColor(96, 98, 108)};
    }
    update();
}

void Clothesline::setEntries(std::vector<core::HistoryEntry> entries) {
    entries_.clear();
    // 只保留最近 kMaxItems 条；显示顺序：新的在右。
    const int start = std::max(0, static_cast<int>(entries.size()) - kMaxItems);
    for (int i = start; i < static_cast<int>(entries.size()); ++i) {
        Item item;
        item.entry = entries[static_cast<std::size_t>(i)];
        item.tilt = tiltForPath(item.entry.path);
        entries_.push_back(std::move(item));
    }
    // 容器被重建：所有基于旧索引的交互状态必须失效。
    // （若不清，hoverIndex_ 可能指向已不存在的项，paintEvent 会越界。）
    hoverIndex_ = -1;
    pressedOnCard_ = false;
    pressedIndex_ = -1;
    reloadThumbnails();
    qInfo().noquote() << QStringLiteral("[jie2] 衣架：%1 项").arg(entries_.size());
    update();
}

void Clothesline::reloadThumbnails() {
    for (Item& item : entries_) {
        if (item.thumbnail.isNull() && !item.entry.path.empty()) {
            QImage image(QString::fromStdString(item.entry.path));
            if (!image.isNull()) {
                // 控制内存：缩略图不超过照片区尺寸的 2 倍。
                item.thumbnail =
                    image.scaled(kPhotoMaxWidth * 2, kPhotoMaxHeight * 2,
                                 Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
        }
    }
}

void Clothesline::setWatchEnabled(bool enabled) {
    watchEnabled_ = enabled;
    if (!enabled) {
        conceal();
    }
}

// ---- 布局（tendedero LineView.Layout 的移植）----

double Clothesline::ropeSag() const {
    return std::min(30.0, width() * 0.018);
}

double Clothesline::ropeYAt(double x) const {
    if (width() <= 0) {
        return kRopeTop;
    }
    const double f = x / width();
    return kRopeTop + 4.0 * ropeSag() * f * (1.0 - f);
}

double Clothesline::cardCenterX(int index) const {
    // 卡片居中排布；张数超出可用宽度时间距压缩，保证两端不出屏
    // （tendedero 的 "move closer together so none is past the edge"）。
    const int count = static_cast<int>(entries_.size());
    const int gaps = std::max(count - 1, 0);
    const double step =
        gaps > 0 ? std::min(static_cast<double>(kCardSpacing),
                            std::max(0, width() - 200) / static_cast<double>(gaps))
                 : kCardSpacing;
    return width() / 2.0 - gaps * step / 2.0 + index * step;
}

QSizeF Clothesline::photoSize(int index) const {
    // 照片等比适配进 136×104，玻璃边框贴着照片（tendedero：
    // "the white border hugs it whether the screenshot is wide or tall"）。
    const Item& item = entries_[static_cast<std::size_t>(index)];
    const QSizeF source = item.thumbnail.isNull()
                              ? QSizeF(kPhotoMaxWidth, kPhotoMaxHeight)
                              : QSizeF(item.thumbnail.size());
    return source.scaled(QSizeF(kPhotoMaxWidth, kPhotoMaxHeight),
                         Qt::KeepAspectRatio);
}

QPointF Clothesline::cardAnchor(int index) const {
    // 悬挂点 = 衣夹顶中 = 绳子上方 kPinAbove 处。倾斜旋转以此点为锚
    // （tendedero：rotationEffect(anchor: .top)）。
    const double cx = cardCenterX(index);
    return QPointF(cx, ropeYAt(cx) - kPinAbove);
}

QRectF Clothesline::cardRectF(int index) const {
    const double cx = cardCenterX(index);
    const double top = ropeYAt(cx) - kPinAbove + kCardBelowClipTop;
    const QSizeF photo = photoSize(index);
    return QRectF(cx - (photo.width() + kFrameInset * 2) / 2.0, top,
                  photo.width() + kFrameInset * 2,
                  photo.height() + kFrameInset * 2);
}

int Clothesline::itemAt(const QPoint& pos) const {
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        const Item& item = entries_[static_cast<std::size_t>(i)];
        const QPointF anchor = cardAnchor(i);
        // 逆旋转回正后再做矩形包含判定（卡片是斜挂着的）。
        QTransform transform;
        transform.translate(anchor.x(), anchor.y());
        transform.rotate(item.tilt);
        transform.translate(-anchor.x(), -anchor.y());
        if (cardRectF(i).contains(transform.inverted().map(QPointF(pos)))) {
            return i;
        }
    }
    return -1;
}

void Clothesline::tick() {
    if (!watchEnabled_) {
        return;
    }

    const QPoint pos = QCursor::pos();
    QScreen* screen = QGuiApplication::screenAt(pos);
    if (screen == nullptr) {
        return;
    }
    const QRect area = screen->geometry();

    // 展开条件：指针触到屏幕顶边（顶边热区）。
    const bool atTopEdge = pos.y() <= area.top() + kEdgeZone &&
                           pos.x() >= area.left() && pos.x() <= area.right();

    if (!revealed_) {
        // 收起动画进行中不重新展开：否则指针停在顶边时，
        // "滑出"与"滑入"会互相打架，表现为条带抖动。
        if (animating_) {
            return;
        }
        if (atTopEdge) {
            reveal(area);  // 按指针所在屏定位并做滑入动画
        }
        return;
    }

    // 已展开：指针离开"保持展开区"→ 收起。
    //
    // 注意不能用 stripGeometry.contains(pos) 做判定：在 macOS 上窗口会被
    // 窗口服务器下移（菜单栏之下），而光标还在屏幕顶边（y=1~3），
    // 落在窗口上方 → 会判定"离开"而收起，下个 tick 又展开（实测抖动）。
    // 因此保持区的上边界取"屏幕顶边"而不是窗口自身，下边界取窗口实际下沿。
    const QRect stripGlobal =
        revealed_ ? frameGeometry() : geometry();
    const QRect keepOpen(stripGlobal.left() - 8, area.top() - 8,
                         stripGlobal.width() + 16,
                         stripGlobal.bottom() - area.top() + 16);
    if (!keepOpen.contains(pos)) {
        conceal();
    }
}

QRect Clothesline::panelScreenArea() const {
    // 用当前所在屏幕；未定位时退回主屏。
    QScreen* screen = QGuiApplication::screenAt(QPoint(x() + width() / 2, 1));
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    return screen != nullptr ? screen->geometry() : QRect(0, 0, 1280, 800);
}

void Clothesline::animateTo(int targetY, bool hideAtEnd) {
    animTargetY_ = targetY;
    animHideAtEnd_ = hideAtEnd;
    if (!animating_) {
        animating_ = true;
        animTimer_->start(kAnimStepMs);
    }
}

void Clothesline::reveal(const QRect& area) {
    if (revealed_) {
        return;
    }
    revealed_ = true;
    // 起始位置在屏幕上方（不可见），动画滑入到 area.top()。
    setGeometry(area.left(), area.top() - kPanelHeight, area.width(), kPanelHeight);
    show();
    raise();
    animateTo(area.top(), /*hideAtEnd=*/false);
    qInfo().noquote() << QStringLiteral("[jie2] 衣架展开（%1 项）")
                             .arg(entries_.size());
    update();
}

void Clothesline::conceal() {
    hoverIndex_ = -1;
    if (!revealed_) {
        // 仍未展开时保持隐藏；同时确保不在屏幕上留下痕迹。
        animTimer_->stop();
        animating_ = false;
        hide();
        return;
    }
    revealed_ = false;
    // 滑出动画结束时才 hide()（否则动画看不见）。
    animateTo(panelScreenArea().top() - kPanelHeight, /*hideAtEnd=*/true);
    qInfo().noquote() << QStringLiteral("[jie2] 衣架收起");
}

void Clothesline::hideNow() {
    // 用于"截图前立刻消失"：不等动画，直接隐藏。
    revealed_ = false;
    hoverIndex_ = -1;
    animTimer_->stop();
    animating_ = false;
    hide();
}

// ---- 绘制（tendedero 样式：抛物线绳 + 铝质衣夹 + 玻璃卡片 + 倾斜）----

void Clothesline::drawRope(QPainter& painter) const {
    // 抛物线：两端在 ropeTop，中心下垂 sag。二次贝塞尔的控制点在
    // (中点, ropeTop + 2·sag) 时曲线恰好是这条抛物线。
    QPainterPath path;
    path.moveTo(-20, kRopeTop);
    path.quadTo(QPointF(width() / 2.0, kRopeTop + 2.0 * ropeSag()),
                QPointF(width() + 20, kRopeTop));

    // 三层描边（tendedero Rope）：柔和投影 + 中性灰芯 + 顶部高光，
    // 深浅壁纸上都可读。两端淡出用画笔画刷上的水平渐变实现。
    const auto stroke = [&](const QColor& color, double width_, double dy) {
        QLinearGradient fade(0, 0, width(), 0);
        fade.setStops(ropeFade(color));
        painter.save();
        painter.translate(0, dy);
        painter.setPen(QPen(QBrush(fade), width_));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
        painter.restore();
    };
    stroke(QColor(0, 0, 0, 56), 1.4, 1.2);       // 投影（黑色 22%）
    stroke(QColor(140, 140, 140), 1.2, 0);       // 灰芯（white 0.55）
    stroke(QColor(255, 255, 255, 115), 0.4, -0.35);  // 高光（白色 45%）
}

void Clothesline::drawClothespin(QPainter& painter,
                                 const QPointF& topCenter) const {
    // 铝质衣夹（tendedero Clothespin）：9×26 拉丝金属胶囊，
    // 绳子从 8.5pt 处的槽里穿过。
    const QRectF body(topCenter.x() - kClipWidth / 2, topCenter.y(),
                    kClipWidth, kClipHeight);

    // 柔和投影（黑色 30%，半径 2，下移 1.5 —— 无模糊API，用偏移近似）
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 50));
    painter.drawRoundedRect(body.translated(0, 1.5), 3.5, 3.5);

    // 拉丝金属：水平渐变（亮斑偏左）
    QLinearGradient metal(body.left(), 0, body.right(), 0);
    metal.setColorAt(0.0, QColor(179, 179, 179));
    metal.setColorAt(0.35, QColor(237, 237, 237));
    metal.setColorAt(0.65, QColor(209, 209, 209));
    metal.setColorAt(1.0, QColor(158, 158, 158));
    painter.setBrush(metal);
    painter.drawRoundedRect(body, 3.5, 3.5);

    // 描边：顶部受光（白 90% → 黑 18% 纵向渐变）
    QLinearGradient edge(0, body.top(), 0, body.bottom());
    edge.setColorAt(0.0, QColor(255, 255, 255, 230));
    edge.setColorAt(1.0, QColor(0, 0, 0, 46));
    painter.setPen(QPen(QBrush(edge), 0.6));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(body, 3.5, 3.5);

    // 绳槽
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 82));
    painter.drawRoundedRect(
        QRectF(topCenter.x() - 2.5, topCenter.y() + 8.5, 5, 1.4), 0.7, 0.7);
}

void Clothesline::drawCardBody(QPainter& painter, int index) const {
    const Item& item = entries_[static_cast<std::size_t>(index)];
    const QRectF card = cardRectF(index);
    const QRectF photo = card.adjusted(kFrameInset, kFrameInset,
                                       -kFrameInset, -kFrameInset);
    const bool hovered = (index == hoverIndex_);

    // 投影（无模糊 API，用三层渐淡的偏移圆角矩形近似柔和阴影；
    // 参数对齐 tendedero：黑 18%、半径 10、下移 5；悬停 26%/14/8）
    const double shadowY = hovered ? 8.0 : 5.0;
    const int shadowAlpha = hovered ? 66 : 46;
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, shadowAlpha / 3));
    painter.drawRoundedRect(card.adjusted(-4, -4, 4, 4).translated(0, shadowY + 2),
                            kFrameRadius + 4, kFrameRadius + 4);
    painter.setBrush(QColor(0, 0, 0, shadowAlpha / 2));
    painter.drawRoundedRect(card.adjusted(-2, -2, 2, 2).translated(0, shadowY + 1),
                            kFrameRadius + 2, kFrameRadius + 2);
    painter.setBrush(QColor(0, 0, 0, shadowAlpha));
    painter.drawRoundedRect(card.translated(0, shadowY), kFrameRadius,
                            kFrameRadius);

    // 玻璃底（ultraThinMaterial 的近似：半透明主题色）
    painter.setBrush(colors_.cardGlass);
    painter.drawRoundedRect(card, kFrameRadius, kFrameRadius);

    // 玻璃描边：顶部受光的纵向渐变（白 55% → 白 12%）
    QLinearGradient specular(0, card.top(), 0, card.bottom());
    specular.setColorAt(0.0, QColor(255, 255, 255, 140));
    specular.setColorAt(1.0, QColor(255, 255, 255, 31));
    painter.setPen(QPen(QBrush(specular), 0.75));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(card, kFrameRadius, kFrameRadius);
    // 外侧细暗边（黑 10%）
    painter.setPen(QPen(QColor(0, 0, 0, 26), 0.5));
    painter.drawRoundedRect(card.adjusted(-0.5, -0.5, 0.5, 0.5),
                            kFrameRadius + 0.5, kFrameRadius + 0.5);

    // 照片（同心圆角：半径 = 边框半径 - 边框厚度）
    painter.setPen(Qt::NoPen);
    if (!item.thumbnail.isNull()) {
        painter.save();
        QPainterPath clip;
        clip.addRoundedRect(photo, kPhotoRadius, kPhotoRadius);
        painter.setClipPath(clip);
        painter.drawImage(photo, item.thumbnail);
        painter.restore();
        // 照片内缘描边（白 18%）
        painter.setPen(QPen(QColor(255, 255, 255, 46), 0.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(photo, kPhotoRadius, kPhotoRadius);
    } else {
        painter.setPen(colors_.failText);
        painter.drawText(photo, Qt::AlignCenter, QStringLiteral("读取失败"));
    }
}

void Clothesline::drawEmptyHint(QPainter& painter) const {
    // 空态提示：绳子中点下方的胶囊气泡（tendedero 的 Hint）。
    const QString text =
        QStringLiteral("截一张图，它会立刻挂在这里");
    painter.setFont(QFont(QString(), 12));
    const QRectF textBounds = painter.fontMetrics().boundingRect(text);
    const QRectF capsule(0, 0, textBounds.width() + 24, textBounds.height() + 12);
    const double cx = width() / 2.0;
    const QRectF placed(cx - capsule.width() / 2,
                        ropeYAt(cx) + 34 - capsule.height() / 2,
                        capsule.width(), capsule.height());
    painter.setPen(Qt::NoPen);
    painter.setBrush(colors_.hintBg);
    painter.drawRoundedRect(placed, placed.height() / 2, placed.height() / 2);
    painter.setPen(colors_.hintText);
    painter.drawText(placed, Qt::AlignCenter, text);
}

void Clothesline::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // 无底板：面板透明，只画绳子、衣夹与卡片（tendedero 形态）。
    drawRope(painter);

    if (entries_.empty()) {
        drawEmptyHint(painter);
        return;
    }

    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        const Item& item = entries_[static_cast<std::size_t>(i)];
        const QPointF anchor = cardAnchor(i);

        painter.save();
        // 整张悬挂单元（衣夹 + 卡片）绕悬挂点倾斜。
        painter.translate(anchor);
        painter.rotate(item.tilt);
        painter.translate(-anchor);

        drawClothespin(painter, anchor);

        // 悬停放大 / 按压缩小以卡顶中心为锚（tendedero 的
        // scaleEffect(anchor: .top) 只作用于卡片，不含衣夹）。
        const bool pressed = pressedOnCard_ && pressedIndex_ == i;
        const double scale =
            pressed ? 0.95 : (i == hoverIndex_ ? 1.035 : 1.0);
        painter.save();
        const QRectF card = cardRectF(i);
        const QPointF cardTop(card.center().x(), card.top());
        painter.translate(cardTop);
        painter.scale(scale, scale);
        painter.translate(-cardTop);
        drawCardBody(painter, i);
        painter.restore();

        painter.restore();
    }
}

void Clothesline::mousePressEvent(QMouseEvent* event) {
    const int index = itemAt(event->pos());
    if (event->button() == Qt::RightButton) {
        if (index >= 0) {
            const QString path =
                QString::fromStdString(entries_[static_cast<std::size_t>(index)].entry.path);
            qInfo().noquote() << QStringLiteral("[jie2] 衣架：取下 %1").arg(path);
            entries_.erase(entries_.begin() + index);
            update();
            emit entryDropped(path);
        } else {
            conceal();
        }
        return;
    }
    if (event->button() == Qt::LeftButton && index >= 0) {
        // 先记录：等 mouseMove 判断这是"点击"还是"拖出"。
        pressedOnCard_ = true;
        pressPos_ = event->pos();
        pressedIndex_ = index;
        update();  // 按压缩小 0.95（tendedero 的长按蓄力感）
        return;
    }
    conceal();
}

bool Clothesline::startDragOut(int index) {
    if (index < 0 || index >= static_cast<int>(entries_.size())) {
        return false;
    }
    // 提前拷贝所需数据：drag.exec 会跑一个嵌套事件循环，
    // 期间可能发生 setEntries（外部触发）使 entries_ 重建 ——
    // 之后不能再触碰任何指向旧容器的引用。
    const QString path =
        QString::fromStdString(entries_[static_cast<std::size_t>(index)].entry.path);
    const QImage thumbnail = entries_[static_cast<std::size_t>(index)].thumbnail;
    if (!QFileInfo::exists(path)) {
        qWarning().noquote()
            << QStringLiteral("[jie2] 衣架：拖出失败，文件不存在 %1").arg(path);
        return false;
    }

    // 拖拽载荷用文件 URL：文件管理器接住即"保存到该文件夹"
    // （tendedero 的 "Drag into a folder → Keep it there"）。
    auto* mime = new QMimeData();
    mime->setUrls({QUrl::fromLocalFile(path)});
    // 同时带图像数据：可拖进聊天窗口 / 图像编辑器等以图像接收的程序。
    if (!thumbnail.isNull()) {
        mime->setImageData(thumbnail);
    }

    QDrag drag(this);
    drag.setMimeData(mime);
    if (!thumbnail.isNull()) {
        drag.setPixmap(QPixmap::fromImage(thumbnail)
                           .scaled(120, 84, Qt::KeepAspectRatio,
                                   Qt::SmoothTransformation));
    }
    qInfo().noquote() << QStringLiteral("[jie2] 衣架：拖出 %1").arg(path);
    const Qt::DropAction action = drag.exec(Qt::CopyAction | Qt::MoveAction,
                                            Qt::CopyAction);

    // 语义（对齐 tendedero）：
    //   - MoveAction：对方把文件拿走（移动）→ 卡片离开衣架；
    //   - CopyAction：对方只取了一份拷贝 → 卡片留在衣架；
    //   - IgnoreAction：拖到无效区域 → 什么都不做。
    if (action == Qt::MoveAction) {
        qInfo().noquote() << QStringLiteral("[jie2] 衣架：%1 已被移走，卡片下线")
                                 .arg(path);
        // 嵌套事件循环后索引可能已失效：按路径定位再删。
        const auto it = std::find_if(
            entries_.begin(), entries_.end(),
            [&path](const Item& item) {
                return QString::fromStdString(item.entry.path) == path;
            });
        if (it != entries_.end()) {
            entries_.erase(it);
            update();
        }
        emit entryDropped(path);  // 与右键取下同一条链路（文件已在别处，不删）
        return true;
    }
    if (action == Qt::CopyAction) {
        qInfo().noquote() << QStringLiteral("[jie2] 衣架：%1 已复制给目标，卡片保留")
                                 .arg(path);
    }
    return false;
}

void Clothesline::mouseMoveEvent(QMouseEvent* event) {
    // 按住卡片并移动超过阈值 → 启动拖出（tendedero 的拖拽手势）。
    if (pressedOnCard_ &&
        (event->pos() - pressPos_).manhattanLength() >= kDragStartDistance) {
        const int index = pressedIndex_;
        pressedOnCard_ = false;
        pressedIndex_ = -1;
        update();  // 撤销按压缩放
        startDragOut(index);
        return;
    }

    const int index = itemAt(event->pos());
    if (index != hoverIndex_) {
        hoverIndex_ = index;
        update();
    }
}

void Clothesline::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    // 没有触发拖出 → 这是一次点击：请求上层复制到剪贴板。
    if (!pressedOnCard_) {
        return;
    }
    const int index = pressedIndex_;
    pressedOnCard_ = false;
    pressedIndex_ = -1;
    update();  // 撤销按压缩放
    if (index < 0 || index >= static_cast<int>(entries_.size())) {
        return;
    }
    // 只发信号，不自己写剪贴板（原因见 copyRequested 的注释）。
    emit copyRequested(
        QString::fromStdString(entries_[static_cast<std::size_t>(index)].entry.path));
}

void Clothesline::leaveEvent(QEvent*) {
    if (hoverIndex_ != -1) {
        hoverIndex_ = -1;
        update();
    }
    // 指针离开时若还"按着卡片"，说明这次按压没有演变成拖出
    // （拖出会由 QDrag 接管事件循环）。清掉状态，避免下次移动误触发。
    pressedOnCard_ = false;
    pressedIndex_ = -1;
}

void Clothesline::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        conceal();
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace jie2::ui
