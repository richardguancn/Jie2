#pragma once

#include <string>
#include <vector>

#include "core/geometry.h"
#include "core/image_buffer.h"

// 「截2」图像内核 — 标注模型（M2）
//
// 设计说明：
//   - 模型只描述"画了什么"，不描述"怎么画"。渲染在 UI 层（QPainter），
//     像素类效果（马赛克/模糊）通过 image_ops 的内核函数实现。
//   - 所有坐标都是**源图物理像素**坐标。UI 层负责逻辑点 ↔ 像素的换算，
//     内核完全不感知屏幕缩放，这样标注在任何 DPI 下语义一致。
//   - 撤销/重做用"整表快照"实现：标注数量级很小（几十条），
//     快照的简单性远胜增量记账的复杂度。

namespace jie2::core {

enum class AnnotationTool {
    Rect,      // 矩形
    Ellipse,   // 椭圆
    Arrow,     // 箭头（拖动两点）
    Pencil,    // 铅笔（自由曲线）
    Marker,    // 马克笔（半透明粗线）
    Mosaic,    // 马赛克（区域像素化）
    Blur,      // 高斯模糊（区域）
    Eraser,    // 橡皮擦（拖过即整条删除，不产生新标注）
    Text,      // 文字
    Number,    // 序号
    Magnifier, // 放大镜标注（把区域内容放大呈现，专业版 v2.6）
};

const char* toString(AnnotationTool tool);

struct AnnotationStyle {
    Rgba color{239, 68, 68, 255};  // 默认红（对截图的对比度最好）
    int width = 3;                 // 线宽（物理像素）
    int fontSize = 28;             // 文字大小（物理像素）
    int blockSize = 16;            // 马赛克块大小（物理像素）
    int blurRadius = 14;           // 模糊半径（物理像素）
    int cornerRadius = 0;          // >0 = 圆角矩形标注（专业版对齐项 v2.4）
    bool dashed = false;           // 虚线/点线样式（专业版对齐项 v2.5）

    // ---- 专业版对齐项（v2.6 / v2.5）----

    /// 椭圆形状：马克笔/马赛克/高斯模糊/橡皮擦以椭圆而非矩形工作。
    bool ellipseShape = false;
    /// 双向箭头：两端都画箭头头（专业版 v2.5）。
    bool arrowBothEnds = false;
    /// 反向箭头：箭头头画在起点而非终点（专业版 v2.5）。
    bool arrowReverse = false;
    /// 放大镜标注的放大倍数（专业版 v2.6）。
    int magnifierZoom = 2;
};

struct Annotation {
    AnnotationTool tool = AnnotationTool::Rect;
    AnnotationStyle style;
    std::vector<Point> points;  // 物理像素坐标；含义随工具而定
    std::string text;           // Text 用

    /// 点集描述的区域（用于马赛克/模糊的区域参数与命中测试）。
    Rect boundingRect() const;
};

class AnnotationDocument {
public:
    /// 添加一条标注；redo 栈会被清空（标准编辑器语义）。
    /// 空标注（点不足）会被拒绝并返回 false。
    bool add(Annotation annotation);

    bool undo();  // 撤销最后一条（移入 redo 栈）
    bool redo();  // 重做

    bool canUndo() const { return !items_.empty(); }
    bool canRedo() const { return !undone_.empty(); }
    bool empty() const { return items_.empty(); }
    std::size_t size() const { return items_.size(); }

    const std::vector<Annotation>& items() const { return items_; }

    /// 按顺序遍历"参与像素处理的标注"（Mosaic/Blur），
    /// 供合成器应用像素效果。
    std::vector<const Annotation*> pixelOpItems() const;

    /// 下一个序号标注的标签（"1"、"2"…）。基于当前可见的序号数量，
    /// 因此撤销掉一个再画会重用那个数字 —— 与"画面上看得见几个"一致。
    std::string nextNumberLabel() const;

    /// 橡皮擦（M2 增补）：移除所有"区域与 region 相交"的标注，
    /// 返回被移除的数量。
    ///
    /// 语义选择说明：这里是**整条删除**而不是"擦掉标注的一部分"——
    /// 矢量标注（一条箭头、一段文字）被擦掉一半没有合理语义；
    /// Snipaste 的橡皮擦也是按"擦过即消"的整条语义工作的。
    /// 相交判定用标注的包围盒（简单、可预期；标注数量级很小）。
    std::size_t eraseIntersecting(const Rect& region);

    /// 按索引移除（供交互层做更精细的控制）。
    bool removeAt(std::size_t index);

    /// 平移指定标注（专业版对齐项 v2.8：自由选中 + 二次编辑的基础）。
    /// 平移所有点；返回是否执行。
    bool translateAt(std::size_t index, int dx, int dy);

    void clear();

private:
    std::vector<Annotation> items_;
    std::vector<Annotation> undone_;
};

} // namespace jie2::core
