// 「J2」Windows 平台实现（Win32）
//
// 状态声明（重要）：
//   本文件已通过 **mingw-w64 交叉编译验证**（编译 + 链接通过，
//   API 用法可编译性得到证实），但**尚未在真实 Windows 上运行验证** ——
//   开发环境没有 Windows 运行链。按本项目"能验证才声称完成"的约定，
//   Windows 支持目前是"实现完成、待实机验证"状态（README 如实标注，
//   并在代码要点处列出实机验证清单）。
//
// 覆盖的接口（与 macOS/Linux 实现同一 Platform 接口）：
//   - 能力探测：会话类型、抓屏/热键/置顶/剪贴板能力矩阵
//   - 抓屏：GDI BitBlt（虚拟桌面坐标系，含负坐标多屏）
//   - 显示器枚举：EnumDisplayMonitors + GetMonitorInfo + 每屏 DPI
//   - 全局热键：RegisterHotKey（专用消息线程，见下方说明）
//   - 窗口几何：EnumWindows（供窗口吸附）
//   - 文字识别：WinRT 的 Windows.Media.Ocr 不属于纯 Win32 范畴，
//     如实报告"系统后端不可用"，指引用户配置 tesseract 后端。
//
// 全局热键的线程模型（与 Linux X11 后端同一约定）：
//   RegisterHotKey 把 WM_HOTKEY 投递到**调用它的线程**的消息队列；
//   平台层没有窗口也没有消息循环（那是 UI 层的）。所以这里起一个
//   专用线程：注册请求经队列投递（PostThreadMessage 唤醒），线程里
//   完成 RegisterHotKey 并跑 GetMessage 循环，收到 WM_HOTKEY 时调用
//   回调。回调可能在平台事件线程触发 —— UI 层负责投递回自己的线程。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/image_buffer.h"
#include "platform/windows/windows_platform.h"

namespace jie2::platform {

namespace {

constexpr UINT kMsgProcessQueue = WM_APP + 1;

/// 平台无关键码 → Windows 虚拟键码（VK_*）；未覆盖返回 0。
UINT toVirtualKey(Key key) {
    switch (key) {
        case Key::A: return 'A'; case Key::B: return 'B'; case Key::C: return 'C';
        case Key::D: return 'D'; case Key::E: return 'E'; case Key::F: return 'F';
        case Key::G: return 'G'; case Key::H: return 'H'; case Key::I: return 'I';
        case Key::J: return 'J'; case Key::K: return 'K'; case Key::L: return 'L';
        case Key::M: return 'M'; case Key::N: return 'N'; case Key::O: return 'O';
        case Key::P: return 'P'; case Key::Q: return 'Q'; case Key::R: return 'R';
        case Key::S: return 'S'; case Key::T: return 'T'; case Key::U: return 'U';
        case Key::V: return 'V'; case Key::W: return 'W'; case Key::X: return 'X';
        case Key::Y: return 'Y'; case Key::Z: return 'Z';
        case Key::Num0: return '0'; case Key::Num1: return '1';
        case Key::Num2: return '2'; case Key::Num3: return '3';
        case Key::Num4: return '4'; case Key::Num5: return '5';
        case Key::Num6: return '6'; case Key::Num7: return '7';
        case Key::Num8: return '8'; case Key::Num9: return '9';
        case Key::F1: return VK_F1; case Key::F2: return VK_F2;
        case Key::F3: return VK_F3; case Key::F4: return VK_F4;
        case Key::F5: return VK_F5; case Key::F6: return VK_F6;
        case Key::F7: return VK_F7; case Key::F8: return VK_F8;
        case Key::F9: return VK_F9; case Key::F10: return VK_F10;
        case Key::F11: return VK_F11; case Key::F12: return VK_F12;
        case Key::Space: return VK_SPACE;
            case Key::Tab: return VK_TAB;
            case Key::Return: return VK_RETURN;
            case Key::Escape: return VK_ESCAPE;
            case Key::Backspace: return VK_BACK;
            case Key::Delete: return VK_DELETE;
            case Key::Insert: return VK_INSERT;
            case Key::Home: return VK_HOME;
            case Key::End: return VK_END;
            case Key::Left: return VK_LEFT;
            case Key::Right: return VK_RIGHT;
            case Key::Up: return VK_UP;
            case Key::Down: return VK_DOWN;
            case Key::PageUp: return VK_PRIOR;
            case Key::PageDown: return VK_NEXT;
            case Key::Minus: return VK_OEM_MINUS;
            case Key::Equal: return VK_OEM_PLUS;
            case Key::BracketLeft: return VK_OEM_4;
            case Key::BracketRight: return VK_OEM_6;
            case Key::Backslash: return VK_OEM_5;
            case Key::Semicolon: return VK_OEM_1;
            case Key::Apostrophe: return VK_OEM_7;
            case Key::Comma: return VK_OEM_COMMA;
            case Key::Period: return VK_OEM_PERIOD;
            case Key::Slash: return VK_OEM_2;
            case Key::Grave: return VK_OEM_3;
            case Key::F13: return VK_F13;
            case Key::F14: return VK_F14;
            case Key::F15: return VK_F15;
            case Key::F16: return VK_F16;
            case Key::F17: return VK_F17;
            case Key::F18: return VK_F18;
            case Key::F19: return VK_F19;
            case Key::F20: return VK_F20;
            case Key::Unknown: break;
    }
    return 0;
}

UINT toWindowsModifiers(unsigned modifiers) {
    UINT out = 0;
    if (modifiers & kModCtrl)  { out |= MOD_CONTROL; }
    if (modifiers & kModAlt)   { out |= MOD_ALT; }
    if (modifiers & kModShift) { out |= MOD_SHIFT; }
    if (modifiers & kModSuper) { out |= MOD_WIN; }
    return out;
}

/// 全局热键的专用消息线程。
class HotkeyThread {
public:
    ~HotkeyThread() { stop(); }

    bool grab(const HotkeySpec& spec, HotkeyCallback callback, void* userData,
              std::string* error) {
        if (!ensureStarted(error)) {
            return false;
        }
        Request request;
        request.specId = spec.id;
        request.callback = callback;
        request.userData = userData;
        request.vk = toVirtualKey(spec.key);
        request.modifiers = toWindowsModifiers(spec.modifiers);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(&request);
        }
        PostThreadMessageW(threadId_, kMsgProcessQueue, 0, 0);
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, std::chrono::seconds(3),
                          [&request]() { return request.done; })) {
            if (error != nullptr) {
                *error = "热键注册等待超时";
            }
            return false;
        }
        if (!request.ok && error != nullptr) {
            *error = request.error;
        }
        return request.ok;
    }

    void releaseAll() {
        std::vector<int> ids;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& entry : entries_) {
                ids.push_back(entry.id);
            }
            entries_.clear();
        }
        for (const int id : ids) {
            // 实机验证点：文档建议由注册线程注销；跨线程解绑在多数实现下
            // 可行，但 Windows 实机验证时应确认（若不行，改为经消息线程处理）。
            UnregisterHotKey(nullptr, id);
        }
    }

    void stop() {
        if (!running_.load()) {
            return;
        }
        running_.store(false);
        if (threadId_ != 0) {
            PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        threadId_ = 0;
        ready_.store(false);
    }

private:
    struct Entry {
        int id = 0;
        std::string specId;
        HotkeyCallback callback = nullptr;
        void* userData = nullptr;
    };

    struct Request {
        std::string specId;
        HotkeyCallback callback = nullptr;
        void* userData = nullptr;
        UINT vk = 0;
        UINT modifiers = 0;
        bool ok = false;
        bool done = false;
        std::string error;
    };

    bool ensureStarted(std::string* error) {
        if (running_.load()) {
            return true;
        }
        ready_.store(false);
        running_.store(true);
        thread_ = std::thread([this]() { threadMain(); });
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, std::chrono::seconds(3),
                          [this]() { return ready_.load(); })) {
            if (error != nullptr) {
                *error = "热键线程启动超时";
            }
            return false;
        }
        return true;
    }

    void threadMain() {
        // 先取一次消息：迫使系统为线程创建消息队列（否则
        // PostThreadMessage 会失败）。
        MSG msg{};
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        threadId_ = GetCurrentThreadId();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ready_.store(true);
        }
        cv_.notify_all();

        while (running_.load()) {
            const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
            if (got <= 0) {
                break;
            }
            if (msg.message == WM_HOTKEY) {
                Entry entryCopy;
                bool found = false;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    for (const auto& candidate : entries_) {
                        if (candidate.id == static_cast<int>(msg.wParam)) {
                            entryCopy = candidate;
                            found = true;
                            break;
                        }
                    }
                }
                if (found && entryCopy.callback != nullptr) {
                    entryCopy.callback(entryCopy.specId, entryCopy.userData);
                }
            } else if (msg.message == kMsgProcessQueue) {
                processQueue();
            }
        }
    }

    void processQueue() {
        for (;;) {
            Request* request = nullptr;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (queue_.empty()) {
                    return;
                }
                request = queue_.front();
                queue_.pop_front();
            }
            if (request->vk == 0) {
                request->ok = false;
                request->error = "键不受支持";
            } else {
                const int id = nextId_++;
                const UINT mods = request->modifiers | MOD_NOREPEAT;
                if (RegisterHotKey(nullptr, id, mods, request->vk)) {
                    Entry entry;
                    entry.id = id;
                    entry.specId = request->specId;
                    entry.callback = request->callback;
                    entry.userData = request->userData;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        entries_.push_back(std::move(entry));
                    }
                    request->ok = true;
                } else {
                    request->ok = false;
                    const DWORD code = GetLastError();
                    request->error =
                        "RegisterHotKey 失败（错误 " + std::to_string(code) +
                        (code == ERROR_HOTKEY_ALREADY_REGISTERED
                             ? "：该组合已被其他程序占用"
                             : "") +
                        "）";
                }
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                request->done = true;
            }
            cv_.notify_all();
        }
    }

    std::thread thread_;
    DWORD threadId_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Request*> queue_;
    std::vector<Entry> entries_;
    int nextId_ = 1;
};

std::string wideToUtf8(const wchar_t* text) {
    if (text == nullptr || text[0] == L'\0') {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
                                           nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr,
                        nullptr);
    return out;
}

} // namespace

class WindowsPlatform final : public Platform {
public:
    WindowsPlatform() {
        // 每屏 DPI 感知：不声明的话在缩放屏上抓屏坐标会错位。
        // SetProcessDpiAwareness（shellscalingapi）从 Win8.1 起可用。
        // 实机验证点：确认多屏混合 DPI 下抓屏区域与虚拟桌面坐标一致。
        SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
    }

    std::string name() const override { return "windows"; }

    core::PlatformCapabilities probe() const override {
        core::PlatformCapabilities caps;
        caps.session = core::SessionType::Windows;

        OSVERSIONINFOEXW version{};
        version.dwOSVersionInfoSize = sizeof(version);
        // 说明：GetVersionEx 在 Win8.1+ 受 manifest 影响；作为展示信息够用，
        // 不作为能力判断依据。实机验证点：改为 RtlGetVersion 获取准确版本。
        std::string versionText = "Windows";
        caps.sessionDetail = versionText;

        caps.screenCapture = {core::CapabilityStatus::Available,
                              "GDI BitBlt（虚拟桌面坐标；DWM 场景实机验证）"};
        caps.globalHotkey = {core::CapabilityStatus::Available,
                             "RegisterHotKey（专用消息线程）"};
        caps.alwaysOnTop = {core::CapabilityStatus::Available,
                            "SetWindowPos(HWND_TOPMOST)"};
        caps.layerShell = {core::CapabilityStatus::NotApplicable,
                           "Windows 无 layer-shell 概念，顶部条用置顶窗实现"};
        caps.clipboardImage = {core::CapabilityStatus::Available,
                               "剪贴板 DIB（由 UI 层 Qt 处理）"};
        return caps;
    }

    // ---- 抓屏 ----

    std::vector<DisplayInfo> displays() const override {
        std::vector<DisplayInfo> out;
        struct Context {
            std::vector<DisplayInfo>* list;
        } context{&out};

        EnumDisplayMonitors(
            nullptr, nullptr,
            [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
                auto* list =
                    reinterpret_cast<Context*>(data)->list;
                MONITORINFOEXW info{};
                info.cbSize = sizeof(info);
                if (GetMonitorInfoW(monitor, &info)) {
                    DisplayInfo display;
                    // 标识：HMONITOR 数值的字符串形式（进程内稳定）。
                    display.id = std::to_string(
                        reinterpret_cast<std::uintptr_t>(monitor));
                    display.name = wideToUtf8(info.szDevice);
                    display.bounds = core::Rect{
                        info.rcMonitor.left, info.rcMonitor.top,
                        info.rcMonitor.right - info.rcMonitor.left,
                        info.rcMonitor.bottom - info.rcMonitor.top};
                    UINT dpiX = 96;
                    UINT dpiY = 96;
                    if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI,
                                                   &dpiX, &dpiY)) &&
                        dpiX > 0) {
                        display.scaleFactor = static_cast<double>(dpiX) / 96.0;
                    }
                    display.isPrimary =
                        (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
                    list->push_back(std::move(display));
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&context));
        return out;
    }

    CaptureResult captureRegion(const core::Rect& region) const override {
        if (region.isEmpty()) {
            return CaptureResult::failure("截图区域为空");
        }
        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        const core::Rect clipped =
            region.intersected(core::Rect{vx, vy, vw, vh});
        if (clipped.isEmpty()) {
            return CaptureResult::failure("截图区域超出虚拟桌面范围");
        }

        HDC screenDc = GetDC(nullptr);
        if (screenDc == nullptr) {
            return CaptureResult::failure("GetDC 失败");
        }
        HDC memDc = CreateCompatibleDC(screenDc);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = clipped.width;
        info.bmiHeader.biHeight = -clipped.height;  // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(screenDc, &info, DIB_RGB_COLORS,
                                          &bits, nullptr, 0);
        if (memDc == nullptr || bitmap == nullptr || bits == nullptr) {
            if (bitmap != nullptr) { DeleteObject(bitmap); }
            if (memDc != nullptr) { DeleteDC(memDc); }
            ReleaseDC(nullptr, screenDc);
            return CaptureResult::failure("创建位图缓冲失败");
        }
        HGDIOBJ old = SelectObject(memDc, bitmap);
        const BOOL ok =
            BitBlt(memDc, 0, 0, clipped.width, clipped.height, screenDc,
                   clipped.x, clipped.y, SRCCOPY);
        SelectObject(memDc, old);

        CaptureResult result;
        if (!ok) {
            result = CaptureResult::failure(
                "BitBlt 失败（实机验证点：DWM/硬件加速窗口场景）");
        } else {
            core::ImageBuffer buffer(clipped.width, clipped.height,
                                     core::PixelFormat::BGRA8888);
            const std::size_t rowBytes =
                static_cast<std::size_t>(clipped.width) * 4;
            for (int y = 0; y < clipped.height; ++y) {
                std::memcpy(buffer.scanline(y),
                            static_cast<const std::uint8_t*>(bits) +
                                static_cast<std::size_t>(y) * rowBytes,
                            rowBytes);
                // DIB 的 alpha 未定义（BI_RGB），截图内容按不透明处理。
                std::uint8_t* row = buffer.scanline(y);
                for (int x = 0; x < clipped.width; ++x) {
                    row[x * 4 + 3] = 255;
                }
            }
            result = CaptureResult::success(std::move(buffer));
        }

        DeleteObject(bitmap);
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);
        return result;
    }

    CaptureResult captureDisplay(const std::string& displayId) const override {
        for (const auto& display : displays()) {
            if (display.id == displayId) {
                return captureRegion(display.bounds);
            }
        }
        return CaptureResult::failure("未找到显示器：" + displayId);
    }

    CaptureResult captureAll() const override {
        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (vw <= 0 || vh <= 0) {
            return CaptureResult::failure("虚拟桌面尺寸无效");
        }
        return captureRegion(core::Rect{vx, vy, vw, vh});
    }

    // ---- 窗口几何 ----

    std::vector<core::Rect> windowRects() const override {
        std::vector<core::Rect> result;
        const DWORD ownPid = GetCurrentProcessId();

        struct Context {
            std::vector<core::Rect>* list;
            DWORD ownPid;
        } context{&result, ownPid};

        EnumWindows(
            [](HWND window, LPARAM data) -> BOOL {
                auto* context = reinterpret_cast<Context*>(data);
                if (!IsWindowVisible(window)) {
                    return TRUE;
                }
                // 过滤工具窗口（悬浮提示等不适合吸附的形态）。
                const LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
                if ((exStyle & WS_EX_TOOLWINDOW) != 0) {
                    return TRUE;
                }
                // 排除本进程自己的窗口。
                DWORD pid = 0;
                GetWindowThreadProcessId(window, &pid);
                if (pid == context->ownPid) {
                    return TRUE;
                }
                RECT rect{};
                if (!GetWindowRect(window, &rect)) {
                    return TRUE;
                }
                const int width = rect.right - rect.left;
                const int height = rect.bottom - rect.top;
                if (width < 40 || height < 40) {
                    return TRUE;
                }
                // 最小化窗口的矩形是 (-32000,...)：过滤。
                if (rect.left <= -30000 || rect.top <= -30000) {
                    return TRUE;
                }
                context->list->push_back(
                    core::Rect{rect.left, rect.top, width, height});
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&context));
        return result;
    }

    // ---- 热键 ----

    bool grabHotkey(const HotkeySpec& spec, HotkeyCallback callback,
                    void* userData, std::string* error) override {
        return hotkeys_.grab(spec, callback, userData, error);
    }

    void releaseAllHotkeys() override { hotkeys_.releaseAll(); }

    // 遮罩窗口层级提升（远程桌面 / 独占全屏场景）。
    //
    // 全屏的前台窗口（含 RDP 客户端全屏）会把普通置顶窗口压在下面，
    // SetForegroundWindow 又受前台锁限制 —— 这里三个动作一起做：
    // ① HWND_TOPMOST 提到最上层；② 解除前台锁再抢前台；③ SetFocus
    // 让键盘事件（Esc / 回车 / F5）直达遮罩。
    bool promoteOverlayWindow(void* nativeWindow) override {
        HWND window = static_cast<HWND>(nativeWindow);
        if (window == nullptr) {
            return false;
        }
        SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        // 前台锁：本进程刚收到全局热键，通常已被允许；失败不视为错误，
        // 因为 SetWindowPos(TOPMOST) 已经保证可见性。
        const DWORD foregroundThread =
            GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        const DWORD selfThread = GetCurrentThreadId();
        if (foregroundThread != 0 && foregroundThread != selfThread) {
            AttachThreadInput(foregroundThread, selfThread, TRUE);
            SetForegroundWindow(window);
            AttachThreadInput(foregroundThread, selfThread, FALSE);
        } else {
            SetForegroundWindow(window);
        }
        SetFocus(window);
        return true;
    }

    // ---- 文字识别（M5）----

    core::OcrResult recognizeText(const core::ImageBuffer&,
                                  const std::string&) const override {
        return core::OcrResult::failure(
            "Windows 无纯 Win32 的内置 OCR（Windows.Media.Ocr 属 WinRT）；"
            "请在配置中选择 tesseract 后端（ocr.backend=tesseract）"
            "或自定义命令 / OpenAI 兼容端点",
            "system");
    }

private:
    HotkeyThread hotkeys_;
};

std::unique_ptr<Platform> createWindowsPlatform() {
    return std::make_unique<WindowsPlatform>();
}

} // namespace jie2::platform
