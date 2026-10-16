#include "core/snap.h"

#include <cstdlib>

namespace jie2::core {

namespace {

/// 在一组候选坐标里找离 value 最近、且距离 <= threshold 的那个。
/// 返回是否命中，命中时 out 为吸附目标坐标。
bool nearestEdge(int value, const std::vector<int>& edges, int threshold, int* out) {
    bool found = false;
    int bestDistance = 0;
    for (const int edge : edges) {
        const int distance = std::abs(edge - value);
        if (distance <= threshold && (!found || distance < bestDistance)) {
            found = true;
            bestDistance = distance;
            *out = edge;
        }
    }
    return found;
}

} // namespace

SnapResult snapRect(const Rect& dragged, const std::vector<Rect>& candidates,
                    int threshold) {
    SnapResult result;
    result.rect = dragged;
    if (threshold <= 0 || candidates.empty() || dragged.isEmpty()) {
        return result;
    }

    // 把候选矩形的四条边各自收集成纵/横坐标集合。
    // 允许一次拖动同时吸附"左到某窗右侧、上到某窗顶边"这类跨窗口组合，
    // 这是常见需求（贴着两个窗口的边角裁一个区域）。
    std::vector<int> vertical;    // x 坐标：候选的左/右边
    std::vector<int> horizontal;  // y 坐标：候选的上/下边
    vertical.reserve(candidates.size() * 2);
    horizontal.reserve(candidates.size() * 2);
    for (const Rect& candidate : candidates) {
        if (candidate.isEmpty()) {
            continue;
        }
        vertical.push_back(candidate.left());
        vertical.push_back(candidate.right());
        horizontal.push_back(candidate.top());
        horizontal.push_back(candidate.bottom());
    }

    int x = dragged.x;
    int y = dragged.y;
    int right = dragged.right();
    int bottom = dragged.bottom();

    // 左/右边分别吸附；若两边各自找到更近的目标，优先近者。
    int leftTarget = 0;
    int rightTarget = 0;
    const bool leftHit = nearestEdge(x, vertical, threshold, &leftTarget);
    const bool rightHit = nearestEdge(right, vertical, threshold, &rightTarget);

    if (leftHit && rightHit) {
        // 两边都命中：选距离更近的一边吸附，避免同时移动两边把宽度拉变形。
        if (std::abs(leftTarget - x) <= std::abs(rightTarget - right)) {
            x = leftTarget;
            result.snappedLeft = true;
        } else {
            right = rightTarget;
            result.snappedRight = true;
        }
    } else if (leftHit) {
        x = leftTarget;
        result.snappedLeft = true;
    } else if (rightHit) {
        right = rightTarget;
        result.snappedRight = true;
    }

    int topTarget = 0;
    int bottomTarget = 0;
    const bool topHit = nearestEdge(y, horizontal, threshold, &topTarget);
    const bool bottomHit = nearestEdge(bottom, horizontal, threshold, &bottomTarget);

    if (topHit && bottomHit) {
        if (std::abs(topTarget - y) <= std::abs(bottomTarget - bottom)) {
            y = topTarget;
            result.snappedTop = true;
        } else {
            bottom = bottomTarget;
            result.snappedBottom = true;
        }
    } else if (topHit) {
        y = topTarget;
        result.snappedTop = true;
    } else if (bottomHit) {
        bottom = bottomTarget;
        result.snappedBottom = true;
    }

    // 保持非负宽高：吸附不得把矩形拉成负值（例如两边同时向内吸附过界）。
    if (right < x) {
        right = x;
    }
    if (bottom < y) {
        bottom = y;
    }
    result.rect = Rect{x, y, right - x, bottom - y};
    return result;
}

} // namespace jie2::core
