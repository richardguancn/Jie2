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
                           !knownPaths_.contains(item.entry.path);
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
    // 容器被重建：所有基于旧索引的交互状态必须失效。
    // （若不清，hoverIndex_ 可能指向已不存在的项，paintEvent 会越界。）
    hoverIndex_ = -1;
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
        const QPointF anchor = cardAnchor(i);
        // 逆旋转回正后再做矩形包含判定（卡片是斜挂着的）。
        QTransform transform;
        transform.translate(anchor.x(), anchor.y());
        transform.rotate(item.tilt + item.swing.value);
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
        // （!revealed_ 时在跑的滑动只可能是收起。）
        if (slideMode_ != Slide::None) {
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
    // 探出亮相期间不自动收起（tendedero 的 peek 窗口期）。
    if (peekUntilMs_ >= 0 && frameClock_.elapsed() < peekUntilMs_) {
        return;
    }
    if (!keepOpen.contains(pos)) {
        conceal();
    }
}

QScreen* Clothesline::panelScreen() const {
    // 用当前所在屏幕；未定位时退回主屏。
    QScreen* screen =
        QGuiApplication::screenAt(QPoint(x() + width() / 2, 1));
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
            move(x(), slideTargetY_);
            slideMode_ = Slide::None;
            if (slideHideAtEnd_) {
                hide();
            }
        } else {
            move(x(), qRound(yv));
            active = true;
        }
    } else if (slideMode_ == Slide::EaseIn) {
        slideElapsedMs_ += dtMs;
        const double t = std::min(1.0, slideElapsedMs_ / kSlideEaseInMs);
        move(x(), qRound(slideFrom_ +
                         (slideTargetY_ - slideFrom_) *
                             core::swiftuiEaseIn(t)));
        if (t >= 1.0) {
            move(x(), slideTargetY_);
            slideMode_ = Slide::None;
            if (slideHideAtEnd_) {
                hide();
            }
        } else {
            active = true;
        }
    }

    // 卡片动效
    for (Item& item : entries_) {
        if (advanceItem(item, dt, dtMs)) {
            active = true;
            dirty = true;
        }
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
            core::Spring::fromResponse(0.42, 0.72, y(), targetY);
    } else {
        // 收起：easeIn 0.22s，加速离场不拖沓。
        slideMode_ = Slide::EaseIn;
        slideFrom_ = y();
        slideElapsedMs_ = 0;
    }
    ensureFrameTimer();
}

void Clothesline::reveal(const QRect& area) {
    if (revealed_) {
        return;
    }
    revealed_ = true;
    // 起始位置在屏幕上方（不可见），弹簧滑入到 area.top()。
    setGeometry(area.left(), area.top() - kPanelHeight, area.width(),
                kPanelHeight);
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
    startSlide(area.top(), /*hideAtEnd=*/false, /*springy=*/true);
    // 展开期间才有风（tendedero 的 breeze 只在可见时吹）。
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
    startSlide(panelScreenArea().top() - kPanelHeight, /*hideAtEnd=*/true,
               /*springy=*/false);
    qInfo().noquote() << QStringLiteral("[jie2] 衣架收起");
}

void Clothesline::hideNow() {
    // 用于"截图前立刻消失"：不等动画，直接隐藏。
    revealed_ = false;
    hoverIndex_ = -1;
    peekUntilMs_ = -1;
    gustTimer_->stop();
    slideMode_ = Slide::None;
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
    const QRectF to = cardRectF(index);
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
    const QString path = QString::fromStdString(item.entry.path);
    qInfo().noquote() << QStringLiteral("[jie2] 衣架：取下 %1").arg(path);

    // 坠落动画：覆盖层从卡片当前位置接管（tendedero：卡片加速坠落、
    // 越转越歪、渐隐，0.55s）。
    if (revealed_ && !item.thumbnail.isNull()) {
        QScreen* screen = panelScreen();
        const QRect area = panelScreenArea();
        // 面板本地 → 屏幕本地：补上当前窗口偏移（滑动中途也正确）。
        QRectF card = cardRectF(index);
        card.translate(x() - area.left(), y() - area.top());
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
            takeDown(index);  // 坠落取下（tendedero 的 Take down）
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
