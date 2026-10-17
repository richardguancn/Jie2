#pragma once

#include <QImage>
#include <QPoint>
#include <QString>  // 接口里有 QString 参数（头文件必须自包含）
#include <QWidget>

namespace jie2::ui {

/// 「截2」贴图窗口（M4）—— 把图像作为置顶窗口钉在屏幕上。
///
/// 对齐 Snipaste 的贴图语义（评估报告 1.5）：
///   - 滚轮 / `+` `-`：缩放
///   - `1` `2`：旋转（对 90° 的整数倍做精确变换，非插值）
///   - `3` `4`：水平 / 垂直翻转
///   - `Ctrl+滚轮` / `Ctrl+加号减号`：透明度
///   - `6`：反色
///   - 中键单击：重置为 100% 大小与 100% 不透明
///   - `Shift+双击`：缩略图模式（缩小到固定上限，保留原始尺寸信息）
///   - 鼠标穿透：由托盘热键触发（切换光标下贴图的穿透状态）
///   - 裸双击：以点击位置为中心切换方形局部；再次双击恢复
///   - `Esc`：关闭
///   - 拖动：按住左键拖动窗口
///
/// 坐标与缩放：内部保存原图 + 变换标志 + 缩放因子，在需要时重算显示图。
/// 不做平滑插值放大的假设 —— 缩放用 Qt 的平滑变换，因为贴图是长期停留的
/// 内容，画质优先于速度（与截图遮罩的实时拖拽不同）。
class PinWindow : public QWidget {
    Q_OBJECT

public:
    explicit PinWindow(QImage image, QWidget* parent = nullptr);

    /// 贴图分组（专业版对齐项）：同组内一起显示/隐藏。
    /// 组号由托盘控制器在创建时分配，本类只保存与查询。
    void setGroup(int group) { group_ = group; }
    int group() const { return group_; }

    /// 选中态（专业版对齐项 v2.4：Ctrl+A 全选贴图）。
    /// 选中的贴图会画高亮边框；拖动其中一张时其余选中的一起移动。
    void setSelected(bool on) { selected_ = on; update(); }
    bool isSelected() const { return selected_; }

    /// 消隐动作（专业版对齐项 v2.8：可自定义贴图 Esc 和 Shift+Esc 的功能）。
    /// Close = 关闭（从列表移除）；Hide = 隐藏（保留，可由托盘"显示全部"恢复）。
    enum class DismissAction { Close, Hide };
    void setDismissActions(DismissAction esc, DismissAction shiftEsc) {
        escAction_ = esc;
        shiftEscAction_ = shiftEsc;
    }

    /// 缩略图修饰键（专业版对齐项 v2.11：可自定义缩略图快捷键）。
    /// 默认 Shift+双击；可配置为 Ctrl+双击 / Alt+双击。
    void setThumbnailModifier(Qt::KeyboardModifier modifier) {
        thumbnailModifier_ = modifier;
    }

    /// 从剪贴板内容创建：图像 / 文本 / 颜色值 / 图像文件路径。
    /// 失败时返回 nullptr 并通过 error 说明原因（不静默）。
    /// 调用方负责 delete。
    static PinWindow* fromClipboard(QString* error);

    /// 当前显示的原图（不含变换）。供"复制回剪贴板"使用。
    const QImage& sourceImage() const { return source_; }

    /// 供测试与批量操作。
    double scale() const { return scale_; }
    double opacityPercent() const { return opacityPercent_; }
    bool inverted() const { return inverted_; }
    void setInverted(bool on);
    void toggleMouseThrough();
    bool mouseThrough() const { return mouseThrough_; }

    /// 重置为 100% 大小与不透明（中键语义）。
    void resetTo100();
    /// 缩略图模式（Shift+双击语义）：缩到固定的最大边长。
    void toggleThumbnail();

    /// 把当前显示内容（含变换与反色）复制到剪贴板。
    void copyToClipboard();

    // ---- 变换（按键与自动化测试共用的入口，避免两处实现）----

    /// 顺时针旋转 90°（按键 `2`；`1` 为逆时针，见 rotateCounterClockwise）。
    void rotateClockwise();
    void rotateCounterClockwise();
    void flipHorizontal();
    void flipVertical();
    void toggleInvert();

    /// 供测试与导出：当前显示图（含旋转/翻转/反色，不含缩放与透明度）。
    QImage displayedImage() const { return transformed(); }

    /// 把当前显示图保存到指定路径（按扩展名选格式）。
    /// 批量保存（专业版对齐项 v2.5）与脚本导出共用此方法。
    bool saveTo(const QString& path) const;

signals:
    void closed();

    /// Ctrl+A（在任一贴图上按下）：请求托盘控制器全选/取消全选所有贴图。
    void selectAllRequested();
    /// 拖动时报告本次位移（供托盘控制器联动其余选中贴图）。
    void movedBy(const QPoint& delta);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    /// 统一在 closeEvent 发 closed()：所有关闭路径（Esc / 双击 /
    /// 程序化 close()）都会经过这里，避免漏发信号导致上层列表残留。
    void closeEvent(QCloseEvent* event) override;

private:
    friend class UiAlignmentTest;
    /// 按 原图 + 变换 + 缩放 重算显示图与窗口大小，并重绘。
    void rebuild();
    /// 当前显示图（含旋转/翻转/反色，不含缩放）。
    QImage transformed() const;
    void applyScaleFactor(double factor);
    void toggleDetailAt(const QPoint& localPos);
    QRectF detailSourceRect() const;
    QPointF sourcePointAt(const QPoint& localPos) const;
    void applyOpacityDelta(double delta);

    QImage source_;          // 原图（不变）
    int group_ = 0;          // 所属贴图分组（0 基）
    int rotation_ = 0;       // 0/90/180/270（顺时针）
    bool flipH_ = false;
    bool flipV_ = false;
    bool inverted_ = false;
    bool thumbnail_ = false;
    bool detailMode_ = false;
    bool detailRestoreThumbnail_ = false;
    QPointF detailCenter_;
    double detailScale_ = 1.0;  // 当前一个原图像素占多少逻辑点
    static constexpr int kDetailSide = 54;
    bool selected_ = false;  // 选中态（Ctrl+A 全选后拖动联动）
    DismissAction escAction_ = DismissAction::Close;         // Esc 默认关闭
    DismissAction shiftEscAction_ = DismissAction::Hide;     // Shift+Esc 默认隐藏
    Qt::KeyboardModifier thumbnailModifier_ = Qt::ShiftModifier;  // 缩略图修饰键
    double scale_ = 1.0;
    double opacityPercent_ = 100.0;
    bool mouseThrough_ = false;

    bool dragging_ = false;
    QPoint dragOffset_;
};

} // namespace jie2::ui
