#pragma once

#include <QDialog>
#include <QImage>
#include <QPixmap>
#include <QRect>
#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

#include "core/annotation.h"
#include "core/snap.h"

class QLineEdit;
class QScreen;

namespace jie2::ui {

/// 截图选择遮罩（M1）。
///
/// 交互（对齐 Snipaste 的截图流程，评估报告 1.5）：
///   - 展示冻结的屏幕画面；拖拽框选，选区外变暗
///   - 实时显示选区的**物理像素**尺寸与起点
///   - 放大镜跟随光标（4 倍物理像素），显示光标处像素颜色；按 C 复制色值
///   - Esc / 右键取消；回车 / 双击确认
///   - `,` / `.` 回放截图历史（专业版对齐项"截图记录回放"）
///   - F5 刷新截图（重新抓屏，清空当前选区）
///
/// 坐标约定：
///   - 控件内部与返回的 selection 都用**逻辑点**（Qt 控件坐标，原点为本屏左上角）。
///   - 返回的 pixelRegion 是**源图内**的物理像素裁剪区，由本类算好，
///     调用方直接按 sourcePath 取源图裁剪即可 —— 不用再关心缩放换算。
///   - 冻结画面的 QPixmap 不设 devicePixelRatio，保持物理像素坐标，
///     使放大镜取源与内核裁剪使用同一套坐标。
class SelectionOverlay : public QDialog {
    Q_OBJECT

public:
    struct Outcome {
        bool accepted = false;
        QRect selection;      // 逻辑点，相对本屏左上角（交互/展示用途）
        QString sourcePath;   // 空 = 当前画面；非空 = 历史截图文件
        QRect pixelRegion;    // 源图内物理像素裁剪区（已按源图边界裁剪）
        // sourcePath 为空时的源图。必须由本类带出而不是让调用方用最初传入的
        // 抓屏结果 —— F5 刷新后画面已更新，用旧图裁剪会得到错误内容。
        QImage currentImage;
        // 最终输出：源图 → 标注合成（矢量绘制 + 马赛克/模糊）→ 按选区裁剪。
        // 调用方直接保存这张图即可，不需要再做任何换算或合成。
        QImage outputImage;
    };

    /// 重新抓屏回调（F5 用）。返回空 QPixmap 表示失败。
    using RecaptureFn = std::function<QPixmap()>;

    /// screen 决定遮罩覆盖哪块屏幕；传 nullptr 时用主屏。
    /// （显式关联 QScreen 而不是只给 QRect：macOS 上无边框窗口需要
    ///  windowHandle()->setScreen() 才会稳定落到目标屏。）
    /// historyPaths 为截图历史（旧的在前、新的在后），用于 , / . 回放。
    /// recapture 为空时 F5 无效。
    /// snapCandidates 为可吸附的窗口矩形（本屏逻辑坐标），空则不启用吸附。
    SelectionOverlay(QPixmap frozen, double scaleFactor, QScreen* screen,
                     QStringList historyPaths = {},
                     RecaptureFn recapture = {},
                     std::vector<core::Rect> snapCandidates = {},
                     int paletteSize = 6,
                     QWidget* parent = nullptr);

    /// 模态运行，返回用户选择结果。
    /// accepted 为 false 表示用户取消（静默，不是错误）。
    /// paletteSize 为调色板可用颜色数量（专业版对齐项，可配置）。
    static Outcome run(const QPixmap& frozen, double scaleFactor, QScreen* screen,
                       QStringList historyPaths = {},
                       RecaptureFn recapture = {},
                       std::vector<core::Rect> snapCandidates = {},
                       int paletteSize = 6);

    const Outcome& outcome() const { return outcome_; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void showEvent(QShowEvent* event) override;
    /// 拦截文字输入框的 Esc（取消输入而不关闭整个遮罩）。
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// 工具条按钮的语义。
    enum class ToolbarButton { None, Save, Cancel, Tool, Undo, Redo, Color };

    /// 工具条按钮描述。顺序即显示顺序。
    struct ToolbarItem {
        ToolbarButton action = ToolbarButton::None;
        core::AnnotationTool tool = core::AnnotationTool::Rect;
        const char* label = "";
        int width = 52;
    };
    /// 工具条按钮表（固定顺序与宽度，绘制与命中共用）。
    static const std::vector<ToolbarItem>& toolbarItems();

    QRect currentSelection() const;
    void commit(bool accepted);
    void paintHint(QPainter& painter) const;
    void paintSelectionChrome(QPainter& painter, const QRect& selection) const;
    void paintMagnifier(QPainter& painter);
    /// 选区确定后显示的工具条：绘制与命中检测共用这里的几何，
    /// 避免"画出来的位置"和"可点的位置"不一致。
    void paintToolbar(QPainter& painter, const QRect& selection) const;
    QRect toolbarRect(const QRect& selection) const;
    QRect toolbarButtonRect(const QRect& selection, int index) const;
    ToolbarButton toolbarHitTest(const QPoint& pos, int* index) const;
    QColor pixelColorAt(const QPoint& logicalPos) const;

    // ---- 标注（M2）----

    /// 逻辑点 → 源图物理像素坐标。
    QPoint imagePos(const QPoint& logical) const;
    /// 当前工具的颜色（按调色板索引取）。
    core::Rgba currentColor() const;
    /// 重建"带像素效果的基础画面"（马赛克/模糊烘焙进底图，矢量实时绘制）。
    void rebuildComposed();
    /// 生成最终输出：合成全部标注 → 按选区裁剪。
    QImage buildFinalImage() const;
    /// 绘制全部标注（含草稿预览）。painter 已设为逻辑坐标系。
    void paintAnnotations(QPainter& painter, const QRect& selection) const;
    /// 撤销/重做并重建底图。
    void performUndo();
    void performRedo();

    // ---- 文字与序号（M2 增补）----

    /// 在指定位置弹出内联文字输入框（回车提交、Esc 取消）。
    void startTextInput(const QPoint& logicalPos);
    /// 提交/取消文字输入。
    void commitTextInput();
    void cancelTextInput();
    /// 在指定位置放置一个序号标注（自动取下一个编号）。
    void placeNumberMarker(const QPoint& logicalPos);

    QLineEdit* textInput_ = nullptr;
    QPoint textAnchorImage_;  // 文字输入的源图像素锚点

    /// 切换/设置冻结画面。isCurrentScreen 为 true 时按"铺满屏幕"建立映射，
    /// 否则（历史图）等比缩放到适应屏幕、居中显示。
    void setFrozenImage(const QImage& image, bool isCurrentScreen);
    /// 回放历史：index < 0 回到当前抓屏画面；否则取 historyPaths_[index]。
    void switchHistory(int index);
    /// 逻辑选区 → 源图内物理像素裁剪区（含边界裁剪）。
    QRect selectionToPixelRegion(const QRect& logical) const;
    /// 刷新来源（F5）：重新抓屏并重置为当前屏画面。
    void recapture();
    /// 更新 toast 并在 1.8s 后自动消失。
    void showToast(const QString& text);

    QPixmap frozen_;
    QImage frozenImage_;   // 取色用（避免每次 toImage()）
    QImage screenImage_;   // 当前屏原始画面备份（回放后用于切回）
    double scaleFactor_ = 1.0;   // 当前屏画面：物理像素 / 逻辑点
    QRect screenGeometry_;
    QScreen* screen_ = nullptr;

    // 冻结画面在窗口逻辑坐标里的位置与"逻辑点 → 画面像素"比例。
    // 当前屏画面 = 铺满屏幕（scaleFactor_）；历史图 = 等比适配+居中。
    QRect frozenLogicalRect_;
    double frozenPixelScale_ = 1.0;

    QStringList historyPaths_;  // 旧的在前
    int historyIndex_ = -1;     // -1 = 当前抓屏画面；>=0 = 历史索引
    RecaptureFn recapture_;

    bool dragging_ = false;
    bool hasSelection_ = false;
    QPoint dragOrigin_;
    QPoint dragCurrent_;
    QPoint cursor_;
    bool hasCursor_ = false;
    Outcome outcome_;
    QString toast_;  // 瞬时提示（按 C 复制色值 / 回放位置 / 刷新结果）

    // ---- 窗口吸附（M1）----
    std::vector<core::Rect> snapCandidates_;  // 本屏逻辑坐标
    QRect selectionRect_;   // 结算后的选区（非拖拽状态使用）
    bool snapActive_ = false;
    core::SnapResult lastSnap_;

    // ---- 标注（M2）----
    core::AnnotationDocument annotations_;
    core::AnnotationTool currentTool_ = core::AnnotationTool::Rect;
    int currentColorIndex_ = 0;
    int currentWidth_ = 3;          // 线宽（物理像素）
    bool dashed_ = false;           // 虚线样式（D 键切换）
    int cornerRadius_ = 0;          // 圆角矩形半径（R 键循环 0/8/16/24）
    bool ellipseShape_ = false;     // 椭圆形状（O 键；马克笔/马赛克/模糊）
    bool arrowBothEnds_ = false;    // 双向箭头（B 键）
    bool arrowReverse_ = false;     // 反向箭头（Shift+B 键）
    int paletteSize_ = 6;           // 调色板颜色数量（专业版对齐项，可配置）
    bool drawing_ = false;          // 正在绘制一条标注
    bool erasing_ = false;          // 橡皮擦拖拽中
    core::Annotation draft_;        // 绘制中的草稿
    QImage composedBase_;           // 底图 + 已烘焙的像素效果（预览用）
    QPixmap displayPixmap_;         // composedBase_ 的 QPixmap 缓存
    bool composedDirty_ = true;     // 需要重建 composedBase_
};

} // namespace jie2::ui
