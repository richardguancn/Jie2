#include "core/spring.h"

#include <algorithm>
#include <cmath>

namespace jie2::core {

double Spring::step(double dt) {
    if (dt <= 0.0) {
        return value;
    }
    // 半隐式欧拉：先更新速度再更新位移。显示欧拉在这个刚度下会发散，
    // 半隐式格式无条件稳定且能量行为正确（摆动衰减而不放大）。
    const double acceleration =
        -stiffness * (value - target) - damping * velocity;
    velocity += acceleration * dt;
    value += velocity * dt;
    return value;
}

bool Spring::settled(double eps) const {
    return std::abs(value - target) < eps && std::abs(velocity) < eps;
}

Spring Spring::fromResponse(double response, double dampingFraction,
                            double value, double target) {
    // SwiftUI 官方换算：ω = 2π/response，k = ω²，c = 2·ζ·ω（质量 1）。
    constexpr double kPi = 3.14159265358979323846;  // MSVC 的 M_PI 需宏开启，不用它
    const double omega = 2.0 * kPi / response;
    Spring spring;
    spring.stiffness = omega * omega;
    spring.damping = 2.0 * dampingFraction * omega;
    spring.value = value;
    spring.target = target;
    return spring;
}

Spring Spring::fromStiffness(double stiffness, double damping,
                             double value, double target) {
    Spring spring;
    spring.stiffness = stiffness;
    spring.damping = damping;
    spring.value = value;
    spring.target = target;
    return spring;
}

double easeInCubic(double t) {
    return t * t * t;
}

double easeInOutCubic(double t) {
    return t < 0.5 ? 4.0 * t * t * t
                   : 1.0 - std::pow(-2.0 * t + 2.0, 3.0) / 2.0;
}

double easeOutCubic(double t) {
    const double u = 1.0 - t;
    return 1.0 - u * u * u;
}

double cubicBezierY(double x, double x1, double y1, double x2, double y2) {
    x = std::clamp(x, 0.0, 1.0);
    // 先按 x 解参数 u（牛顿迭代，起始点取 u = x；该曲线单调，迭代收敛快）。
    const auto bx = [x1, x2](double u) {
        const double v = 1.0 - u;
        return 3.0 * v * v * u * x1 + 3.0 * v * u * u * x2 + u * u * u;
    };
    const auto dbx = [x1, x2](double u) {
        const double v = 1.0 - u;
        return 3.0 * v * v * x1 + 6.0 * v * u * (x2 - x1) +
               3.0 * u * u * (1.0 - x2);
    };
    double u = x;
    for (int i = 0; i < 8; ++i) {
        const double err = bx(u) - x;
        if (std::abs(err) < 1e-6) {
            break;
        }
        const double deriv = dbx(u);
        if (std::abs(deriv) < 1e-9) {
            break;
        }
        u = std::clamp(u - err / deriv, 0.0, 1.0);
    }
    const double v = 1.0 - u;
    return 3.0 * v * v * u * y1 + 3.0 * v * u * u * y2 + u * u * u;
}

double swiftuiEaseIn(double t) {
    return cubicBezierY(t, 0.42, 0.0, 1.0, 1.0);
}

double swiftuiEaseOut(double t) {
    return cubicBezierY(t, 0.25, 0.1, 0.25, 1.0);
}

double smoothstep(double x, double a, double b) {
    if (a >= b) {
        return x >= b ? 1.0 : 0.0;
    }
    const double u = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return u * u * (3.0 - 2.0 * u);
}

} // namespace jie2::core
