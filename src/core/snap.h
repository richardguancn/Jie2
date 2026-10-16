#pragma once

#include <vector>

#include "core/geometry.h"

// 「截2」图像内核 — 窗口吸附
//
// 为什么放在内核：吸附是纯几何运算，与 UI 框架和平台 API 无关。
// 放在这里可以直接单测（给一组候选矩形 + 一个拖拽矩形，断言吸附结果），
// 不用起 GUI。UI 层只负责把候选窗口矩形喂进来、把"哪条边被吸附了"
// 画出来。

namespace jie2::core {

/// 吸附结果。
struct SnapResult {
    Rect rect;             // 吸附后的矩形
    bool snappedLeft = false;
    bool snappedTop = false;
    bool snappedRight = false;
    bool snappedBottom = false;

    bool snapped() const {
        return snappedLeft || snappedTop || snappedRight || snappedBottom;
    }
};

/// 把拖拽中的矩形吸附到候选边缘。
///
/// 规则（与 Snipaste 的"自动检测界面元素区域"同类，实现取更保守的一种）：
///   - 四条边各自独立判定：边与候选边距离 <= threshold 时吸附到候选边；
///   - 同时考虑候选矩形的四条边（左吸附左/右吸附右等，也允许左吸附右）；
///   - 结果矩形保证非负宽高（吸附不会把矩形拉成负值）；
///   - 没有任何边命中时原样返回（不修改）。
///
/// candidates 与 dragged 使用同一坐标空间；threshold 为吸附触发距离。
SnapResult snapRect(const Rect& dragged, const std::vector<Rect>& candidates,
                    int threshold);

} // namespace jie2::core
