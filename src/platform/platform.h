#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/capability.h"
#include "core/ocr.h"
#include "platform/display.h"
#include "platform/hotkey.h"

namespace jie2::platform {

/// 平台集成层接口。
///
/// 硬约束（评估报告 3.5.3）：
///   - 本层不得依赖 UI 层；不得调用 Widgets/QML。
///   - UI 层只通过本接口获取平台能力，不得直接调用系统 API。
///
/// M0 覆盖：能力探测 + 全局热键。
/// M1 增补：显示器枚举 + 抓屏。
/// 后续里程碑继续在此接口上扩展（置顶窗 / layer-shell / 剪贴板），
/// 扩展时保持同一约定：能则能，不能则如实报告并给出原因。
class Platform {
public:
    virtual ~Platform() = default;

    /// 平台实现名（"macos" / "linux" / "unsupported"）。
    virtual std::string name() const = 0;

    /// 探测当前会话与各项能力的真实可用状态。
    virtual core::PlatformCapabilities probe() const = 0;

    /// 注册全局热键。失败时通过 error 返回原因，不抛异常。
    /// 注意：回调可能在平台事件线程触发，UI 层需自行投递回主线程。
    virtual bool grabHotkey(const HotkeySpec& spec, HotkeyCallback callback,
                            void* userData, std::string* error) = 0;

    /// 注销全部已注册热键（退出前调用）。
    virtual void releaseAllHotkeys() = 0;

    // ---- 抓屏（M1）----

    /// 枚举显示器。返回空表示无法枚举（调用方应据能力矩阵判断原因）。
    virtual std::vector<DisplayInfo> displays() const = 0;

    /// 抓取指定区域（虚拟桌面坐标，物理像素）。
    /// 区域会被裁剪到虚拟桌面范围内；完全越界视为失败。
    virtual CaptureResult captureRegion(const core::Rect& region) const = 0;

    /// 抓取单台显示器全屏。
    virtual CaptureResult captureDisplay(const std::string& displayId) const = 0;

    /// 抓取全部显示器合屏（按虚拟桌面边界拼接）。默认实现基于前两者。
    virtual CaptureResult captureAll() const = 0;

    // ---- 窗口几何（M1：窗口/元素吸附）----

    /// 枚举当前可见的普通窗口矩形（虚拟桌面坐标，与 displays() 同一空间）。
    ///
    /// 只返回**适合吸附**的窗口：过滤掉工具提示、菜单、桌面元素、
    /// 过小的窗口与不可见窗口。宁可少返回也不要返回噪声 ——
    /// 错误的吸附比不吸附更烦人（评估报告 1.5 的窗口吸附要求）。
    virtual std::vector<core::Rect> windowRects() const = 0;

    // ---- 系统文字识别（M5：OCR 的"系统能力"后端）----

    // ---- 遮罩窗口层级提升（M1 增补：远程桌面 / 全屏应用场景）----

    /// 把截图遮罩窗口提升到能盖住全屏应用与远程桌面客户端的层级，
    /// 并让它拿到键盘焦点。
    ///
    /// 为什么必须由平台层做：`Qt::WindowStaysOnTopHint` 只映射到普通浮动层，
    /// 而全屏窗口（含 macOS 的独立 Space、屏幕共享全屏窗口、Windows 的
    /// 独占全屏、X11 的全屏应用）在同一层级之上 —— 用户看到的现象是
    /// **"按了截图热键，必须先切一下窗口（或退出全屏）才能看到遮罩"**。
    ///
    /// nativeWindow 是平台原生窗口句柄：
    ///   - macOS：NSView*（Qt 的 QWidget::winId()）
    ///   - Windows：HWND
    ///   - Linux X11：Window
    /// Wayland 合成器不允许客户端自定层级，如实返回 false。
    /// 返回 true 表示已施加提升动作（不保证合成器一定接受）。
    virtual bool promoteOverlayWindow(void* nativeWindow) {
        (void)nativeWindow;
        return false;
    }

    /// 用系统内置能力识别图像中的文字。
    ///   - macOS：Vision 框架（离线、零配置）
    ///   - Linux/Windows：无系统能力 → 返回 failure 并说明原因
    ///     （Linux 走 tesseract 后端，由 ui 层调度）
    /// languageHint 形如 "zh-Hans" / "en-US"；留空表示用系统默认。
    /// 失败时 error 必填，不静默返回空文本。
    virtual core::OcrResult recognizeText(const core::ImageBuffer& image,
                                          const std::string& languageHint) const = 0;
};

/// 创建当前平台的实现。永不为空。
/// 未知平台返回"能力全为 Unknown"的空实现，而不是崩溃或静默假装可用。
std::unique_ptr<Platform> createPlatform();
#if defined(__APPLE__)
/// 在 Qt 全局逻辑坐标处显示原生 macOS 菜单；nativeMenu 为借用的 NSMenu。
void popupMacContextMenu(void* nativeMenu, int globalX, int globalY);
#endif

} // namespace jie2::platform
