#include "core/annotation.h"

#include <algorithm>
#include <cmath>

namespace jie2::core {

const char* toString(AnnotationTool tool) {
    switch (tool) {
        case AnnotationTool::Rect:      return "rect";
        case AnnotationTool::Ellipse:   return "ellipse";
        case AnnotationTool::Arrow:     return "arrow";
        case AnnotationTool::Pencil:    return "pencil";
        case AnnotationTool::Marker:    return "marker";
        case AnnotationTool::Mosaic:    return "mosaic";
        case AnnotationTool::Blur:      return "blur";
        case AnnotationTool::Eraser:    return "eraser";
        case AnnotationTool::Text:      return "text";
        case AnnotationTool::Number:    return "number";
        case AnnotationTool::Magnifier: return "magnifier";
    }
    return "unknown";
}

Rect Annotation::boundingRect() const {
    if (points.empty()) {
        return {};
    }
    int minX = points.front().x;
    int minY = points.front().y;
    int maxX = points.front().x;
    int maxY = points.front().y;
    for (const Point& p : points) {
        minX = std::min(minX, p.x);
        minY = std::min(minY, p.y);
        maxX = std::max(maxX, p.x);
        maxY = std::max(maxY, p.y);
    }
    return Rect{minX, minY, maxX - minX, maxY - minY};
}

// ---------------------------------------------------------------------------
// 可旋转区域命中测试
// ---------------------------------------------------------------------------

namespace {

struct Vec2 {
    double x = 0.0;
    double y = 0.0;
};

/// 轴对齐矩形的四角多边形。
std::vector<Vec2> boxCorners(const Rect& box) {
    const double l = box.x;
    const double t = box.y;
    const double r = box.x + box.width;
    const double b = box.y + box.height;
    return {{l, t}, {r, t}, {r, b}, {l, b}};
}

/// 区域的凸多边形表示：旋转后的矩形（4 角）或旋转后的椭圆（24 边形近似）。
std::vector<Vec2> regionCorners(const Rect& region, int angleDeg, bool ellipse) {
    const double cx = region.x + region.width / 2.0;
    const double cy = region.y + region.height / 2.0;
    const double rad = static_cast<double>(angleDeg) * 3.14159265358979323846 / 180.0;
    const double ca = std::cos(rad);
    const double sa = std::sin(rad);
    const auto rotate = [&](double x, double y) {
        const double dx = x - cx;
        const double dy = y - cy;
        return Vec2{cx + dx * ca - dy * sa, cy + dx * sa + dy * ca};
    };
    if (ellipse) {
        // 24 边形内接近似椭圆：橡皮擦不需要像素级精确，但要稳定可预期。
        std::vector<Vec2> poly;
        constexpr int kSteps = 24;
        for (int i = 0; i < kSteps; ++i) {
            const double t = 2.0 * 3.14159265358979323846 * i / kSteps;
            poly.push_back(rotate(cx + (region.width / 2.0) * std::cos(t),
                                  cy + (region.height / 2.0) * std::sin(t)));
        }
        return poly;
    }
    return {rotate(region.x, region.y),
            rotate(region.x + region.width, region.y),
            rotate(region.x + region.width, region.y + region.height),
            rotate(region.x, region.y + region.height)};
}

/// 两个凸多边形是否相交（SAT：分离轴定理）。
/// 贴边（投影仅接触）算相交 —— 对橡皮擦而言"碰到就擦掉"更符合直觉。
bool polygonsOverlap(const std::vector<Vec2>& a, const std::vector<Vec2>& b) {
    const auto separatedOn = [](const std::vector<Vec2>& p,
                                const std::vector<Vec2>& q, double nx, double ny) {
        double minP = 0.0;
        double maxP = 0.0;
        double minQ = 0.0;
        double maxQ = 0.0;
        bool first = true;
        for (const Vec2& v : p) {
            const double d = v.x * nx + v.y * ny;
            if (first) {
                minP = maxP = d;
                first = false;
            } else {
                minP = std::min(minP, d);
                maxP = std::max(maxP, d);
            }
        }
        first = true;
        for (const Vec2& v : q) {
            const double d = v.x * nx + v.y * ny;
            if (first) {
                minQ = maxQ = d;
                first = false;
            } else {
                minQ = std::min(minQ, d);
                maxQ = std::max(maxQ, d);
            }
        }
        return maxP < minQ || maxQ < minP;
    };
    const auto hasSeparatingAxis = [&separatedOn](const std::vector<Vec2>& poly,
                                                  const std::vector<Vec2>& other) {
        for (std::size_t i = 0; i < poly.size(); ++i) {
            const Vec2& p1 = poly[i];
            const Vec2& p2 = poly[(i + 1) % poly.size()];
            const double ex = p2.x - p1.x;
            const double ey = p2.y - p1.y;
            const double len = std::hypot(ex, ey);
            if (len < 1e-9) {
                continue;  // 退化边（零长）没有可用法线
            }
            const double nx = -ey / len;
            const double ny = ex / len;
            if (separatedOn(poly, other, nx, ny)) {
                return true;
            }
        }
        return false;
    };
    return !hasSeparatingAxis(a, b) && !hasSeparatingAxis(b, a);
}

} // namespace

bool regionIntersectsBox(const Rect& region, int angleDeg, bool ellipseShape,
                         const Rect& box) {
    if (region.isEmpty()) {
        return false;
    }
    const bool axisAligned = (angleDeg % 360) == 0;
    // 快路径：轴对齐矩形 vs 非退化矩形 —— 与经典"有重叠面积"语义完全一致。
    // 退化包围盒（零宽/零高）交给多边形路径（贴边即命中，修复旧缺陷）。
    if (axisAligned && !ellipseShape && box.width > 0 && box.height > 0 &&
        region.width > 0 && region.height > 0) {
        return !box.intersected(region).isEmpty();
    }
    return polygonsOverlap(regionCorners(region, angleDeg, ellipseShape),
                           boxCorners(box));
}

// ---------------------------------------------------------------------------
// AnnotationDocument
// ---------------------------------------------------------------------------

bool AnnotationDocument::add(Annotation annotation) {
    // 最小点数随工具而定：
    //   - 两点形状（矩形/椭圆/箭头/折线/自由曲线）与区域效果（马赛克/模糊）：≥2
    //   - 点状标注（文字/序号）：≥1
    const std::size_t minPoints =
        (annotation.tool == AnnotationTool::Text ||
         annotation.tool == AnnotationTool::Number)
            ? 1u
            : 2u;
    if (annotation.points.size() < minPoints) {
        return false;
    }
    // 文字不允许空白（否则画面上什么都看不到，属于无效标注）。
    if (annotation.tool == AnnotationTool::Text && annotation.text.empty()) {
        return false;
    }
    annotation.serial = nextSerial_++;
    items_.push_back(std::move(annotation));
    commit();
    return true;
}

std::string AnnotationDocument::nextNumberLabel() const {
    // 序号从 1 开始；已有多少个序号标注，下一个就是几。
    // 只数当前 items_（撤销掉的不算）——这与"画面上看得见几个"一致。
    std::size_t count = 0;
    for (const Annotation& item : items_) {
        if (item.tool == AnnotationTool::Number) {
            ++count;
        }
    }
    return std::to_string(count + 1);
}

bool AnnotationDocument::undo() {
    if (cursor_ == 0) {
        return false;
    }
    pending_ = false;  // 撤销回到已提交状态，挂起的手势修改作废
    --cursor_;
    items_ = history_[cursor_];
    return true;
}

bool AnnotationDocument::redo() {
    if (cursor_ + 1 >= history_.size()) {
        return false;
    }
    pending_ = false;
    ++cursor_;
    items_ = history_[cursor_];
    return true;
}

void AnnotationDocument::commit() {
    pending_ = false;  // 任何一次完整提交都会终结"未入历史"状态
    // 丢弃"未来"（撤销后又编辑 → redo 不可用，标准编辑器语义）。
    history_.resize(cursor_ + 1);
    history_.push_back(items_);
    cursor_ = history_.size() - 1;
    if (history_.size() > kMaxHistoryStates) {
        const std::size_t drop = history_.size() - kMaxHistoryStates;
        history_.erase(history_.begin(),
                       history_.begin() + static_cast<std::ptrdiff_t>(drop));
        cursor_ -= drop;
    }
}

std::size_t AnnotationDocument::eraseIntersecting(const Rect& region,
                                                  int angleDeg,
                                                  bool ellipseShape) {
    if (region.isEmpty()) {
        return 0;
    }
    std::vector<std::uint64_t> removedSerials;
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [&](const Annotation& item) {
                                    const bool hit = regionIntersectsBox(
                                        region, angleDeg, ellipseShape,
                                        item.boundingRect());
                                    if (hit) {
                                        removedSerials.push_back(item.serial);
                                    }
                                    return hit;
                                }),
                 items_.end());
    if (removedSerials.empty()) {
        return 0;
    }
    const auto wasRemoved = [&removedSerials](const Annotation& item) {
        return std::find(removedSerials.begin(), removedSerials.end(),
                         item.serial) != removedSerials.end();
    };

    if (!eraserUndoable_) {
        // 禁用二次编辑：把被擦除的标注从**所有历史快照**里也剔除，
        // 并且不为本次擦除新增状态 —— 无论怎么撤销/重做都不会复活，
        // 而其它编辑的撤销/重做仍然正常。
        history_.resize(cursor_ + 1);  // 与普通编辑一致：先丢弃"未来"
        for (auto& snapshot : history_) {
            snapshot.erase(
                std::remove_if(snapshot.begin(), snapshot.end(), wasRemoved),
                snapshot.end());
        }
        cursor_ = history_.size() - 1;
        // 当前 items_ 已是最新快照（history_.back() 的内容与之一致）。
    } else {
        commit();
    }
    return removedSerials.size();
}

bool AnnotationDocument::removeAt(std::size_t index) {
    if (index >= items_.size()) {
        return false;
    }
    items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(index));
    commit();
    return true;
}

bool AnnotationDocument::translateAt(std::size_t index, int dx, int dy) {
    if (index >= items_.size() || (dx == 0 && dy == 0)) {
        return false;
    }
    // 平移点集即可平移整条标注（所有绘制类型都由 points 驱动）。
    Annotation& item = items_[index];
    for (Point& point : item.points) {
        point.x += dx;
        point.y += dy;
    }
    commit();
    return true;
}

bool AnnotationDocument::replacePoints(std::size_t index,
                                       std::vector<Point> points, bool record) {
    if (index >= items_.size() || points.empty()) {
        return false;
    }
    items_[index].points = std::move(points);
    if (record) {
        commit();
    } else {
        pending_ = true;
    }
    return true;
}

void AnnotationDocument::commitPending() {
    if (pending_) {
        commit();
        pending_ = false;
    }
}

std::vector<const Annotation*> AnnotationDocument::pixelOpItems() const {
    std::vector<const Annotation*> result;
    for (const Annotation& item : items_) {
        if (item.tool == AnnotationTool::Mosaic ||
            item.tool == AnnotationTool::Blur) {
            result.push_back(&item);
        }
    }
    return result;
}

void AnnotationDocument::clear() {
    items_.clear();
    history_ = {std::vector<Annotation>{}};
    cursor_ = 0;
    pending_ = false;
}

} // namespace jie2::core
