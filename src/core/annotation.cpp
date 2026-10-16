#include "core/annotation.h"

#include <algorithm>

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
    items_.push_back(std::move(annotation));
    undone_.clear();
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
    if (items_.empty()) {
        return false;
    }
    undone_.push_back(std::move(items_.back()));
    items_.pop_back();
    return true;
}

bool AnnotationDocument::redo() {
    if (undone_.empty()) {
        return false;
    }
    items_.push_back(std::move(undone_.back()));
    undone_.pop_back();
    return true;
}

std::size_t AnnotationDocument::eraseIntersecting(const Rect& region) {
    if (region.isEmpty()) {
        return 0;
    }
    const std::size_t before = items_.size();
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [&region](const Annotation& item) {
                                    const Rect bounds = item.boundingRect();
                                    // 相交 = 两矩形有重叠面积。
                                    return !bounds.intersected(region).isEmpty();
                                }),
                 items_.end());
    const std::size_t removed = before - items_.size();
    if (removed > 0) {
        // 删除会破坏 redo 的一致性（redo 栈里可能是这些已删标注的后续状态），
        // 与本类"新添加即清空 redo 栈"的既有约定保持一致。
        undone_.clear();
    }
    return removed;
}

bool AnnotationDocument::removeAt(std::size_t index) {
    if (index >= items_.size()) {
        return false;
    }
    items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(index));
    undone_.clear();
    return true;
}

bool AnnotationDocument::translateAt(std::size_t index, int dx, int dy) {
    if (index >= items_.size() || (dx == 0 && dy == 0)) {
        return false;
    }
    // 平移点集即可平移整条标注（所有绘制类型都由 points 驱动）。
    // 注意不清空 redo 栈：平移是"编辑现有对象"，而 redo 栈描述的是
    // 增删序列 —— 这与 Snipaste 的"平移后仍可撤销"一致性无关紧要，
    // 但保持简单：不做 redo 交互。
    Annotation& item = items_[index];
    for (Point& point : item.points) {
        point.x += dx;
        point.y += dy;
    }
    return true;
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
    undone_.clear();
}

} // namespace jie2::core
