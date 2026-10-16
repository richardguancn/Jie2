#include "ui/flight_overlay.h"

#include <algorithm>
#include <cmath>

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QTimer>

#include "core/spring.h"

namespace jie2::ui {

namespace {

/// 进行中的 Fly 数量（tendedero：flightsInProgress < 2 才再起飞）。
int gFlightsInProgress = 0;

/// 所有存活实例（dismissAll 用）。覆盖层没有父对象，只能自己记账。
std::vector<FlightOverlay*> gInstances;

/// 一帧的绘制状态（tick 计算，paintEvent 消费）。
struct Frame {
    QRectF card;      // 当前卡片边框矩形（含倾角前的正立矩形）
    double radius = 16;
    double inset = 4;
    double chrome = 1;   // 玻璃框 / 描边 / 衣夹的不透明度（0..1）
    double angle = 0;    // 当前倾角（度，顺时针为正）
    double opacity = 1;  // 整体不透明度（Fall 的渐隐）
};

/// 绘制一张飞行中的卡片（CaptureFlight.update 的图层树：
/// container.shadow → glass → photo → edge → clip）。
void paintFlightCard(QPainter& painter, const Frame& frame,
                     const QImage& photo) {
    painter.save();
    painter.setOpacity(frame.opacity);

    const QRectF& card = frame.card;
    // 旋转锚 = 卡片顶中（CaptureFlight container.anchorPoint = (0.5, 1)）。
    const QPointF topCenter(card.center().x(), card.top());
    painter.translate(topCenter);
    painter.rotate(frame.angle);
    painter.translate(-topCenter);

    // 阴影（黑 24%、半径 10、下移 5；无模糊 API，用两层渐淡近似）。
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 31));
    painter.drawRoundedRect(card.adjusted(-2, -2, 2, 2).translated(0, 6),
                            frame.radius + 2, frame.radius + 2);
    painter.setBrush(QColor(0, 0, 0, 61));
    painter.drawRoundedRect(card.translated(0, 5), frame.radius, frame.radius);

    // 玻璃底（CaptureFlight 的 glass 是固定的近白 72%，不随主题）。
    if (frame.chrome > 0.0) {
        QColor glass(247, 247, 247);
        glass.setAlphaF(0.72 * frame.chrome);
        painter.setBrush(glass);
        painter.drawRoundedRect(card, frame.radius, frame.radius);
    }

    // 照片（resizeAspectFill：等比铺满并裁掉溢出）。
    const QRectF photoRect =
        card.adjusted(frame.inset, frame.inset, -frame.inset, -frame.inset);
    const double photoRadius = std::max(0.0, frame.radius - frame.inset);
    if (!photo.isNull() && photoRect.isValid() && !photoRect.isEmpty()) {
        painter.save();
        QPainterPath clipPath;
        clipPath.addRoundedRect(photoRect, photoRadius, photoRadius);
        painter.setClipPath(clipPath);
        const QSizeF scaled = QSizeF(photo.size()).scaled(
            photoRect.size(), Qt::KeepAspectRatioByExpanding);
        const QRectF target(photoRect.center().x() - scaled.width() / 2,
                            photoRect.center().y() - scaled.height() / 2,
                            scaled.width(), scaled.height());
        painter.drawImage(target, photo);
        painter.restore();
    }

    // 玻璃描边（顶部受光：白 90% → 白 25%，宽 1.5，内缩 0.75）。
    if (frame.chrome > 0.0) {
        QLinearGradient edge(0, card.top(), 0, card.bottom());
        QColor top(255, 255, 255), bottom(255, 255, 255);
        top.setAlphaF(0.9 * frame.chrome);
        bottom.setAlphaF(0.25 * frame.chrome);
        edge.setColorAt(0.0, top);
        edge.setColorAt(1.0, bottom);
        painter.setPen(QPen(QBrush(edge), 1.5));
        painter.setBrush(Qt::NoBrush);
        const QRectF insetCard = card.adjusted(0.75, 0.75, -0.75, -0.75);
        painter.drawRoundedRect(insetCard,
                                std::max(0.0, frame.radius - 0.75),
                                std::max(0.0, frame.radius - 0.75));
    }

    // 铝质衣夹：9×26，与卡片重叠 12（顶中上方伸出 14）。
    if (frame.chrome > 0.0) {
        const QRectF clip(card.center().x() - 4.5, card.top() - 14, 9, 26);
        painter.setOpacity(frame.opacity * frame.chrome);
        QLinearGradient metal(clip.left(), 0, clip.right(), 0);
        metal.setColorAt(0.0, QColor(179, 179, 179));
        metal.setColorAt(0.35, QColor(237, 237, 237));
        metal.setColorAt(0.65, QColor(209, 209, 209));
        metal.setColorAt(1.0, QColor(158, 158, 158));
        painter.setPen(Qt::NoPen);
        painter.setBrush(metal);
        painter.drawRoundedRect(clip, 3.5, 3.5);
        QColor edge(255, 255, 255);
        edge.setAlphaF(0.7);
        painter.setPen(QPen(edge, 0.6));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(clip, 3.5, 3.5);
    }

    painter.restore();
}

} // namespace

FlightOverlay::FlightOverlay(Mode mode, QImage photo, const QRectF& from,
                             const QRectF& to, double tilt, QScreen* screen,
                             std::function<void()> done)
    : QWidget(nullptr),
      mode_(mode),
      photo_(std::move(photo)),
      from_(from),
      to_(to),
      tilt_(tilt),
      done_(std::move(done)) {
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint |
                   Qt::WindowStaysOnTopHint | Qt::Tool |
                   Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_DeleteOnClose, true);

    if (screen != nullptr) {
        setGeometry(screen->geometry());
    }

    gInstances.push_back(this);

    clock_ = new QElapsedTimer();
    clock_->start();
    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &FlightOverlay::tick);
    // 120fps（与 CaptureFlight 的 1/120s 定时器一致）。
    timer_->start(8);
    show();
    raise();
}

void FlightOverlay::fly(QImage photo, const QRectF& from, const QRectF& to,
                        double tilt, QScreen* screen,
                        std::function<void()> done) {
    if (photo.isNull() || screen == nullptr || !from.isValid() ||
        !to.isValid()) {
        if (done) {
            done();
        }
        return;
    }
    ++gFlightsInProgress;
    new FlightOverlay(Mode::Fly, std::move(photo), from, to, tilt, screen,
                      std::move(done));
}

void FlightOverlay::fall(QImage photo, const QRectF& card, double tilt,
                         QScreen* screen) {
    if (photo.isNull() || screen == nullptr || !card.isValid()) {
        return;
    }
    new FlightOverlay(Mode::Fall, std::move(photo), card, card, tilt, screen,
                      {});
}

FlightOverlay::~FlightOverlay() {
    gInstances.erase(std::remove(gInstances.begin(), gInstances.end(), this),
                     gInstances.end());
    if (mode_ == Mode::Fly) {
        --gFlightsInProgress;
    }
}

void FlightOverlay::dismissAll() {
    // 先落地（恢复卡片可见性），再销毁窗口。拷贝一份清单：
    // done_ 回调可能触发衣架重绘，但不会增删本清单。
    const std::vector<FlightOverlay*> instances = gInstances;
    for (FlightOverlay* overlay : instances) {
        if (overlay->mode_ == Mode::Fly && !overlay->completed_) {
            overlay->completed_ = true;
            if (overlay->done_) {
                overlay->done_();
                overlay->done_ = nullptr;
            }
        }
        overlay->close();  // WA_DeleteOnClose：销毁自身
    }
}

int FlightOverlay::flightsInProgress() {
    return gFlightsInProgress;
}

void FlightOverlay::tick() {
    const double elapsed = clock_->elapsed() / 1000.0;
    if (mode_ == Mode::Fly) {
        if (!completed_) {
            if (elapsed >= kFlyDuration) {
                completed_ = true;
                fadeStartMs_ = clock_->elapsed();
                // 真实卡片在此刻接管（tendedero：completion 与 0.16s
                // 的窗口淡出同时开始）。
                if (done_) {
                    done_();
                    done_ = nullptr;
                }
            }
        } else {
            const double fade = (clock_->elapsed() - fadeStartMs_) / 1000.0;
            if (fade >= kFadeOut) {
                close();
                return;
            }
            setWindowOpacity(1.0 - fade / kFadeOut);
        }
    } else {  // Fall
        if (elapsed >= kFallDuration) {
            close();
            return;
        }
    }
    update();
}

void FlightOverlay::paintEvent(QPaintEvent*) {
    const double elapsed = clock_->elapsed() / 1000.0;
    Frame frame;

    if (mode_ == Mode::Fly) {
        const double raw = std::min(1.0, elapsed / kFlyDuration);
        const double k = core::easeInOutCubic(raw);
        frame.chrome = core::smoothstep(k, 0.35, 1.0);
        const auto lerp = [k](double a, double b) { return a + (b - a) * k; };
        const double w = lerp(from_.width(), to_.width());
        const double h = lerp(from_.height(), to_.height());
        const double cx = lerp(from_.center().x(), to_.center().x());
        // 弧线：中途抬升 30pt（Qt y 向下，抬升是负方向）。
        constexpr double kPi = 3.14159265358979323846;  // MSVC 的 M_PI 需宏开启，不用它
        const double top =
            lerp(from_.top(), to_.top()) - std::sin(kPi * k) * kFlyArc;
        frame.card = QRectF(cx - w / 2, top, w, h);
        frame.radius = 16.0 * k;
        frame.inset = 4.0 * k;
        // SwiftUI 与 Qt 的旋转都是顺时针为正，直接乘进度。
        frame.angle = tilt_ * k;
        frame.opacity = 1.0;
    } else {  // Fall
        const double raw = std::min(1.0, elapsed / kFallDuration);
        const double e = raw * raw * raw;  // CaptureFlight：k³ 加速下坠
        frame.card = to_.translated(0, kFallDistance * e);
        // 越落越歪：角度在静止倾角上继续叠加（与原倾斜同号放大 + 20°）。
        frame.angle = tilt_ + (tilt_ * 7 + 20) * e;
        frame.opacity = 1.0 - e;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    paintFlightCard(painter, frame, photo_);
}

} // namespace jie2::ui
