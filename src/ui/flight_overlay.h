#pragma once

// 飞行覆盖层（tendedero CaptureFlight.swift 的 Qt 移植）。
//
// 两种模式：
//   - Fly：新截图从被截取的区域"起飞"，沿 30pt 的弧线飞上衣架，
//     途中收缩成卡片尺寸、倾角渐起、玻璃框与衣夹在 35% 后渐入，
//     0.65s easeInOutCubic；到位后真实卡片接管（调用方在 done 里
//     让衣架卡片现身），本窗口再用 0.16s 淡出。
//   - Fall：被取下的卡片从原位置坠落 —— 下落 520pt、加速
//     （e = k³）、越转越歪、渐隐，0.55s，然后窗口销毁。
//
// 为什么单独开窗口：卡片要飞越/坠出整条屏幕，而衣架面板只有 210pt
// 高，在面板里画会被裁掉（tendedero 同理："drawn over the whole
// screen so it is never cut by the line's strip"）。
//
// 窗口是透明、穿透、置顶的无边框面板：只负责画，不拦截任何输入。

#include <QImage>
#include <QRectF>
#include <QWidget>

#include <functional>
#include <vector>

class QElapsedTimer;
class QScreen;
class QTimer;

namespace jie2::ui {

class FlightOverlay : public QWidget {
    Q_OBJECT

public:
    /// 新截图飞入。from/to 为目标屏幕的**本地逻辑坐标**；
    /// tilt 为落点卡片的静止倾角（度，顺时针为正 —— 与 SwiftUI 一致）。
    /// 飞行结束时回调 done（真实卡片应在此刻现身），随后窗口自行淡出销毁。
    static void fly(QImage photo, const QRectF& from, const QRectF& to,
                    double tilt, QScreen* screen, std::function<void()> done);

    /// 卡片坠落。card 为目标屏幕本地逻辑坐标下的卡片边框矩形。
    static void fall(QImage photo, const QRectF& card, double tilt,
                     QScreen* screen);

    /// 进行中的 Fly 数量（tendedero：同时最多飞 2 张，多出的直接落位）。
    static int flightsInProgress();

    /// 立即销毁所有进行中的覆盖层（截图前调用：这些全屏置顶窗口
    /// 会被拍进下一张截图）。Fly 的落地回调照常触发（调用方需要它
    /// 恢复卡片可见性）。
    static void dismissAll();

    ~FlightOverlay() override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    enum class Mode { Fly, Fall };

    FlightOverlay(Mode mode, QImage photo, const QRectF& from,
                  const QRectF& to, double tilt, QScreen* screen,
                  std::function<void()> done);

    void tick();

    Mode mode_;
    QImage photo_;       // 飞行中的照片（Fly 用较清晰版本，Fall 用缩略图）
    QRectF from_;        // 屏幕本地逻辑坐标
    QRectF to_;
    double tilt_ = 0;
    std::function<void()> done_;

    QTimer* timer_ = nullptr;
    QElapsedTimer* clock_ = nullptr;
    bool completed_ = false;   // Fly：已到位（进入淡出阶段）
    qint64 fadeStartMs_ = -1;  // 淡出起始时刻

    // tendedero CaptureFlight 的原始参数。
    static constexpr double kFlyDuration = 0.65;   // 秒
    static constexpr double kFallDuration = 0.55;
    static constexpr double kFlyArc = 30;          // 弧顶抬升（逻辑点）
    static constexpr double kFadeOut = 0.16;       // 到位后的窗口淡出
    static constexpr double kFallDistance = 520;   // 坠落位移
};

} // namespace jie2::ui
