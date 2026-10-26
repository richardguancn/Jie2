#include "ui/qt_compat.h"
#include "ui/system_menu.h"
#include "ui/clothesline.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <QCache>

#include "core/image_ops.h"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDebug>
#include <QDrag>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMimeData>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QRandomGenerator>
#include <QScreen>
#include <QTimer>
#include <QUrl>

#include "ui/flight_overlay.h"

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

const QImage& cardShadow(const QSize& size, bool hovered) {
    static QCache<QString, QImage> cache(64);
    const QString key = QStringLiteral("%1x%2:%3").arg(size.width()).arg(size.height()).arg(hovered);
    if (auto* existing = cache.object(key)) return *existing;
    QImage mask(size + QSize(40, 40), QImage::Format_RGBA8888);
    mask.fill(Qt::transparent);
    { QPainter p(&mask); p.setRenderHint(QPainter::Antialiasing);
      p.setPen(Qt::NoPen); p.setBrush(QColor(0, 0, 0, hovered ? 66 : 46));
      p.drawRoundedRect(QRectF(QPointF(20, 20), QSizeF(size)), 16, 16); }
    core::ImageBuffer source(mask.width(), mask.height());
    for (int y = 0; y < mask.height(); ++y) std::memcpy(source.scanline(y), mask.constScanLine(y), source.stride());
    const auto blurred = core::boxBlur(source, {0, 0, mask.width(), mask.height()}, hovered ? 10 : 7);
    auto* result = new QImage(blurred.data(), blurred.width(), blurred.height(),
                              int(blurred.stride()), QImage::Format_RGBA8888);
    *result = result->copy();
    cache.insert(key, result);
    return *result;
}

} // namespace

Clothesline::Clothesline(QWidget* parent) : QWidget(parent) {
    setWindowTitle(QStringLiteral("图片暂存"));
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

    // 帧驱动：面板滑动与卡片动效（掉落/摆动/淡入）共用一个定时器。
    // 用 QElapsedTimer 授时而不用"每帧固定加 16ms"：定时器触发有抖动，
    // 按真实流逝时间推进弹簧才能保证阻尼行为稳定。
    frameClock_.start();
    frameTimer_ = new QTimer(this);
    connect(frameTimer_, &QTimer::timeout, this, &Clothesline::advanceFrame);

    // 风吹：单次定时器，每次触发后重新随机一个间隔
    // （tendedero 的阵风不是严格周期的）。
    gustTimer_ = new QTimer(this);
    gustTimer_->setSingleShot(true);
    connect(gustTimer_, &QTimer::timeout, this, &Clothesline::gustTick);

    holdTimer_ = new QTimer(this);
    holdTimer_->setSingleShot(true);
    holdTimer_->setInterval(450);
    connect(holdTimer_, &QTimer::timeout, this, [this] {
        if (!pressedOnCard_ || pressedIndex_ < 0 ||
            pressedIndex_ >= static_cast<int>(entries_.size())) return;
        const QString path = QString::fromStdString(entries_[pressedIndex_].entry.path);
        pressedOnCard_ = false;
        pressedIndex_ = -1;
        update();
        emit annotateRequested(path);
    });
    conceal();  // 初始收起
}

void Clothesline::setDarkTheme(bool dark) {
    darkTheme_ = dark;
    if (dark) {
        colors_ = StripColors{QColor(234, 242, 250, 155), QColor(40, 40, 46, 150),
                              QColor(225, 225, 232), QColor(170, 170, 178)};
    } else {
        colors_ = StripColors{QColor(243, 248, 255, 185), QColor(250, 250, 253, 165),
                              QColor(52, 54, 62), QColor(96, 98, 108)};
    }
    update();
}

void Clothesline::showCopied(const QString& path) {
    if (indexOfPath(path) < 0) return;
    copiedPath_ = path;
    copiedUntilMs_ = frameClock_.elapsed() + 1600;
    peekUntilMs_ = qMax(peekUntilMs_, copiedUntilMs_);
    ensureFrameTimer();
    update();
}

void Clothesline::setEntries(std::vector<core::HistoryEntry> entries) {
    std::vector<Item> fresh;
    // 只保留最近 kMaxItems 条；显示顺序：新的在右。
    const int start =
        std::max(0, static_cast<int>(entries.size()) - kMaxItems);
    bool hasNew = false;
    for (int i = start; i < static_cast<int>(entries.size()); ++i) {
        Item item;
        item.entry = entries[static_cast<std::size_t>(i)];
        item.tilt = tiltForPath(item.entry.path);
        // 首次见到的路径 = 新挂上的。首次调用（启动载入历史）不算"新"，
        // 否则一启动整排旧截图都会播放掉落并触发探出。
        const bool isNew = primed_ &&
                           knownPaths_.find(item.entry.path) == knownPaths_.end();
        if (isNew) {
            hasNew = true;
            if (revealed_) {
                // 面板开着：新卡片当场掉落（tendedero：面板展开时
                // 新截图直接落位）。
                item.arriving = true;
                item.arriveOffset = core::Spring::fromResponse(
                    0.42, 0.72, -kArriveDrop, 0);
                item.arriveSwing =
                    core::Spring::fromStiffness(46, 2.6, kArriveTilt, 0);
            } else {
                item.pendingArrive = true;
            }
        }
        knownPaths_.insert(item.entry.path);
        fresh.push_back(std::move(item));
    }
    primed_ = true;
    entries_ = std::move(fresh);
    if (!copiedPath_.isEmpty() && indexOfPath(copiedPath_) < 0) {
        copiedPath_.clear();
        copiedUntilMs_ = -1;
    }
    // 容器被重建：所有基于旧索引的交互状态必须失效。
    // （若不清，hoverIndex_ 可能指向已不存在的项，paintEvent 会越界。）
    hoverIndex_ = -1;
    holdTimer_->stop();
    pressedOnCard_ = false;
    pressedIndex_ = -1;
    // 防无界增长：只保留当前在线的路径。
    if (knownPaths_.size() > 500) {
        knownPaths_.clear();
        for (const Item& item : entries_) {
            knownPaths_.insert(item.entry.path);
        }
    }
    reloadThumbnails();
    if (hasNew) {
        if (revealed_) {
            ensureFrameTimer();
        } else if (watchEnabled_) {
            // tendedero：Something new hanging shows itself for a moment —
            // 有新截图挂上时自动探出亮相片刻。
            QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
            peek(screen != nullptr ? screen->geometry()
                                   : panelScreenArea());
        }
    }
    // flyCapture 先于 setEntries 到达（防御性顺序）：现在落点可算了。
    if (!pendingFlyingPath_.isEmpty()) {
        const QString path = pendingFlyingPath_;
        const QRect rect = pendingFlyingRect_;
        pendingFlyingPath_.clear();
        flyCapture(path, rect);
    }
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

int Clothesline::indexOfPath(const QString& path) const {
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        if (QString::fromStdString(
                entries_[static_cast<std::size_t>(i)].entry.path) == path) {
            return i;
        }
    }
    return -1;
}

int Clothesline::itemAt(const QPoint& pos) const {
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        const Item& item = entries_[static_cast<std::size_t>(i)];
        // 飞行中 / 坠落中的卡片由覆盖层接管，面板内不可点。
        if (item.flying || item.falling) {
            continue;
        }
        if (cardRectF(i).contains(cardLocalPoint(i, pos))) return i;
    }
    return -1;
}

void Clothesline::tick() {
    if (!watchEnabled_ || menuOpen_ || pressedOnCard_ || draggingOut_) {
        return;
    }

    const QPoint pos = QCursor::pos();
    QScreen* screen = QGuiApplication::screenAt(pos);
    if (screen == nullptr) {
        return;
    }
    updateRevealForPointer(pos, screen->geometry(), frameClock_.elapsed());
}

void Clothesline::updateRevealForPointer(const QPoint& pos, const QRect& area, qint64 now) {
    if (menuOpen_ || pressedOnCard_ || draggingOut_) return;
    const bool atTopEdge = area.contains(pos) && pos.y() <= area.top() + kEdgeZone;
    if (!revealed_) {
        outsideSinceMs_ = -1;
        if (slideMode_ != Slide::None) return;
        if (!atTopEdge) { edgeSinceMs_ = -1; return; }
        if (edgeSinceMs_ < 0) edgeSinceMs_ = now;
        if (now - edgeSinceMs_ >= 250) {
            edgeSinceMs_ = -1;
            reveal(area);
        }
        return;
    }
    // 保持区始终属于已展开的屏幕，不能随指针切换到另一块屏幕。
    const QRect keepOpen(x() - 8, anchoredScreenArea_.top() - 8,
                         width() + 16, y() + height() - anchoredScreenArea_.top() + 16);
    if (keepOpen.contains(pos) || (peekUntilMs_ >= 0 && now < peekUntilMs_)) {
        outsideSinceMs_ = -1;
        return;
    }
    if (outsideSinceMs_ < 0) outsideSinceMs_ = now;
    if (now - outsideSinceMs_ >= 500) conceal();
}

QScreen* Clothesline::panelScreen() const {
    // 用当前所在屏幕；未定位时退回主屏。
    QScreen* screen =
        QGuiApplication::screenAt(anchoredScreenArea_.center());
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    return screen;
}

QRect Clothesline::panelScreenArea() const {
    QScreen* screen = panelScreen();
    return screen != nullptr ? screen->geometry() : QRect(0, 0, 1280, 800);
}

// ---- 帧驱动 ----

void Clothesline::ensureFrameTimer() {
    if (!frameTimer_->isActive()) {
        lastFrameMs_ = frameClock_.elapsed();
        frameTimer_->start(kFrameMs);
    }
}

void Clothesline::advanceFrame() {
    const qint64 now = frameClock_.elapsed();
    // 卡顿后不做大跳：dt 截断到 50ms（弹簧在半隐式欧拉下仍稳定，
    // 只是动画"慢放"而不是瞬移）。
    const double dtMs = std::min<double>(now - lastFrameMs_, 50.0);
    lastFrameMs_ = now;
    const double dt = dtMs / 1000.0;
    bool active = false;
    bool dirty = false;

    // 面板滑动
    if (slideMode_ == Slide::Spring) {
        const double yv = slideSpring_.step(dt);
        if (slideSpring_.settled(0.5)) {
            contentOffsetY_ = slideTargetY_;
            slideMode_ = Slide::None;
            if (slideHideAtEnd_) {
                hide();
            }
        } else {
            contentOffsetY_ = yv;
            active = true;
        }
    } else if (slideMode_ == Slide::EaseIn) {
        slideElapsedMs_ += dtMs;
        const double t = std::min(1.0, slideElapsedMs_ / kSlideEaseInMs);
        contentOffsetY_ = slideFrom_ + (slideTargetY_ - slideFrom_) * core::swiftuiEaseIn(t);
        if (t >= 1.0) {
            contentOffsetY_ = slideTargetY_;
            slideMode_ = Slide::None;
            if (slideHideAtEnd_) {
                hide();
            }
        } else {
            active = true;
        }
    }

    update();  // 只重绘内容，动画期间绝不移动系统窗口

    // 卡片动效
    for (Item& item : entries_) {
        if (advanceItem(item, dt, dtMs)) {
            active = true;
            dirty = true;
        }
    }
    if (!copiedPath_.isEmpty()) {
        if (now < copiedUntilMs_) active = true;
        else { copiedPath_.clear(); copiedUntilMs_ = -1; }
        dirty = true;
    }
    if (dirty) {
        update();
    }
    if (!active) {
        frameTimer_->stop();
    }
}

bool Clothesline::advanceItem(Item& item, double dt, double dtMs) {
    bool active = false;

    // 挂载掉落：纵向弹簧 + 欠阻尼摆动（tendedero 的挂载动画）。
    if (item.arriving) {
        item.arriveOffset.step(dt);
        item.arriveSwing.step(dt);
        if (item.arriveOffset.settled(0.2) && item.arriveSwing.settled(0.05)) {
            item.arriveOffset.value = 0;
            item.arriveOffset.velocity = 0;
            item.arriveSwing.value = 0;
            item.arriveSwing.velocity = 0;
            item.arriving = false;
        } else {
            active = true;
        }
    }

    // 落地淡入（0.16s easeOut）。
    if (item.landing) {
        item.landElapsedMs += dtMs;
        if (item.landElapsedMs >= kLandingMs) {
            item.landing = false;
        } else {
            active = true;
        }
    }

    // 摆动两段式：easeOut 到目标摆角 → 欠阻尼弹簧回稳。
    SwingState& sw = item.swing;
    if (sw.phase == SwingState::Phase::EaseOut) {
        sw.easeElapsedMs += dtMs;
        const double t = std::min(1.0, sw.easeElapsedMs / kNudgeEaseMs);
        sw.value = sw.easeFrom +
                   (sw.easeTo - sw.easeFrom) * core::swiftuiEaseOut(t);
        if (t >= 1.0) {
            sw.phase = SwingState::Phase::Spring;
            sw.spring = core::Spring::fromStiffness(46, 2.6, sw.easeTo, 0);
        }
        active = true;
    } else if (sw.phase == SwingState::Phase::Spring) {
        sw.spring.step(dt);
        sw.value = sw.spring.value;
        if (sw.spring.settled(0.05)) {
            sw.value = 0;
            sw.phase = SwingState::Phase::Idle;
        } else {
            active = true;
        }
    }
    return active;
}

// ---- 滑入 / 滑出 ----

void Clothesline::startSlide(int targetY, bool hideAtEnd, bool springy) {
    slideTargetY_ = targetY;
    slideHideAtEnd_ = hideAtEnd;
    if (springy) {
        // 展开：SwiftUI spring(response: 0.42, dampingFraction: 0.72)，
        // 带过冲回弹 —— tendedero 面板落下的"顿一下"感。
        slideMode_ = Slide::Spring;
        slideSpring_ =
            core::Spring::fromResponse(0.42, 0.72, contentOffsetY_, targetY);
    } else {
        // 收起：easeIn 0.22s，加速离场不拖沓。
        slideMode_ = Slide::EaseIn;
        slideFrom_ = contentOffsetY_;
        slideElapsedMs_ = 0;
    }
    ensureFrameTimer();
}

void Clothesline::reveal(const QRect& area) {
    if (revealed_) {
        return;
    }
    revealed_ = true;
    anchoredScreenArea_ = area;
    edgeSinceMs_ = outsideSinceMs_ = -1;
    // macOS 会约束顶层窗口位置。窗口只定位一次；滑动只改变绘制偏移，
    // 防止负坐标反复被系统修正而使衣架逐帧下坠。
    int panelTop = area.top();
#ifdef Q_OS_MACOS
    if (QScreen* screen = QGuiApplication::screenAt(area.center())) {
        panelTop = qMax(panelTop, screen->availableGeometry().top());
    }
#endif
    contentOffsetY_ = -kPanelHeight;
    setGeometry(area.left(), panelTop, area.width(), kPanelHeight);
    show();
    raise();
    // 等这次展开的卡片此刻起播挂载掉落（tendedero：卡片随面板落下）。
    for (Item& item : entries_) {
        if (item.pendingArrive && !item.flying) {
            item.pendingArrive = false;
            item.arriving = true;
            item.arriveOffset =
                core::Spring::fromResponse(0.42, 0.72, -kArriveDrop, 0);
            item.arriveSwing =
                core::Spring::fromStiffness(46, 2.6, kArriveTilt, 0);
        }
    }
    startSlide(0, /*hideAtEnd=*/false, /*springy=*/true);
    // 刚展开就有一阵风按卡片顺序扫过，之后再间歇轻推。
    startRevealBreeze();
    gustTimer_->start(QRandomGenerator::global()->bounded(2200, 4600));
    qInfo().noquote() << QStringLiteral("[jie2] 衣架展开（%1 项）")
                             .arg(entries_.size());
    update();
}

void Clothesline::peek(const QRect& area) {
    // 亮相窗口期：期间 tick 不自动收起。
    peekUntilMs_ = frameClock_.elapsed() + kPeekMs;
    if (!revealed_) {
        reveal(area);
    }
}

void Clothesline::conceal() {
    ++revealEpoch_;
    copiedPath_.clear();
    copiedUntilMs_ = -1;
    holdTimer_->stop();
    pressedOnCard_ = false;
    pressedIndex_ = -1;
    hoverIndex_ = -1;
    peekUntilMs_ = -1;
    gustTimer_->stop();
    if (!revealed_) {
        // 仍未展开时保持隐藏；同时确保不在屏幕上留下痕迹。
        slideMode_ = Slide::None;
        hide();
        return;
    }
    revealed_ = false;
    // 掉到一半的卡片就地定格：下次展开不再重播掉落
    // （tendedero 收起也不重播入场动画）。
    for (Item& item : entries_) {
        item.arriving = false;
        item.pendingArrive = false;
        item.arriveOffset.value = 0;
        item.arriveSwing.value = 0;
    }
    // 滑出动画结束时才 hide()（否则动画看不见）。
    startSlide(-kPanelHeight, /*hideAtEnd=*/true,
               /*springy=*/false);
    qInfo().noquote() << QStringLiteral("[jie2] 衣架收起");
}

void Clothesline::hideNow() {
    ++revealEpoch_;
    copiedPath_.clear();
    copiedUntilMs_ = -1;
    holdTimer_->stop();
    pressedOnCard_ = false;
    pressedIndex_ = -1;
    // 用于"截图前立刻消失"：不等动画，直接隐藏。
    revealed_ = false;
    hoverIndex_ = -1;
    peekUntilMs_ = -1;
    gustTimer_->stop();
    slideMode_ = Slide::None;
    contentOffsetY_ = -kPanelHeight;
    edgeSinceMs_ = outsideSinceMs_ = -1;
    frameTimer_->stop();
    hide();
    // 飞行/坠落中的覆盖层是全屏置顶窗口，也会被拍进截图。
    FlightOverlay::dismissAll();
}

// ---- 截图飞入（tendedero CaptureFlight 的接入点）----

void Clothesline::flyCapture(const QString& path, const QRect& globalRect) {
    if (path.isEmpty() || !globalRect.isValid() || globalRect.isEmpty()) {
        return;
    }
    const int index = indexOfPath(path);
    if (index < 0) {
        // setEntries 还没跑到（防御性顺序）：暂存，等它重建后重试。
        pendingFlyingPath_ = path;
        pendingFlyingRect_ = globalRect;
        return;
    }
    // tendedero：同时最多飞 2 张，多出的直接落位（走挂载掉落）。
    if (FlightOverlay::flightsInProgress() >= 2) {
        return;
    }

    Item& item = entries_[static_cast<std::size_t>(index)];
    // 起飞用较清晰版本（FlightOverlay 内部会缩放绘制）；文件读不出
    // 就退用缩略图。
    QImage photo(QString::fromStdString(item.entry.path));
    if (photo.isNull()) {
        photo = item.thumbnail;
    }
    if (photo.isNull()) {
        return;
    }

    // 目标屏：截图区域所在屏（衣架也在同一屏探出承接）。
    QScreen* screen = QGuiApplication::screenAt(globalRect.center());
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen == nullptr) {
        return;
    }
    const QRect area = screen->geometry();

    // 面板探出承接（未展开时先滑下来；滑动中途就起飞，不等待 ——
    // tendedero 同样如此）。
    peek(area);

    // 坐标换算：面板展开到位后贴在屏幕顶边，面板本地坐标正好等于
    // 屏幕本地逻辑坐标（x 偏移同为 area.left()）。
    const QRectF to = cardRectF(index).translated(x() - area.left(), y() - area.top());
    const QRectF from = QRectF(globalRect).translated(-area.topLeft());

    item.flying = true;
    item.pendingArrive = false;
    item.arriving = false;
    update();  // 本体立刻隐藏，覆盖层接管

    QPointer<Clothesline> guard(this);
    qInfo().noquote() << QStringLiteral("[jie2] 衣架：截图飞入 %1").arg(path);
    FlightOverlay::fly(photo, from, to, item.tilt, screen,
                       [guard, path]() {
                           if (guard != nullptr) {
                               guard->landFlying(path);
                           }
                       });
}

void Clothesline::landFlying(const QString& path) {
    const int index = indexOfPath(path);
    if (index < 0) {
        return;
    }
    Item& item = entries_[static_cast<std::size_t>(index)];
    item.flying = false;
    // 落地淡入 0.16s（CaptureFlight：窗口淡出与卡片淡入同步开始）。
    item.landing = true;
    item.landElapsedMs = 0;
    ensureFrameTimer();
    update();
}

// ---- 风吹与取下 ----

void Clothesline::nudge(int index, double degrees) {
    if (index < 0 || index >= static_cast<int>(entries_.size())) {
        return;
    }
    Item& item = entries_[static_cast<std::size_t>(index)];
    if (item.flying || item.falling) {
        return;
    }
    SwingState& sw = item.swing;
    sw.phase = SwingState::Phase::EaseOut;
    sw.easeFrom = sw.value;
    sw.easeTo = sw.value + degrees;
    sw.easeElapsedMs = 0;
    ensureFrameTimer();
}

void Clothesline::startRevealBreeze() {
    const quint64 epoch = ++revealEpoch_;
    const double direction = QRandomGenerator::global()->bounded(2) ? 1.0 : -1.0;
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        const QString path = QString::fromStdString(entries_[i].entry.path);
        const double degrees = direction * (1.8 + (i % 3) * 0.35);
        QTimer::singleShot(140 + i * 55, this, [this, epoch, path, degrees] {
            if (!revealed_ || revealEpoch_ != epoch) return;
            nudge(indexOfPath(path), degrees);
        });
    }
}

void Clothesline::gustTick() {
    if (!revealed_ || entries_.empty()) {
        return;
    }
    // tendedero 的风：随机挑一张轻推 1.2°–3.0°（方向随机），
    // 然后欠阻尼回稳 —— 看起来像被风拂了一下。
    auto* rng = QRandomGenerator::global();
    const int index = rng->bounded(static_cast<int>(entries_.size()));
    const double magnitude = 1.2 + rng->generateDouble() * 1.8;
    const double sign = rng->bounded(2) == 0 ? -1.0 : 1.0;
    nudge(index, sign * magnitude);
    gustTimer_->start(rng->bounded(2200, 4600));
}

void Clothesline::takeDown(int index) {
    if (index < 0 || index >= static_cast<int>(entries_.size())) {
        return;
    }
    Item& item = entries_[static_cast<std::size_t>(index)];
    if (item.falling) return;
    const QString path = QString::fromStdString(item.entry.path);
    qInfo().noquote() << QStringLiteral("[jie2] 衣架：取下 %1").arg(path);

    // 坠落动画：覆盖层从卡片当前位置接管（tendedero：卡片加速坠落、
    // 越转越歪、渐隐，0.55s）。
    if (revealed_ && !item.thumbnail.isNull()) {
        QScreen* screen = panelScreen();
        const QRect area = panelScreenArea();
        // 面板本地 → 屏幕本地：补上当前窗口偏移（滑动中途也正确）。
        QRectF card = cardRectF(index);
        card.translate(x() - area.left(), y() - area.top() + contentOffsetY_);
        FlightOverlay::fall(item.thumbnail, card,
                            item.tilt + item.swing.value, screen);
    }
    item.falling = true;
    update();

    // 布局保持到坠落结束才收拢（tendedero：items removed after 0.6s），
    // 之后发 entryDropped 让上层更新历史。
    QTimer::singleShot(kFallHoldMs, this, [this, path]() {
        const int i = indexOfPath(path);
        if (i >= 0) {
            entries_.erase(entries_.begin() + i);
        }
        update();
        emit entryDropped(path);
    });
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

    const QImage& shadow = cardShadow(card.size().toSize(), hovered);
    painter.drawImage(card.topLeft() + QPointF(-20, hovered ? -12 : -15), shadow);
    painter.setPen(Qt::NoPen);
    // 玻璃底（ultraThinMaterial 的近似：半透明主题色）
    painter.setBrush(colors_.cardGlass);
    painter.drawRoundedRect(card, kFrameRadius, kFrameRadius);

    // 玻璃描边：顶部受光的纵向渐变（白 55% → 白 12%）
    QLinearGradient specular(0, card.top(), 0, card.bottom());
    specular.setColorAt(0.0, QColor(255, 255, 255, 205));
    specular.setColorAt(1.0, QColor(255, 255, 255, 95));
    painter.setPen(QPen(QBrush(specular), 1.0));
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
    if (hovered) {
        const QRectF close = closeRect(index);
        painter.setPen(QPen(QColor(255, 255, 255, 115), .7));
        painter.setBrush(QColor(24, 27, 40, 190));
        painter.drawEllipse(close);
        painter.setPen(QPen(Qt::white, 1.2, Qt::SolidLine, Qt::RoundCap));
        const QPointF c = close.center();
        painter.drawLine(c + QPointF(-2.5, -2.5), c + QPointF(2.5, 2.5));
        painter.drawLine(c + QPointF(2.5, -2.5), c + QPointF(-2.5, 2.5));
    }

    if (QString::fromStdString(item.entry.path) == copiedPath_ &&
        copiedUntilMs_ > frameClock_.elapsed()) {
        painter.save();
        const qreal fade = std::min(1.0, (copiedUntilMs_ - frameClock_.elapsed()) / 220.0);
        painter.setOpacity(painter.opacity() * fade);
        QFont font; font.setPointSize(11); font.setWeight(QFont::DemiBold);
        painter.setFont(font);
        const QString text = QStringLiteral("已复制");
        const qreal width = QFontMetricsF(font).horizontalAdvance(text) + 42;
        const QRectF bubble(card.center().x() - width / 2, card.bottom() - 5, width, 27);
        painter.setPen(QPen(QColor(255, 255, 255, 125), .75));
        painter.setBrush(darkTheme_ ? QColor(174, 155, 199, 245) : QColor(224, 215, 237, 250));
        painter.drawRoundedRect(bubble, 13.5, 13.5);
        painter.setPen(QPen(QColor(39, 31, 49), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const QPointF check(bubble.left() + 13, bubble.center().y());
        QPolygonF checkmark;
        checkmark << check + QPointF(-3, 0) << check + QPointF(0, 3) << check + QPointF(6, -4);
        painter.drawPolyline(checkmark);
        painter.drawText(bubble.adjusted(29, 0, -10, 0), Qt::AlignCenter, text);
        painter.restore();
    }

}

void Clothesline::drawEmptyHint(QPainter& painter) const {
    // 空态提示：绳子中点下方的胶囊气泡（tendedero 的 Hint）。
    const QString text =
        QStringLiteral("截图后，图片会暂存在这里");
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
    painter.translate(0, contentOffsetY_);

    // 无底板：面板透明，只画绳子、衣夹与卡片（tendedero 形态）。
    drawRope(painter);

    if (entries_.empty()) {
        drawEmptyHint(painter);
        return;
    }

    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        const Item& item = entries_[static_cast<std::size_t>(i)];
        // 飞行中（截图飞入）与坠落中的卡片由覆盖层接管，面板内不画。
        if (item.flying || item.falling) {
            continue;
        }
        const QPointF anchor = cardAnchor(i);

        painter.save();
        // 整张悬挂单元（衣夹 + 卡片）绕悬挂点倾斜：
        // 静止倾角 + 风吹摆角 + 挂载掉落的摆动分量。
        painter.translate(anchor);
        painter.rotate(item.tilt + item.swing.value +
                       (item.arriving ? item.arriveSwing.value : 0.0));
        painter.translate(-anchor);

        // 挂载掉落：纵向弹簧偏移 + 透明度渐入（tendedero 的
        // opacity 与位移同步）。
        double opacity = 1.0;
        if (item.arriving) {
            painter.translate(0, item.arriveOffset.value);
            opacity *= std::clamp(
                1.0 + item.arriveOffset.value / kArriveDrop, 0.0, 1.0);
        }
        // 落地淡入（0.16s easeOut）。
        if (item.landing) {
            opacity *= core::swiftuiEaseOut(
                std::min(1.0, item.landElapsedMs / kLandingMs));
        }
        painter.setOpacity(opacity);

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
        // 衣夹位于照片之前，保留与照片重叠的完整长度。
        drawClothespin(painter, anchor);
        painter.restore();
    }
}

QRectF Clothesline::closeRect(int index) const {
    const QRectF card = cardRectF(index);
    return QRectF(card.left() + 4, card.top() + 4, 20, 20);
}

QPointF Clothesline::cardLocalPoint(int index, const QPoint& pos) const {
    const Item& item = entries_[index];
    const QPointF anchor = cardAnchor(index);
    QTransform transform;
    transform.translate(anchor.x(), anchor.y() + contentOffsetY_);
    transform.rotate(item.tilt + item.swing.value + (item.arriving ? item.arriveSwing.value : 0));
    transform.translate(-anchor.x(), -anchor.y());
    if (item.arriving) transform.translate(0, item.arriveOffset.value);
    const QRectF card = cardRectF(index);
    const QPointF top(card.center().x(), card.top());
    transform.translate(top.x(), top.y());
    const double scale = pressedOnCard_ && pressedIndex_ == index ? 0.95 : hoverIndex_ == index ? 1.035 : 1.0;
    transform.scale(scale, scale);
    transform.translate(-top.x(), -top.y());
    return transform.inverted().map(QPointF(pos));
}

QMenu* Clothesline::createContextMenu(const QString& path) {
    auto* menu = new QMenu(this);
    auto add = [menu](const QString& label, auto callback) {
        auto* action = menu->addAction(label);
        QObject::connect(action, &QAction::triggered, menu, callback);
        return action;
    };
    add(QStringLiteral("复制"), [this, path] { emit copyRequested(path); });
    add(QStringLiteral("打开"), [this, path] { emit openRequested(path); });
    add(QStringLiteral("标注"), [this, path] { emit annotateRequested(path); });
#ifdef Q_OS_MACOS
    add(QStringLiteral("显示文件"), [this, path] { emit revealRequested(path); });
#else
    add(QStringLiteral("显示文件"), [this, path] { emit revealRequested(path); });
#endif
    add(QStringLiteral("存到桌面"), [this, path] { emit saveDesktopRequested(path); });
    menu->addSeparator();
    const bool owned = !managedDirectory_.isEmpty() &&
        QFileInfo(path).canonicalPath() == QFileInfo(managedDirectory_).canonicalFilePath();
    auto* discard = add(owned ? QStringLiteral("删除图片") : QStringLiteral("移出暂存"),
        [this, path] { emit discardRequested(path); });
    discard->setToolTip(owned ? QStringLiteral("移到废纸篓，可从废纸篓恢复")
                              : QStringLiteral("仅从暂存移除，保留原文件"));
    menu->setToolTipsVisible(true);
    return menu;
}

void Clothesline::mousePressEvent(QMouseEvent* event) {
    const int index = itemAt(event->pos());
    if (event->button() == Qt::RightButton) {
        if (index >= 0) {
            holdTimer_->stop();
            pressedOnCard_ = false;
            pressedIndex_ = -1;
            const QString path = QString::fromStdString(entries_[index].entry.path);
            auto* menu = createContextMenu(path);
            menuOpen_ = true;
            execContextMenu(menu, mouseGlobalPosition(event));
            menuOpen_ = false;
            menu->deleteLater();
        } else {
            conceal();
        }
        return;
    }
    if (event->button() == Qt::LeftButton && index >= 0) {
        if (closeRect(index).contains(cardLocalPoint(index, event->pos())) && hoverIndex_ == index) {
            const QString path = QString::fromStdString(entries_[index].entry.path);
            emit discardRequested(path);
            return;
        }
        // 先记录：等 mouseMove 判断这是"点击"还是"拖出"。
        pressedOnCard_ = true;
        pressPos_ = event->pos();
        pressedIndex_ = index;
        holdTimer_->start();
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
    const QImage fullImage(path);
    if (!fullImage.isNull()) {
        mime->setImageData(fullImage);
    }

    QDrag drag(this);
    drag.setMimeData(mime);
    if (!thumbnail.isNull()) {
        drag.setPixmap(QPixmap::fromImage(thumbnail)
                           .scaled(120, 84, Qt::KeepAspectRatio,
                                   Qt::SmoothTransformation));
    }
    qInfo().noquote() << QStringLiteral("[jie2] 衣架：拖出 %1").arg(path);
    draggingOut_ = true;
    const Qt::DropAction action = drag.exec(Qt::CopyAction | Qt::MoveAction,
                                            Qt::CopyAction);
    draggingOut_ = false;

    // 语义（对齐 tendedero）：
    //   - MoveAction：对方把文件拿走（移动）→ 卡片离开衣架；
    //   - CopyAction：对方只取了一份拷贝 → 卡片留在衣架；
    //   - IgnoreAction：拖到无效区域 → 什么都不做。
    if (action == Qt::MoveAction) {
        qInfo().noquote() << QStringLiteral("[jie2] 衣架：%1 已被移走，卡片下线")
                                 .arg(path);
        // 嵌套事件循环后索引可能已失效：按路径定位再删。
        const int i = indexOfPath(path);
        if (i >= 0) {
            entries_.erase(entries_.begin() + i);
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
        holdTimer_->stop();
        pressedOnCard_ = false;
        pressedIndex_ = -1;
        update();  // 撤销按压缩放
        startDragOut(index);
        return;
    }

    const int index = itemAt(event->pos());
    if (index != hoverIndex_) {
        hoverIndex_ = index;
        setCursor(index >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        setToolTip(index >= 0 ? QStringLiteral("单击复制，双击打开，长按标注，拖出分享") : QString());
        update();
    }
}

void Clothesline::mouseReleaseEvent(QMouseEvent* event) {
    holdTimer_->stop();
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

void Clothesline::mouseDoubleClickEvent(QMouseEvent* event) {
    holdTimer_->stop();
    pressedOnCard_ = false;
    pressedIndex_ = -1;
    const int index = itemAt(event->pos());
    if (event->button() == Qt::LeftButton && index >= 0) {
        const QString path = QString::fromStdString(entries_[index].entry.path);
        emit openRequested(path);
    }
    update();
}

void Clothesline::leaveEvent(QEvent*) {
    holdTimer_->stop();
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
