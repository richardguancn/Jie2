#pragma once

#include <QColor>
#include <QImage>  // decorateForOutput / CaptureOverrides 用到（头文件必须自包含）
#include <QObject>
#include <QRect>
#include <QString>
#include <QSystemTrayIcon>

#include <memory>
#include <string>
#include <vector>

#include "core/capability.h"
#include "core/capture_history.h"
#include "core/ocr.h"
#include "core/shortcut_map.h"
#include "platform/platform.h"
#include "ui/pin_window.h"  // PinWindow::DismissAction（Esc 行为配置）

class QAction;
class QMenu;
class QProcess;
class QScreen;
class QTimer;

namespace jie2::ui {
class Clothesline;
class PinWindow;
}

namespace jie2::ui {

/// 托盘控制器 —— 用户可见的入口。
///
/// 职责：
///   - 把平台集成层的探测结果如实呈现给用户（尤其受限项，对应评估报告 3.2）
///   - 通过平台层注册全局热键，并把可能来自后台线程的回调投递回 Qt 主线程
///   - M1：触发截图（抓主屏 → 存文件 + 写剪贴板 → 通知）
///   - 配置持久化
///
/// 约束：本类只调用 platform / core 的接口，不直接触碰任何系统 API。
class TrayController : public QObject {
    Q_OBJECT

public:
    explicit TrayController(std::unique_ptr<platform::Platform> platform,
                            QObject* parent = nullptr);
    ~TrayController() override;

    /// 启动：载入配置、探测、注册热键、显示托盘图标。
    bool start();

    /// 抓取主屏，保存到截图目录并写入系统剪贴板。
    /// 成功返回保存路径；失败返回空字符串并通过 error 说明原因。
    /// 供 --capture-once（自动化验证）使用，不含交互。
    QString captureToFile(QString* error);

    /// 交互式截图：抓取光标所在屏幕 → 选择遮罩框选 → 裁剪 → 保存 + 剪贴板。
    /// 用户取消时返回空路径且 error 为空（取消不是错误）。
    /// 供托盘动作与全局热键使用。
    QString captureWithSelection(QString* error);

    /// 按给定选区截图（逻辑坐标，相对 screen 的左上角）。
    /// screen 为 nullptr 时用光标所在屏。
    /// 与交互路径共用同一条"抓屏 → 换算 → 裁剪 → 保存"链路，
    /// 只是选区由调用方给定 —— 用于 --select-rect 自动化验证与 M6 脚本化调用。
    QString captureRectToFile(const QRect& logicalSelection, QScreen* screen,
                              QString* error);

    /// 供测试查询：当前热键是否注册成功。
    bool hotkeyRegistered() const { return hotkeyRegistered_; }
    const QString& hotkeyError() const { return hotkeyError_; }

    // ---- 贴图分组（M4 增补，专业版对齐项）----

    /// 切换到指定贴图分组：只显示该组的贴图，其余组的隐藏
    /// （保留在内存与列表里）。传 -1 表示"下一组"（循环）。
    void switchPinGroup(int group);

    /// 全选/取消全选所有贴图（专业版 v2.4：Ctrl+A）。
    /// 全选后拖动任一选中贴图会带动其余一起移动。
    void toggleSelectAllPins();

    /// 清空当前贴图组（专业版命令行 `empty-group` 的语义）。
    /// 覆盖所有贴图类型：关闭 = 可再贴出（销毁语义由贴图自身的关闭行为处理）。
    /// 返回被关闭的贴图数量。
    int emptyCurrentPinGroup();

    /// 显示"贴图分组管理"对话框（专业版命令行 `show-group-manager`）。
    /// 列出各组贴图数量，可切换/清空。
    void showPinGroupManager();

    /// 唤起已有实例的提示（二次启动时由单实例通道调用）。
    void activateExistingInstance();

    /// 供测试查询：当前组内贴图数量。
    int currentGroupPinCount() const;

    // ---- 供自动化验证（--test-pin-groups）----
    /// 直接创建 N 张纯色测试贴图（不经过剪贴板），返回创建数量。
    int createTestPins(int count);
    /// 当前贴图总数 / 可见贴图数。
    int pinCount() const { return static_cast<int>(pins_.size()); }
    int visiblePinCount() const;
    int currentPinGroup() const { return currentPinGroup_; }

    // ---- 命令行覆盖（M6）----
    //
    // 一次性覆盖项：只影响本次进程的截图落点与装饰，不写入配置文件
    // （命令行调用不应改变用户的持久设置）。
    struct CaptureOverrides {
        QString outputPath;      // 显式输出路径（静默保存，替代自动命名）
        bool noSave = false;     // 只写剪贴板，不落盘
        bool hasShadow = false;
        bool shadow = false;
        bool hasBorder = false;
        int borderWidth = 0;     // >0 时启用并指定宽度
        bool hasRound = false;
        int roundRadius = 0;
        bool hasSize = false;    // --size W H：强制输出尺寸
        int sizeWidth = 0;
        int sizeHeight = 0;
        bool keepAspectRatio = false;  // --keep-aspect-ratio：配合 --size，保持比例
    };
    void setCaptureOverrides(const CaptureOverrides& overrides);

    /// 白板模式（M6）：不抓屏，用纯色画布打开标注遮罩，保存标注结果。
    /// 对齐 Snipaste 的 `whiteboard` 命令：用于纯演示/画图场景。
    /// 用户取消时返回空路径且 error 为空。
    QString whiteboardWithSelection(const QColor& background, QString* error);

    // ---- M1：输出装饰（专业版对齐项：带边框 / 带阴影 / 圆角截图）----

    /// 按当前设置对图像应用输出装饰（圆角 → 边框 → 阴影，顺序与
    /// "先切形状、再加边、最后投影"的直觉一致）。
    /// 未启用任何装饰时原样返回，不做无谓拷贝。
    QImage decorateForOutput(const QImage& image) const;

private:
    void rebuildMenu();
    void showCapabilities();
    void registerHotkey();
    void handleHotkey(const std::string& id);
    void triggerCapture();
    void note(const QString& title, const QString& body, bool warning = false);
    void loadConfig();
    void saveConfig();

    /// 触发角（M1）：屏幕角落停留触发截图。
    void setupTriggerCorner();
    void cornerTick();

    /// 衣架（M3）：把历史条目喂给衣架窗口。
    void refreshClothesline();

    // ---- Wayland 顶部条（B 项：layer-shell）----
    //
    // Wayland 会话下 Qt 窗口不能定位（合成器不接受 move），所以 Qt 衣架
    // 窗口无法贴到屏幕顶边。改用独立的 jie2-line 进程渲染 layer-shell
    // 顶部条：主程序把条目写入 items 文件，jie2-line 轮询并重绘。
    void updateWaylandLine();   // 写 items 文件 + 确保 jie2-line 在运行
    void stopWaylandLine();     // 退出时终止子进程
    /// 查找 jie2-line 可执行文件（环境变量 → 同目录 → 构建树 → PATH）。
    static QString findLineBinary();

    /// 剪贴板图片监听（M3 增补）：复制了图像就自动挂上衣架。
    /// 对齐 tendedero 的 "Hang copied images"：只处理纯图像 ——
    /// 同时带文本/文件的内容（如网页复制图片带 URL）跳过。
    void setupClipboardWatcher();
    void clipboardChanged();
    /// 把图像存到截图目录（供监听与其它落点复用），返回路径。
    QString saveImageToDirectory(const QImage& image, const QString& prefix,
                                 QString* error);
    /// 主动写剪贴板图像（截图落点 / 衣架复制 / 等）。
    ///
    /// 必须走这个入口而不是直接 setImage：写完后把该图的摘要记进
    /// lastClipboardDigest_，让**异步**到达的 dataChanged（X11 上
    /// 剪贴板事件不是同步投递的，同步标志拦不住）被内容去重过滤掉。
    void writeClipboardImage(const QImage& image);
    /// 图像摘要（缩到 32x32 后 MD5）：用于"同一张图不重复挂"的判定。
    static QByteArray imageDigest(const QImage& image);

    /// 贴图（M4）：把剪贴板内容贴成置顶窗口。
    void triggerPin();
    /// 隐藏/显示全部贴图（Snipaste 的 Shift+F3 语义）。
    void toggleHidePins();
    /// 关闭全部贴图。
    void closeAllPins();

    /// 贴图批量保存（M4 增补）：把当前所有贴图存到截图目录。
    void saveAllPins();

    /// 文字识别（M5）：对剪贴板图像跑 OCR，结果写回剪贴板。
    void triggerOcr();
    /// 对指定图像跑 OCR（供托盘动作与自动化验证共用）。
    core::OcrResult runOcrOnImage(const QImage& image);
    const core::OcrConfig& ocrConfig() const { return ocrConfig_; }

    /// 二维码/条形码识别（M6，专业版对齐项）：对剪贴板图像扫码。
    void triggerBarcodeScan();

    // ---- 标注调色板（M2 增补，专业版对齐项：颜色数量可自定义）----
    int paletteSize_ = 6;

    // ---- 输出装饰设置（M1）----
    bool roundEnabled_ = false;
    int roundRadius_ = 8;
    bool borderEnabled_ = false;
    int borderWidth_ = 1;
    QColor borderColor_{210, 210, 210};
    bool shadowEnabled_ = false;

    // ---- 触发角设置（M1）----
    bool triggerEnabled_ = false;
    int triggerSize_ = 6;        // 触发区边长（逻辑点）
    int triggerDwellMs_ = 350;   // 停留时长
    QString triggerCorners_ = QStringLiteral("all");  // all / tl,tr,bl,br 组合
    QTimer* cornerTimer_ = nullptr;
    qint64 cornerEnteredAt_ = 0;  // 进入角落的时刻（0 = 不在角落）
    bool cornerArmed_ = true;     // 触发后需离开角落才会重新武装
    bool captureInProgress_ = false;  // 遮罩打开期间抑制触发角重复触发

    // ---- 衣架（M3，tendedero 合并）----
    Clothesline* clothesline_ = nullptr;
    bool clotheslineEnabled_ = true;

    // ---- 剪贴板监听（M3 增补）----
    bool clipboardWatchEnabled_ = true;
    QByteArray lastClipboardDigest_;   // 去重：同一图像不重复挂
    bool clipboardProcessing_ = false; // 防重入（监听处理期间不再进入）

    // ---- Wayland 顶部条（B 项）----
    bool waylandLineEnabled_ = false;
    QProcess* lineProcess_ = nullptr;

    // ---- 贴图（M4）----
    std::vector<PinWindow*> pins_;
    bool pinsHidden_ = false;
    int pinGroupCount_ = 3;    // 分组数（专业版对齐项；默认 3 组）
    int currentPinGroup_ = 0;  // 当前显示的分组
    std::string pinHotkeyId_;
    bool pinHotkeyRegistered_ = false;
    QString pinHotkeyError_;
    bool saveClipboardHotkeyRegistered_ = false;  // Ctrl+Alt+C（专业版 v2.5）

    // 贴图 Esc 行为（专业版 v2.8：可自定义 Esc / Shift+Esc 的功能）。
    // 从配置 pin.esc_action / pin.shift_esc_action 读取（close|hide）。
    PinWindow::DismissAction pinEscAction_ = PinWindow::DismissAction::Close;
    PinWindow::DismissAction pinShiftEscAction_ = PinWindow::DismissAction::Hide;

    // 缩略图修饰键（专业版 v2.11：可自定义缩略图快捷键）。
    // 从配置 pin.thumbnail_modifier 读取（shift|ctrl|alt）。
    Qt::KeyboardModifier pinThumbnailModifier_ = Qt::ShiftModifier;

    // ---- 文字识别（M5）----
    core::OcrConfig ocrConfig_;

    // ---- 主题（v2.6：跟随系统）与自定义快捷键（v2.11.3）----
    QString themeSetting_ = QStringLiteral("system");
    bool darkTheme_ = true;                       // 解析后的实际主题
    /// 应用内快捷键映射（v2.11.3）。默认即出厂绑定 —— 即便配置读取
    /// 失败（或未来新增 CLI 路径忘了 loadConfig），字母快捷键仍然可用。
    core::ShortcutMap shortcutMap_ = core::ShortcutMap::defaults();
    bool eraserUndoable_ = true;                  // 橡皮擦删除可撤销（v2.6 可禁用）

    // ---- 命令行覆盖（M6）----
    CaptureOverrides overrides_;

    /// 截图历史（M1：, / . 回放）。持久化在配置目录的 history.txt。
    void loadHistory();
    void saveHistory();
    void recordCapture(const QString& path, const QImage& image);
    /// 新截图的源区域（全局逻辑坐标）：recordCapture 据此让衣架播放
    /// "从被截取区域飞入"动效（tendedero CaptureFlight）。由截图路径
    /// 在 saveAndCopy 前设置，recordCapture 消费后清空。
    QRect pendingFlyRect_;

    /// 保存 PNG 到截图目录并写入剪贴板（截图落点的公共实现）。
    /// writeClipboard=false 时只落盘不回写（Ctrl+Alt+C 保存剪贴板图像用）。
    QString saveAndCopy(const QImage& image, QString* error,
                        bool writeClipboard = true);

    /// Ctrl+Alt+C：把剪贴板中的图像保存为文件（专业版对齐项 v2.5）。
    void triggerSaveClipboardImage();

    /// 平台层回调是裸函数指针，这里做静态跳板。
    static void hotkeyThunk(const std::string& id, void* userData);

    std::unique_ptr<platform::Platform> platform_;
    core::PlatformCapabilities capabilities_;
    QSystemTrayIcon* tray_ = nullptr;
    QMenu* menu_ = nullptr;
    bool hotkeyRegistered_ = false;
    QString hotkeyError_;
    int hotkeyCount_ = 0;
    int captureCount_ = 0;
    QString lastCapturePath_;
    QString configPath_;
    core::CaptureHistory history_{16};  // 容量与 Snipaste 默认相当的保守值
};

} // namespace jie2::ui
