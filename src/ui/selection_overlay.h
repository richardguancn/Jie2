#pragma once

#include <QDialog>
#include <QImage>
#include <QPixmap>
#include <QRect>
#include <QString>
#include <QStringList>

#include <array>
#include <functional>
#include <vector>

#include "core/annotation.h"
#include "core/ocr.h"
#include "core/shortcut_map.h"
#include "core/snap.h"

class QLineEdit;
class QScreen;
class QFrame;
class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace jie2::ui {

/// 重新抓屏回调（F5 用）。返回空 QPixmap 表示失败。
using OverlayRecaptureFn = std::function<QPixmap()>;

/// 遮罩构造参数集（会随专业版对齐项继续增长，独立成结构体避免参数列表失控）。
/// 定义在类外：作为默认实参使用时不受"类内默认成员初始化器"限制。
struct SelectionOverlayOptions {
    QStringList historyPaths;   // 截图历史（旧的在前），, / . 回放
    OverlayRecaptureFn recapture;  // 空则 F5 无效
    std::vector<core::Rect> snapCandidates;  // 可吸附窗口（本屏逻辑坐标）
    int paletteSize = 6;        // 调色板可用颜色数量（v2.4 对齐项）
    core::ShortcutMap shortcuts = core::ShortcutMap::defaults();  // v2.11.3
    bool selectImage = false;  // 衣架标注：居中显示并选中整张图片
    bool darkTheme = true;      // 主题（v2.6：跟随系统）
    bool eraserUndoable = true; // 橡皮擦删除可撤销（v2.6：可禁用二次编辑）
    core::OcrConfig ocrConfig;  // 与设置中的文字识别服务共用配置
    /// 把遮罩窗口提升到能盖住全屏应用 / 远程桌面客户端的层级，
    /// 并让它拿到键盘焦点。参数是原生窗口句柄，返回是否已施加提升动作。
    /// 由调用方注入（UI 层不能直接依赖 platform 层）。
    std::function<bool(void* nativeWindow)> promoteWindow;
};

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
    enum class Action { Finish, SaveFile, Copy, Pin };
    struct Outcome {
        Action action = Action::Finish;
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
    using RecaptureFn = OverlayRecaptureFn;

    /// 构造参数集（见命名空间级 SelectionOverlayOptions）。
    using Options = SelectionOverlayOptions;

    /// screen 决定遮罩覆盖哪块屏幕；传 nullptr 时用主屏。
    /// （显式关联 QScreen 而不是只给 QRect：macOS 上无边框窗口需要
    ///  windowHandle()->setScreen() 才会稳定落到目标屏。）
    SelectionOverlay(QPixmap frozen, double scaleFactor, QScreen* screen,
                     Options options = {},
                     QWidget* parent = nullptr);

    /// 模态运行，返回用户选择结果。
    /// accepted 为 false 表示用户取消（静默，不是错误）。
    static Outcome run(const QPixmap& frozen, double scaleFactor, QScreen* screen,
                       Options options = {});

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
    /// 交给平台层把遮罩提到可覆盖全屏/远程桌面窗口的层级并抢焦点。
    void promoteToFront();
    /// 拦截文字输入框的 Esc（取消输入而不关闭整个遮罩）。
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    friend class UiAlignmentTest;
    /// 工具条按钮的语义。
    enum class ToolbarButton { None, Save, Copy, Pin, Cancel, Tool, Undo, Redo, Color, Ocr };

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
    void advanceToolColor();
    void showToolMenu(int index, const QPoint& globalPos);
    std::array<QPoint, 8> cropHandles() const;
    int cropHandleAt(const QPoint& pos) const;
    /// 手柄序号 → 悬停/拖动时的调整光标（0 TL, 1 上中, 2 TR, 3 右中, 4 BR, …）。
    static Qt::CursorShape cursorForCropHandle(int handle);
    void updateCropGesture(const QPoint& pos);
    QRect toolbarRect(const QRect& selection) const;
    QRect toolbarButtonRect(const QRect& selection, int index) const;
    ToolbarButton toolbarHitTest(const QPoint& pos, int* index) const;
    void startOcr();
    void ensureOcrPanel();
    void positionOcrPanel();
    void invalidateOcr();
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
    /// 下一个序号值（已有序号数 + 1 + numberOffset_，下限 1）。
    int nextNumberValue() const;
    /// 提交/取消文字输入。
    void commitTextInput();
    void cancelTextInput();
    /// 在指定位置放置一个序号标注（自动取下一个编号）。
    void placeNumberMarker(const QPoint& logicalPos);

    /// 自由选中（专业版对齐项 v2.8）：Ctrl+点击命中已有标注则选中它；
    /// 未命中则取消选中。选中后可 Delete 删除、方向键微移。
    void handleSelectClick(const QPoint& logicalPos);

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
    /// 窗口层级提升回调（平台层实现，见 SelectionOverlayOptions）。
    std::function<bool(void*)> promoteWindow_;

    bool dragging_ = false;
    bool hasSelection_ = false;
    QPoint dragOrigin_;
    QPoint dragCurrent_;
    QPoint cursor_;
    bool hasCursor_ = false;
    Outcome outcome_;
    int toolbarHover_ = -1;
    bool toolChosen_ = false;
    core::OcrConfig ocrConfig_;
    bool ocrBusy_ = false;
    bool ocrInvalidated_ = false;
    QRect ocrSelection_;
    QFrame* ocrPanel_ = nullptr;
    QLabel* ocrStatus_ = nullptr;
    QPlainTextEdit* ocrText_ = nullptr;
    QPushButton* ocrCopy_ = nullptr;
    QPushButton* ocrRetry_ = nullptr;
    int cropHandle_ = -1;
    bool movingCrop_ = false;
    QPoint cropPress_;
    QRect cropStart_;
    QString toast_;  // 瞬时提示（按 C 复制色值 / 回放位置 / 刷新结果）

    // ---- 窗口吸附（M1）----
    std::vector<core::Rect> snapCandidates_;  // 本屏逻辑坐标
    QRect selectionRect_;   // 结算后的选区（非拖拽状态使用）
    bool snapActive_ = false;
    core::SnapResult lastSnap_;

    // ---- 标注（M2）----
    core::AnnotationDocument annotations_;
    core::AnnotationTool currentTool_ = core::AnnotationTool::Rect;
    /// 调色板索引：**每个工具独立记忆**（专业版对齐项 v2.4：
    /// "为不同标注形状分别记忆颜色"）—— 切换工具时各自的颜色不变。
    /// 按 AnnotationTool 的枚举值索引（当前 11 个工具；数组略大留余量）。
    /// 初值：马克笔默认黄（1），其余默认红（0）。
    int colorIndexByTool_[16] = {0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    int currentWidth_ = 3;          // 线宽（物理像素）
    bool dashed_ = false;           // 虚线样式（快捷键可配）
    int cornerRadius_ = 0;          // 圆角矩形半径（快捷键循环 0/8/16/24）
    bool ellipseShape_ = false;     // 椭圆形状（快捷键；马克笔/马赛克/模糊/橡皮擦）
    bool arrowBothEnds_ = false;    // 双向箭头
    bool arrowReverse_ = false;     // 反向箭头
    int paletteSize_ = 6;           // 调色板颜色数量（专业版对齐项，可配置）
    core::ShortcutMap shortcuts_;   // 应用内快捷键映射（v2.11.3 可配置）
    int eraserAngle_ = 0;           // 橡皮擦区域旋转角（v2.6：Shift+] / Shift+[）
    /// 序号偏移（v2.5.2："第一个数字可大于 1；+/- 调整序号"）。
    /// 下一个序号 = 已有序号数 + 1 + numberOffset_（下限 1）。
    int numberOffset_ = 0;
    bool drawing_ = false;          // 正在绘制一条标注
    bool erasing_ = false;          // 橡皮擦拖拽中
    int selectedIndex_ = -1;        // 自由选中（v2.8）：当前选中的标注索引（-1=无）

    // ---- 选中标注的移动 / 缩放手势（专业版对齐项 v2.3 / v2.11）----
    //
    // v2.3："中心点缩放（按住 Ctrl）；等比例缩放（按住 Shift）"——
    //      拖动选中矩形/椭圆的四角手柄进行缩放，修饰键实时生效。
    // v2.11："选中矩形/椭圆后 Shift+滚轮缩放"（见 wheelEvent）。
    // 手势期间用 replacePoints(record=false) 实时刷新，收尾 commitPending()
    // 一次性入历史（中间帧不污染撤销栈）。
    bool movingSelection_ = false;      // 拖动移动中
    bool resizingSelection_ = false;    // 角柄缩放中
    QPoint gestureStartLogical_;        // 移动手势起点（逻辑点）
    std::vector<core::Point> gestureBasePoints_;  // 手势起点的点集快照
    core::Rect gestureBaseBounds_;      // 手势起点的包围盒（图像像素）
    QPoint gestureGrabCorner_;          // 抓手的角柄（图像像素，box 角坐标）
    bool gestureCenterAnchor_ = false;  // 缩放基准：中心 or 对角
    bool gestureProportional_ = false;  // 缩放是否等比（Shift）

    /// 选中标注的四角手柄（图像像素坐标；与绘制一致 = 包围盒外扩 4px）。
    std::array<QPoint, 4> selectedHandles() const;
    /// 命中所选手柄（图像坐标比较，pad 随缩放换算；未选中返回 false）。
    bool hitSelectedHandle(const QPoint& logicalPos, QPoint* corner) const;
    void beginMoveSelection(const QPoint& logicalPos);
    void beginResizeSelection(const QPoint& corner);
    void updateMoveSelection(const QPoint& logicalPos);
    void updateResizeSelection(const QPoint& logicalPos,
                               Qt::KeyboardModifiers mods);
    void finishSelectionGesture();
    core::Annotation draft_;        // 绘制中的草稿
    QImage composedBase_;           // 底图 + 已烘焙的像素效果（预览用）
    QPixmap displayPixmap_;         // composedBase_ 的 QPixmap 缓存
    bool composedDirty_ = true;     // 需要重建 composedBase_

    // ---- 主题（v2.6：主题色跟随系统主题）----
    //
    // 只主题化"自绘界面色板"：工具条/提示条/toast 的底色与前景。
    // 刻意不动：选区变暗（黑色半透明，与底色无关）、吸附高亮与工具条
    // 强调色（accent 蓝在深浅两色上都保持可辨识）。
    struct ChromeColors {
        QColor panelBg;     // 工具条与提示条底板
        QColor buttonBg;    // 普通按钮
        QColor buttonFg;    // 普通按钮文字
        QColor disabledBg;  // 禁用按钮
        QColor disabledFg;
        QColor textFg;      // 提示条 / toast 文字
    };
    ChromeColors chrome_;
};

} // namespace jie2::ui
