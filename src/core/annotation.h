#pragma once

#include <cstdint>
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

    /// 文档分配的内部序号（撤销事务按它把"被永久擦除"的标注从历史里剔除）。
    /// 调用方不应手工设置 —— add() 会覆盖为文档的递增序号。
    std::uint64_t serial = 0;

    /// 点集描述的区域（用于马赛克/模糊的区域参数与命中测试）。
    Rect boundingRect() const;
};

/// 可旋转区域命中测试（专业版对齐项 v2.6：矩形/椭圆区域橡皮擦可旋转）。
///
/// 判断一个（可绕中心旋转的）矩形或椭圆区域是否与轴对齐包围盒相交。
///   - angleDeg 为旋转角（度，顺时针；0 = 轴对齐）；
///   - ellipseShape 为 true 时区域是椭圆（由 region 外接）。
///
/// 实现：轴对齐快路径（与"有重叠面积"的经典语义一致）+ 通用凸多边形
/// SAT（分离轴定理）路径（旋转矩形 4 边形、椭圆 24 边形近似）。
/// 退化包围盒（零宽/零高，如水平箭头）走多边形路径 —— 贴边即算命中，
/// 这修复了旧实现里"退化包围盒永远擦不掉"的隐性缺陷。
bool regionIntersectsBox(const Rect& region, int angleDeg, bool ellipseShape,
                         const Rect& box);

class AnnotationDocument {
public:
    /// 添加一条标注；后续（redo）历史会被清空（标准编辑器语义）。
    /// 空标注（点不足）会被拒绝并返回 false。
    bool add(Annotation annotation);

    /// 撤销 / 重做（整表快照：任意编辑都能逐步回退，含撤销掉的中途状态）。
    bool undo();
    bool redo();

    bool canUndo() const { return cursor_ > 0; }
    bool canRedo() const { return cursor_ + 1 < history_.size(); }
    bool empty() const { return items_.empty(); }
    std::size_t size() const { return items_.size(); }

    const std::vector<Annotation>& items() const { return items_; }

    /// 按顺序遍历"参与像素处理的标注"（Mosaic/Blur），
    /// 供合成器应用像素效果。
    std::vector<const Annotation*> pixelOpItems() const;

    /// 下一个序号标注的标签（"1"、"2"…）。基于当前可见的序号数量，
    /// 因此撤销掉一个再画会重用那个数字 —— 与"画面上看得见几个"一致。
    std::string nextNumberLabel() const;

    /// 橡皮擦：移除所有"区域与 region 相交"的标注，返回被移除的数量。
    ///
    /// 语义选择说明：这里是**整条删除**而不是"擦掉标注的一部分"——
    /// 矢量标注（一条箭头、一段文字）被擦掉一半没有合理语义；
    /// Snipaste 的橡皮擦也是按"擦过即消"的整条语义工作的。
    /// 相交判定用标注的包围盒 + 可旋转区域（见 regionIntersectsBox）。
    ///
    /// 快照撤销语义：默认擦除**可撤销**（Ctrl+Z 恢复）；
    /// 若 setEraserUndoable(false)（配置项《对橡皮擦禁用二次编辑》），
    /// 被擦除的标注会从整个历史里剔除 —— 无论怎么撤销/重做都不会复活，
    /// 而其它编辑的撤销/重做不受影响。
    std::size_t eraseIntersecting(const Rect& region, int angleDeg = 0,
                                  bool ellipseShape = false);

    /// 橡皮擦的删除是否可撤销（专业版对齐项 v2.6：可禁用二次编辑）。
    /// 默认 true（可恢复）——比旧行为更友好；置 false 得到"擦除即永久"。
    void setEraserUndoable(bool on) { eraserUndoable_ = on; }
    bool eraserUndoable() const { return eraserUndoable_; }

    /// 按索引移除（供交互层做更精细的控制）。
    bool removeAt(std::size_t index);

    /// 平移指定标注（专业版对齐项 v2.8：自由选中 + 二次编辑的基础）。
    /// 平移所有点；返回是否执行。
    bool translateAt(std::size_t index, int dx, int dy);

    /// 用给定点集覆盖某标注（专业版对齐项 v2.3 / v2.11：拖动移动 / 角柄缩放 /
    /// Shift+滚轮缩放）。交互层手势从"手势起点的点集快照"重新推导每一帧，
    /// 避免逐帧累积误差：
    ///   - record=false：不入历史（拖拽中的实时刷新），收尾调用 commitPending()；
    ///   - record=true：立即入历史（如 Shift+滚轮的单步缩放）。
    bool replacePoints(std::size_t index, std::vector<Point> points,
                       bool record = true);

    /// 把"未入历史的实时修改"一次性补进历史（拖拽收尾时调用；无挂起则空操作）。
    void commitPending();

    /// 清空全部标注（历史重置为单条空状态）。
    void clear();

private:
    /// 记录一次状态（调用前 items_ 已被修改）。
    void commit();

    std::vector<Annotation> items_;
    /// 整表快照历史；history_[cursor_] 恒等于 items_。
    std::vector<std::vector<Annotation>> history_{std::vector<Annotation>{}};
    std::size_t cursor_ = 0;
    std::uint64_t nextSerial_ = 1;
    bool eraserUndoable_ = true;
    /// 存在"未入历史"的实时修改（拖拽手势中 replacePoints(record=false)）。
    bool pending_ = false;

    /// 快照深度上限：标注数量级很小（几十条），128 个状态的内存开销可忽略，
    /// 又足以覆盖整段编辑；超出时丢弃最旧状态。
    static constexpr std::size_t kMaxHistoryStates = 128;
};

} // namespace jie2::core
