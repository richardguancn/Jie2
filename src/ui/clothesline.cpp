#include "ui/clothesline.h"

#include <algorithm>

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDebug>
#include <QDrag>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QTimer>
#include <QUrl>

namespace jie2::ui {

namespace {

const QColor kStripBg(26, 26, 30, 242);
const QColor kRopeColor(150, 150, 160);
const QColor kCardBg(40, 40, 46);
const QColor kCardHover(58, 58, 66);
const QColor kAccent(64, 158, 255);

} // namespace

Clothesline::Clothesline(QWidget* parent) : QWidget(parent) {
    setWindowTitle(QStringLiteral("截2 衣架"));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint |
                   Qt::WindowStaysOnTopHint | Qt::Tool |
                   Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);

    pollTimer_ = new QTimer(this);
    connect(pollTimer_, &QTimer::timeout, this, &Clothesline::tick);
    pollTimer_->start(120);

    // 滑入滑出动画：用固定步长的位移代替 QPropertyAnimation ——
    // 窗口位置动画在部分合成器上被忽略（Wayland 不允许应用定位窗口），
    // 而这里动画的其实是"窗口内的可视条带"，用定时器驱动最可控。
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

void Clothesline::setEntries(std::vector<core::HistoryEntry> entries) {
    entries_.clear();
    // 只保留最近 kMaxItems 条；显示顺序：新的在右。
    const int start = std::max(0, static_cast<int>(entries.size()) - kMaxItems);
    for (int i = start; i < static_cast<int>(entries.size()); ++i) {
        Item item;
        item.entry = entries[static_cast<std::size_t>(i)];
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
                // 控制内存：缩略图不超过卡片尺寸的 2 倍。
                item.thumbnail =
                    image.scaled(kCardWidth * 2, kCardHeight * 2,
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

QRect Clothesline::ropeRect() const {
    // 绳子在条带顶部往下 14pt 处。
    return QRect(0, 14, width(), 2);
}

QRect Clothesline::itemRect(int index) const {
    // 卡片从左侧排起（新的在右）。
    const int x = 16 + index * (kCardWidth + kCardGap);
    const int y = 22;
    return QRect(x, y, kCardWidth, kCardHeight);
}

int Clothesline::itemAt(const QPoint& pos) const {
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        if (itemRect(i).contains(pos)) {
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
    setGeometry(area.left(), area.top() - kStripHeight, area.width(), kStripHeight);
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
    animateTo(panelScreenArea().top() - kStripHeight, /*hideAtEnd=*/true);
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

void Clothesline::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // 底板
    painter.setPen(Qt::NoPen);
    painter.setBrush(kStripBg);
    painter.drawRect(rect());

    // 晾衣绳
    painter.setPen(QPen(kRopeColor, 2));
    painter.drawLine(ropeRect().topLeft(), ropeRect().topRight());

    if (entries_.empty()) {
        painter.setPen(QColor(200, 200, 205));
        painter.setFont(QFont(QString(), 12));
        painter.drawText(rect().adjusted(0, 40, 0, 0), Qt::AlignHCenter | Qt::AlignTop,
                         QStringLiteral("衣架还空着 —— 截一张图，它会立刻挂在这里"));
        return;
    }

    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        const QRect card = itemRect(i);

        // 挂线（从绳子到卡片）
        painter.setPen(QPen(kRopeColor, 1));
        painter.drawLine(QPoint(card.center().x(), ropeRect().bottom()),
                         QPoint(card.center().x(), card.top()));

        // 卡片底
        painter.setPen(Qt::NoPen);
        painter.setBrush(i == hoverIndex_ ? kCardHover : kCardBg);
        painter.drawRoundedRect(card, 8, 8);

        // 缩略图（等比适配，居中）
        Item& item = entries_[static_cast<std::size_t>(i)];
        if (!item.thumbnail.isNull()) {
            const QSizeF scaled = QSizeF(item.thumbnail.size())
                                      .scaled(QSizeF(card.width() - 12,
                                                     card.height() - 12),
                                              Qt::KeepAspectRatio);
            const QRectF target(card.center().x() - scaled.width() / 2,
                                card.center().y() - scaled.height() / 2,
                                scaled.width(), scaled.height());
            painter.setClipPath([&] {
                QPainterPath path;
                path.addRoundedRect(target, 4, 4);
                return path;
            }());
            painter.drawImage(target, item.thumbnail);
            painter.setClipping(false);
        } else {
            painter.setPen(QColor(150, 150, 155));
            painter.drawText(card, Qt::AlignCenter, QStringLiteral("读取失败"));
        }

        // 悬停时画描边
        if (i == hoverIndex_) {
            painter.setPen(QPen(kAccent, 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(card.adjusted(-1, -1, 1, 1), 9, 9);
        }
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
