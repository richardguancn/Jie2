#pragma once

// 「截2」动画内核：阻尼弹簧与标准缓动曲线。
//
// 用途：衣架（tendedero 合并）的摆动、挂载掉落、面板滑动等动效。
// tendedero 用 SwiftUI 动画原语（interpolatingSpring / spring(response:
// dampingFraction:) / easeIn / easeOut），这里给出等价的数值实现，
// 放在 core 层是为了不依赖 Qt、可以脱机单测。
//
// 参数口径（与 SwiftUI 对齐）：
//   - interpolatingSpring(stiffness k, damping c)：质量 m=1 的阻尼振子
//       x'' = -k·(x - target) - c·v
//   - spring(response R, dampingFraction ζ)：换算为 k = (2π/R)²、
//     c = 2ζ·(2π/R)（SwiftUI 官方换算式）
//   - easeIn / easeOut：SwiftUI 的三次贝塞尔（见下）

namespace jie2::core {

/// 阻尼弹簧状态（质量 = 1）。
///
/// 用法：设定 value/velocity/target 后按帧调用 step(dt) 推进；
/// settled() 判定是否可停表。积分用半隐式欧拉（先速度后位移），
/// 在 60–120 fps 下对本模块使用的参数范围（k ≤ 224, c ≤ 25）稳定。
struct Spring {
    double stiffness = 0;  // k
    double damping = 0;    // c
    double value = 0;
    double velocity = 0;
    double target = 0;

    /// 推进 dt 秒，返回推进后的 value。dt ≤ 0 时不动作。
    double step(double dt);

    /// 是否已停稳（位置与速度都小于阈值），可停止定时器。
    [[nodiscard]] bool settled(double eps = 0.01) const;

    /// SwiftUI spring(response:dampingFraction:) 的等价构造。
    static Spring fromResponse(double response, double dampingFraction,
                               double value, double target);

    /// SwiftUI interpolatingSpring(stiffness:damping:) 的等价构造。
    static Spring fromStiffness(double stiffness, double damping,
                                double value, double target);
};

// ---- 缓动曲线（输入 t ∈ [0,1]，输出进度；端点精确过 0/1）----

double easeInCubic(double t);      // t³
double easeInOutCubic(double t);
double easeOutCubic(double t);

/// SwiftUI 的 .easeIn：cubic-bezier(0.42, 0, 1, 1)。
double swiftuiEaseIn(double t);
/// SwiftUI 的 .easeOut：cubic-bezier(0.25, 0.1, 0.25, 1)。
double swiftuiEaseOut(double t);

/// 平滑阶跃：x ≤ a → 0，x ≥ b → 1，其间 3u²-2u³（CaptureFlight 的
/// chrome 渐入曲线）。a ≥ b 时退化为阶跃。
double smoothstep(double x, double a, double b);

/// 三次贝塞尔求值（p0=(0,0)、p3=(1,1) 的缓动形式）：
/// 给定 x（时间占比）解出曲线纵坐标。x 裁剪到 [0,1]。
double cubicBezierY(double x, double x1, double y1, double x2, double y2);

} // namespace jie2::core
