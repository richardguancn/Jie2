#pragma once

#include <QImage>
#include <QString>  // copyRequested 的信号参数（缺它会引发级联解析错误）
#include <QWidget>

#include <vector>

#include "core/capture_history.h"

class QTimer;

namespace jie2::ui {

/// 「衣架」（M3）—— 合并 tendedero 的核心功能。
///
/// 形态（样式 1:1 对齐 tendedero，见 PeggedView.swift / LineView.swift）：
/// 贴在屏幕顶部的**透明**面板上挂着一条会下垂的抛物线晾衣绳，
/// 每张截图是一张带玻璃边框的卡片，由一枚铝质衣夹夹在绳上，
/// 每张卡片带一个固定的随机倾斜角。平时收起；指针触到屏幕顶边时
/// 滑下来，把最近的截图一张张挂在线上。
///
/// 交互（对齐 tendedero 的手势语义，按 Linux/macOS 的差异做了等价替换）：
///   - 指针停在屏幕顶边 → **滑入动画**展开；指针离开 → **滑出动画**收起
///   - 单击卡片 → 复制图像到剪贴板
///   - **把卡片拖出去** → 交给目标程序（拖到文件管理器 = 保存文件；
///     接受 MoveAction 时该卡片从衣架取下）
///   - 右键卡片 → 从衣架取下（文件保留，只离开线）
///   - Esc → 收起
///
/// 与 tendedero 的差异（评估报告 4.3）：
///   - macOS 菜单栏 → Linux/跨平台用"屏幕顶边热区"替代
///   - 不接管系统截图设置（Linux 无此概念）
///   - 倾斜角由路径哈希确定（tendedero 是创建时随机；哈希让同一文件
///     每次挂载角度一致，也便于测试）
///
/// 约束：本类是 UI 层组件，数据来自 core::CaptureHistory，
/// 不直接触碰系统 API（连剪贴板都经 QClipboard）。
class Clothesline : public QWidget {
    Q_OBJECT

public:
    explicit Clothesline(QWidget* parent = nullptr);

    /// 载入/刷新挂载项（按时间从旧到新；显示时新的在右）。
    void setEntries(std::vector<core::HistoryEntry> entries);

    /// 启停"顶边停留展开"的轮询（截图进行中应暂停，避免遮挡）。
    void setWatchEnabled(bool enabled);

    /// 立即收起（截图前调用，避免衣架被拍进截图）。
    void hideNow();

    /// 供测试查询。
    int itemCount() const { return static_cast<int>(entries_.size()); }
    bool isRevealed() const { return revealed_; }

    /// 主题（专业版对齐项 v2.6：主题色跟随系统主题）。
    /// tendedero 的玻璃材质随系统自动深浅；这里体现为卡片玻璃底
    /// 与提示气泡的深浅两套。绳子按 tendedero 的设计保持中性灰，
    /// 在深浅壁纸上都可读。
    void setDarkTheme(bool dark);

signals:
    /// 用户点击卡片，**请求**把该图复制到剪贴板。
    ///
    /// 注意：剪贴板由托盘控制器写入，本窗口不直接写 —— 因为写入会
    /// 同步触发剪贴板监听回调，而那个回调会重建本窗口的 entries_，
    /// 使调用栈里持有的引用失效（实测导致段错误）。把动作交给上层，
    /// 由上层在写入前后屏蔽自己的监听器。
    void copyRequested(const QString& path);
    /// 用户右键取下（或拖出被移走）某张（托盘控制器据此更新历史）。
    void entryDropped(const QString& path);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    struct Item {
        core::HistoryEntry entry;
        QImage thumbnail;  // 懒加载
        double tilt = 0;   // 固定倾斜角（度），由路径哈希决定
    };

    void tick();               // 轮询指针位置
    /// 展开到指定屏幕区域（area 为屏幕几何，来自 tick 的指针所在屏）。
    void reveal(const QRect& area);
    void conceal();
    void reloadThumbnails();

    // ---- 布局（tendedero LineView.Layout 的移植）----
    double ropeSag() const;             // 绳子下垂量：min(30, 宽 × 0.018)
    double ropeYAt(double x) const;     // 抛物线：ropeTop + 4·sag·f·(1-f)
    double cardCenterX(int index) const;  // 居中排布，间距受限时压缩
    QSizeF photoSize(int index) const;    // 缩略图等比适配 136×104
    QRectF cardRectF(int index) const;    // 未旋转的卡片边框矩形
    QPointF cardAnchor(int index) const;  // 悬挂点（衣夹顶中），倾斜旋转锚点
    int itemAt(const QPoint& pos) const;  // 命中测试（做倾斜逆变换）

    // ---- 绘制 ----
    void drawRope(QPainter& painter) const;
    void drawClothespin(QPainter& painter, const QPointF& topCenter) const;
    void drawCardBody(QPainter& painter, int index) const;
    void drawEmptyHint(QPainter& painter) const;

    /// 滑入/滑出动画（tendedero 的核心观感）。
    /// 目标 y = area.top()（展开）或 area.top() - kPanelHeight（收起）。
    /// 动画结束时收起才真正 hide()；展开期间窗口必须已 show()。
    void animateTo(int targetY, bool hideAtEnd);
    /// 面板所在的屏幕区域（动画与定位共用）。
    QRect panelScreenArea() const;
    /// 把某张卡片拖给外部程序（文件管理器等）。
    /// 返回 true 表示对方以 MoveAction 接受 —— 此时卡片从衣架取下。
    bool startDragOut(int index);

    std::vector<Item> entries_;
    bool revealed_ = false;
    bool watchEnabled_ = false;
    int hoverIndex_ = -1;
    QTimer* pollTimer_ = nullptr;

    // ---- 滑入滑出动画状态 ----
    // 动画由"窗口沿屏幕上沿移动"实现（定时器驱动）。
    // 注：Wayland 下本窗口不创建（走 jie2-line 子进程），故只有这一种形态。
    QTimer* animTimer_ = nullptr;
    int animTargetY_ = 0;
    bool animHideAtEnd_ = false;
    bool animating_ = false;

    // ---- 拖出状态 ----
    bool pressedOnCard_ = false;   // 左键按在卡片上（等待判定是点击还是拖拽）
    QPoint pressPos_;
    int pressedIndex_ = -1;

    // ---- 主题色（v2.6）----
    // tendedero 的绳子与衣夹是中性色（深浅壁纸通用），主题只体现在
    // 玻璃卡片底与提示气泡上。
    struct StripColors {
        QColor cardGlass;  // 卡片玻璃底（半透明）
        QColor hintBg;     // 空态提示气泡底
        QColor hintText;   // 空态提示文案
        QColor failText;   // "读取失败"文案
    };
    StripColors colors_;
    bool darkTheme_ = true;

    // 视觉参数全部对齐 tendedero（LineView.swift / PeggedView.swift）。
    static constexpr int kPanelHeight = 210;  // photoMaxHeight + 106
    static constexpr int kPhotoMaxWidth = 136;   // cardWidth(150) - 14
    static constexpr int kPhotoMaxHeight = 104;
    static constexpr int kCardSpacing = 174;  // cardWidth + 24
    static constexpr double kRopeTop = 10;
    static constexpr double kPinAbove = 9.5;  // 衣夹顶高出绳子的距离
    static constexpr double kClipWidth = 9;
    static constexpr double kClipHeight = 26;
    static constexpr double kCardBelowClipTop = 14;  // 衣夹与卡片重叠 12
    static constexpr int kFrameInset = 4;     // 玻璃边框厚度
    static constexpr double kFrameRadius = 16;
    static constexpr double kPhotoRadius = 12;  // 同心圆角：16 - 4

    static constexpr int kEdgeZone = 3;       // 触发展开的顶边厚度
    static constexpr int kMaxItems = 12;      // 最多展示数量
    static constexpr int kAnimStepMs = 12;    // 动画步进间隔
    static constexpr int kAnimPxPerStep = 14; // 每步位移（≈ 1100px/s，观感干脆）
    static constexpr int kDragStartDistance = 8;  // 超过此位移视为拖出而非点击
};

} // namespace jie2::ui
