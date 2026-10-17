#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QString>  // copyRequested 的信号参数（缺它会引发级联解析错误）
#include <QWidget>

#include <set>
#include <string>
#include <vector>

#include "core/capture_history.h"
#include "core/spring.h"

class QScreen;
class QMenu;
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
/// 动效（对齐 tendedero 的动画规格，数值实现见 core/spring.h）：
///   - 展开/收起：面板整体弹簧滑入（spring 0.42/0.72）/ easeIn 滑出（0.22s）
///   - 挂载掉落：新卡片从绳子上方 46pt 处弹簧落下（0.42/0.72），
///     同时带 16° 的欠阻尼摆动衰减到静止（k=46, c=2.6），透明度渐入
///   - 截图飞入：截图从被截取的区域起飞，沿 30pt 弧线收缩成卡片落位
///     （FlightOverlay，0.65s easeInOutCubic），落地后卡片 0.16s 淡入接管
///   - 风吹：展开时依次轻推卡片，之后每隔 2.2–4.5s 随机轻推一张
///     （0.3s easeOut 到摆角，再欠阻尼弹簧回稳）
///   - 自动探出：有新截图挂上时，面板自动展开亮相 2.5s 再收起
///   - 取下坠落：右键取下的卡片加速坠落（e = t³）、越转越歪、渐隐，
///     0.55s 后离场（FlightOverlay），布局在坠落结束后才收拢
///
/// 交互（对齐 tendedero 的手势语义，按 Linux/macOS 的差异做了等价替换）：
///   - 指针停在屏幕顶边 → 滑入展开；指针离开 → 滑出收起
///   - 单击卡片 → 复制图像到剪贴板
///   - 把卡片拖出去 → 交给目标程序（拖到文件管理器 = 保存文件；
///     接受 MoveAction 时该卡片从衣架取下）
///   - 双击卡片 → 打开图像；长按 0.45 秒 → 标注图像
///   - 右键卡片 → 操作菜单，支持取下（文件保留，只离开线）
///   - Esc → 收起
///
/// 约束：本类是 UI 层组件，数据来自 core::CaptureHistory，
/// 不直接触碰系统 API（连剪贴板都经 QClipboard）。
class Clothesline : public QWidget {
    Q_OBJECT

public:
    explicit Clothesline(QWidget* parent = nullptr);

    /// 载入/刷新挂载项（按时间从旧到新；显示时新的在右）。
    /// 首次见到的路径视为"新挂上的"：下次展开时播放挂载掉落动画，
    /// 且面板会自动探出亮相片刻（tendedero：Something new hanging
    /// shows itself for a moment）。
    void setEntries(std::vector<core::HistoryEntry> entries);

    /// 新截图飞入：从 globalRect（全局逻辑坐标）起飞，落到对应卡片的
    /// 最终位置。面板未展开时先探出承接。同时最多飞 2 张，多出的走
    /// 挂载掉落（tendedero 的 flightsInProgress 上限）。
    void flyCapture(const QString& path, const QRect& globalRect);

    /// 启停"顶边停留展开"的轮询（截图进行中应暂停，避免遮挡）。
    void setWatchEnabled(bool enabled);

    /// 立即收起（截图前调用，避免衣架被拍进截图）。
    void hideNow();

    /// 供测试查询。
    void takeDownPath(const QString& path) { takeDown(indexOfPath(path)); }
    int itemCount() const { return static_cast<int>(entries_.size()); }
    bool isRevealed() const { return revealed_; }
    /// 供离屏渲染测试驱动动效（生产路径走 tick 轮询触发）。
    void revealForTest(const QRect& area) { reveal(area); }
    void nudgeForTest(int index, double degrees) { nudge(index, degrees); }

    /// 主题（专业版对齐项 v2.6：主题色跟随系统主题）。
    void setDarkTheme(bool dark);
    /// 在复制成功的卡片下方显示短暂反馈，按路径跟随历史刷新。
    void showCopied(const QString& path);
    void setManagedDirectory(const QString& path) { managedDirectory_ = path; }

signals:
    /// 用户点击卡片，**请求**把该图复制到剪贴板。
    ///
    /// 注意：剪贴板由托盘控制器写入，本窗口不直接写 —— 因为写入会
    /// 同步触发剪贴板监听回调，而那个回调会重建本窗口的 entries_，
    /// 使调用栈里持有的引用失效（实测导致段错误）。把动作交给上层，
    /// 由上层在写入前后屏蔽自己的监听器。
    void copyRequested(const QString& path);
    /// 用户右键取下（或拖出被移走）某张（托盘控制器据此更新历史）。
    /// 取下动画（0.55s 坠落 + 0.6s 布局保持）结束后才发出。
    void entryDropped(const QString& path);
    void openRequested(const QString& path);
    void annotateRequested(const QString& path);
    void revealRequested(const QString& path);
    void saveDesktopRequested(const QString& path);
    void discardRequested(const QString& path);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    friend class UiAlignmentTest;
    /// 摆动状态（风吹 / 挂载掉落的旋转分量共用）。
    /// 两段式（tendedero）：先 0.3s easeOut 到目标摆角，
    /// 再欠阻尼弹簧回稳到 0。
    struct SwingState {
        enum class Phase { Idle, EaseOut, Spring } phase = Phase::Idle;
        double value = 0;         // 当前摆角（度）
        double easeFrom = 0;      // EaseOut 段起点
        double easeTo = 0;        // EaseOut 段终点
        double easeElapsedMs = 0;
        core::Spring spring;      // Spring 段（value→0）
    };

    struct Item {
        core::HistoryEntry entry;
        QImage thumbnail;  // 懒加载
        double tilt = 0;   // 固定倾斜角（度），由路径哈希决定

        // ---- 动效状态 ----
        bool pendingArrive = false;  // 新挂上的：等下一次展开时播放掉落
        bool arriving = false;       // 挂载掉落动画进行中
        core::Spring arriveOffset;   // 纵向 -46 → 0（spring 0.42/0.72）
        core::Spring arriveSwing;    // 摆角 16° → 0（k=46, c=2.6）
        bool flying = false;         // 截图飞入中（本体隐藏，覆盖层在飞）
        bool falling = false;        // 坠落离场中（面板内不再绘制）
        bool landing = false;        // 落地淡入（0.16s easeOut）
        double landElapsedMs = 0;
        SwingState swing;
    };

    void tick();
    void updateRevealForPointer(const QPoint& pos, const QRect& area, qint64 now);
    QMenu* createContextMenu(const QString& path);
    QRectF closeRect(int index) const;
    QPointF cardLocalPoint(int index, const QPoint& pos) const;
    /// 展开到指定屏幕区域（area 为屏幕几何，来自 tick 的指针所在屏）。
    void reveal(const QRect& area);
    /// 探出亮相：展开并在 kPeekMs 后自动收起（有新截图挂上时）。
    void peek(const QRect& area);
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
    int indexOfPath(const QString& path) const;

    // ---- 绘制 ----
    void drawRope(QPainter& painter) const;
    void drawClothespin(QPainter& painter, const QPointF& topCenter) const;
    void drawCardBody(QPainter& painter, int index) const;
    void drawEmptyHint(QPainter& painter) const;

    // ---- 帧驱动（面板滑动 + 卡片动效共用一个 16ms 定时器）----
    void advanceFrame();
    /// 推进一张卡片的动效状态，返回是否仍需下一帧。
    bool advanceItem(Item& item, double dt, double dtMs);
    void ensureFrameTimer();

    // ---- 滑入/滑出 ----
    /// springy = true：弹簧滑入（展开）；false：easeIn 滑出（收起）。
    void startSlide(int targetY, bool hideAtEnd, bool springy);
    /// 面板所在的屏幕区域（动画与定位共用）。
    QRect panelScreenArea() const;
    QScreen* panelScreen() const;

    // ---- 卡片动效 ----
    /// 轻推一张卡片（风吹）：easeOut 到 value+degrees 再弹簧回稳。
    void nudge(int index, double degrees);
    /// 风吹节拍：随机挑一张卡片轻推（tendedero 的 breeze）。
    void gustTick();
    void startRevealBreeze();
    /// 坠落取下：面板内隐藏卡片，FlightOverlay 播坠落，0.6s 后
    /// 才真正移除并发 entryDropped（布局保持到坠落结束）。
    void takeDown(int index);
    /// 飞行到位：真实卡片以 0.16s 淡入接管。
    void landFlying(const QString& path);

    /// 把某张卡片拖给外部程序（文件管理器等）。
    /// 返回 true 表示对方以 MoveAction 接受 —— 此时卡片从衣架取下。
    bool startDragOut(int index);

    std::vector<Item> entries_;
    /// 见过的路径（判定"新挂上的"；防止容量无界增长，超 500 重建）。
    std::set<std::string> knownPaths_;
    /// 是否已完成首次载入（启动载入的历史不算"新挂上的"）。
    bool primed_ = false;
    bool revealed_ = false;
    bool watchEnabled_ = false;
    int hoverIndex_ = -1;
    QTimer* pollTimer_ = nullptr;
    QTimer* holdTimer_ = nullptr;
    bool menuOpen_ = false;
    bool draggingOut_ = false;
    /// 探出亮相的截止时刻（frameClock_ 时基）；在此之前不自动收起。
    qint64 peekUntilMs_ = -1;
    /// flyCapture 先于 setEntries 到达时暂存的请求。
    QString pendingFlyingPath_;
    QRect pendingFlyingRect_;

    // ---- 帧驱动状态 ----
    QTimer* frameTimer_ = nullptr;
    QElapsedTimer frameClock_;   // 帧时钟（也是 peekUntilMs_ 的时基）
    qint64 lastFrameMs_ = 0;

    // ---- 滑入滑出状态 ----
    enum class Slide { None, Spring, EaseIn };
    Slide slideMode_ = Slide::None;
    core::Spring slideSpring_;   // Spring 模式用
    double slideFrom_ = 0;       // EaseIn 模式用
    int slideTargetY_ = 0;  // 内容偏移，窗口本身固定
    double contentOffsetY_ = -210;
    QRect anchoredScreenArea_;
    QString managedDirectory_;
    qint64 edgeSinceMs_ = -1;
    qint64 outsideSinceMs_ = -1;
    double slideElapsedMs_ = 0;  // EaseIn 模式用
    bool slideHideAtEnd_ = false;

    // ---- 风吹 ----
    QTimer* gustTimer_ = nullptr;
    quint64 revealEpoch_ = 0;
    QString copiedPath_;
    qint64 copiedUntilMs_ = -1;

    // ---- 拖出状态 ----
    bool pressedOnCard_ = false;   // 左键按在卡片上（等待判定是点击还是拖拽）
    QPoint pressPos_;
    int pressedIndex_ = -1;

    // ---- 主题色（v2.6）----
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
    static constexpr int kDragStartDistance = 8;  // 超过此位移视为拖出而非点击

    // ---- 动效参数（tendedero 的动画规格）----
    static constexpr int kFrameMs = 16;          // 帧步进（≈60fps）
    static constexpr qint64 kPeekMs = 2500;      // 探出亮相时长
    static constexpr double kSlideEaseInMs = 220;  // 收起时长
    static constexpr double kArriveDrop = 46;    // 挂载掉落起始高度（pt）
    static constexpr double kArriveTilt = 16;    // 挂载掉落起始摆角（度）
    static constexpr double kLandingMs = 160;    // 落地淡入时长
    static constexpr double kNudgeEaseMs = 300;  // 风吹 easeOut 段时长
    static constexpr int kFallHoldMs = 600;      // 取下后布局保持时长
};

} // namespace jie2::ui
