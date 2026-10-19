// 「J2」Linux 平台实现
//
// 设计要点（评估报告 3.2 / 3.5）：
//   - X11 与 Wayland 是两套能力模型，必须分开如实报告，不能假装一致。
//   - X11 用 XGrabKey 实现全局热键（直通，无授权弹窗）。
//   - Wayland 下热键需要 xdg-desktop-portal GlobalShortcuts 或合成器私有协议，
//     M0 尚未实现 —— 如实报告 Unavailable 并注明计划里程碑，绝不静默失败。
//   - 本文件在未找到 X11 开发库时仍可编译（JIE2_HAVE_X11 未定义），
//     此时 X11 会话的热键功能如实报告为不可用。

#include "platform/linux/linux_platform.h"

#include "core/image_ops.h"
#include "platform/linux/wayland_portal.h"
#include "platform/linux/wayland_screenshot.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(JIE2_HAVE_X11)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#if defined(JIE2_HAVE_XRANDR)
#include <X11/extensions/Xrandr.h>
#endif

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#endif

namespace jie2::platform {

namespace {

const char* envValue(const char* name) {
    const char* value = std::getenv(name);
    return (value != nullptr && *value != '\0') ? value : nullptr;
}

struct SessionProbe {
    core::SessionType type = core::SessionType::Unknown;
    std::string detail;
};

/// 判定当前桌面会话类型。优先 XDG_SESSION_TYPE，退化到 DISPLAY / WAYLAND_DISPLAY。
SessionProbe detectSession() {
    const char* sessionType = envValue("XDG_SESSION_TYPE");
    const char* waylandDisplay = envValue("WAYLAND_DISPLAY");
    const char* x11Display = envValue("DISPLAY");
    const char* desktop = envValue("XDG_CURRENT_DESKTOP");

    const std::string desktopName = desktop != nullptr ? desktop : "";

    bool isWayland = false;
    bool isX11 = false;
    if (sessionType != nullptr) {
        if (std::strcmp(sessionType, "wayland") == 0) {
            isWayland = true;
        } else if (std::strcmp(sessionType, "x11") == 0) {
            isX11 = true;
        }
    }
    if (!isWayland && !isX11) {
        if (waylandDisplay != nullptr) {
            isWayland = true;
        } else if (x11Display != nullptr) {
            isX11 = true;
        }
    }

    SessionProbe probe;
    if (isWayland) {
        probe.type = core::SessionType::Wayland;
        probe.detail = desktopName.empty() ? "Wayland 会话"
                                           : desktopName + " (Wayland)";
    } else if (isX11) {
        probe.type = core::SessionType::X11;
        probe.detail = desktopName.empty() ? "X11 会话" : desktopName + " (X11)";
    } else {
        probe.type = core::SessionType::Unknown;
        probe.detail = "未检测到 DISPLAY / WAYLAND_DISPLAY（无桌面会话）";
    }
    return probe;
}

#if defined(JIE2_HAVE_X11)

/// X11 全局热键监听。
///
/// 实现要点：
///   - 独占一条到 X 服务器的连接，在工作线程里用 select + XPending 轮询，
///     避免阻塞调用方线程。
///   - 注册时对 NumLock / CapsLock 的全部组合分别 grab，否则这两个修饰键
///     处于开启状态时热键会失效（X11 的经典陷阱）。
///   - 所有对 Display* 的访问都由 mutex_ 串行化（Xlib 连接非线程安全）。
class X11HotkeyListener {
public:
    ~X11HotkeyListener() { releaseAll(); }

    bool ensureStarted(std::string* error) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (display_ != nullptr) {
            return true;
        }
        display_ = XOpenDisplay(nullptr);
        if (display_ == nullptr) {
            if (error != nullptr) {
                *error = "XOpenDisplay 失败：无法连接 X 服务器";
            }
            return false;
        }
        XSetErrorHandler(&x11ErrorHandler);
        running_.store(true);
        worker_ = std::thread([this] { workerLoop(); });
        return true;
    }

    bool grab(const HotkeySpec& spec, HotkeyCallback callback, void* userData,
              std::string* error) {
        if (!ensureStarted(error)) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (display_ == nullptr) {
            if (error != nullptr) { *error = "X 连接不可用"; }
            return false;
        }

        const KeySym keysym = toKeysym(spec.key);
        if (keysym == NoSymbol) {
            if (error != nullptr) { *error = "键不受支持"; }
            return false;
        }
        const KeyCode keycode = XKeysymToKeycode(display_, keysym);
        if (keycode == 0) {
            if (error != nullptr) { *error = "该键在当前键盘布局下无键码"; }
            return false;
        }

        const unsigned modifiers = toX11Modifiers(spec.modifiers);
        const Window root = DefaultRootWindow(display_);

        // NumLock / CapsLock 的全部组合都要 grab，否则这两个修饰键开启时热键失效。
        const unsigned extras[] = {0u, Mod2Mask, LockMask, Mod2Mask | LockMask};
        errorSlot() = 0;
        for (unsigned extra : extras) {
            XGrabKey(display_, static_cast<int>(keycode), modifiers | extra, root,
                     True, GrabModeAsync, GrabModeAsync);
        }
        XSync(display_, False);

        const int grabbed = errorSlot();
        if (grabbed != 0) {
            // 回滚：本次 grab 的组合全部撤销
            for (unsigned extra : extras) {
                XUngrabKey(display_, static_cast<int>(keycode), modifiers | extra, root);
            }
            XSync(display_, False);
            if (error != nullptr) {
                *error = "XGrabKey 被拒绝（多半是该组合已被其他程序占用）";
            }
            return false;
        }

        Entry entry;
        entry.id = spec.id;
        entry.callback = callback;
        entry.userData = userData;
        entry.keycode = keycode;
        entry.modifiers = modifiers;
        entries_.push_back(std::move(entry));
        return true;
    }

    void releaseAll() {
        std::thread worker;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_.store(false);
            if (display_ != nullptr && !entries_.empty()) {
                const Window root = DefaultRootWindow(display_);
                for (const auto& entry : entries_) {
                    const unsigned extras[] = {0u, Mod2Mask, LockMask,
                                               Mod2Mask | LockMask};
                    for (unsigned extra : extras) {
                        XUngrabKey(display_, static_cast<int>(entry.keycode),
                                   entry.modifiers | extra, root);
                    }
                }
                XSync(display_, False);
            }
            entries_.clear();
            worker = std::move(worker_);
        }
        if (worker.joinable()) {
            worker.join();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (display_ != nullptr) {
            XCloseDisplay(display_);
            display_ = nullptr;
        }
    }

private:
    struct Entry {
        std::string id;
        HotkeyCallback callback = nullptr;
        void* userData = nullptr;
        KeyCode keycode = 0;
        unsigned modifiers = 0;
    };

    /// Xlib 错误是异步投递的：先清零本槽位，再做批量 XGrabKey + XSync，
    /// 期间若有 BadAccess 会被处理器写入，从而把"热键被占用"变成可检测的返回值。
    static int& errorSlot() {
        static int value = 0;
        return value;
    }

    static int x11ErrorHandler(Display*, XErrorEvent* event) {
        errorSlot() = event != nullptr ? event->error_code : -1;
        return 0;
    }

    static KeySym toKeysym(Key key) {
        switch (key) {
            case Key::A: return XK_A; case Key::B: return XK_B;
            case Key::C: return XK_C; case Key::D: return XK_D;
            case Key::E: return XK_E; case Key::F: return XK_F;
            case Key::G: return XK_G; case Key::H: return XK_H;
            case Key::I: return XK_I; case Key::J: return XK_J;
            case Key::K: return XK_K; case Key::L: return XK_L;
            case Key::M: return XK_M; case Key::N: return XK_N;
            case Key::O: return XK_O; case Key::P: return XK_P;
            case Key::Q: return XK_Q; case Key::R: return XK_R;
            case Key::S: return XK_S; case Key::T: return XK_T;
            case Key::U: return XK_U; case Key::V: return XK_V;
            case Key::W: return XK_W; case Key::X: return XK_X;
            case Key::Y: return XK_Y; case Key::Z: return XK_Z;
            case Key::Num0: return XK_0; case Key::Num1: return XK_1;
            case Key::Num2: return XK_2; case Key::Num3: return XK_3;
            case Key::Num4: return XK_4; case Key::Num5: return XK_5;
            case Key::Num6: return XK_6; case Key::Num7: return XK_7;
            case Key::Num8: return XK_8; case Key::Num9: return XK_9;
            case Key::F1: return XK_F1; case Key::F2: return XK_F2;
            case Key::F3: return XK_F3; case Key::F4: return XK_F4;
            case Key::F5: return XK_F5; case Key::F6: return XK_F6;
            case Key::F7: return XK_F7; case Key::F8: return XK_F8;
            case Key::F9: return XK_F9; case Key::F10: return XK_F10;
            case Key::F11: return XK_F11; case Key::F12: return XK_F12;
            case Key::Space: return XK_space;
            case Key::Tab: return XK_Tab;
            case Key::Return: return XK_Return;
            case Key::Escape: return XK_Escape;
            case Key::Backspace: return XK_BackSpace;
            case Key::Delete: return XK_Delete;
            case Key::Insert: return XK_Insert;
            case Key::Home: return XK_Home;
            case Key::End: return XK_End;
            case Key::Left: return XK_Left;
            case Key::Right: return XK_Right;
            case Key::Up: return XK_Up;
            case Key::Down: return XK_Down;
            case Key::PageUp: return XK_Page_Up;
            case Key::PageDown: return XK_Page_Down;
            case Key::Minus: return XK_minus;
            case Key::Equal: return XK_equal;
            case Key::BracketLeft: return XK_bracketleft;
            case Key::BracketRight: return XK_bracketright;
            case Key::Backslash: return XK_backslash;
            case Key::Semicolon: return XK_semicolon;
            case Key::Apostrophe: return XK_apostrophe;
            case Key::Comma: return XK_comma;
            case Key::Period: return XK_period;
            case Key::Slash: return XK_slash;
            case Key::Grave: return XK_grave;
            case Key::F13: return XK_F13;
            case Key::F14: return XK_F14;
            case Key::F15: return XK_F15;
            case Key::F16: return XK_F16;
            case Key::F17: return XK_F17;
            case Key::F18: return XK_F18;
            case Key::F19: return XK_F19;
            case Key::F20: return XK_F20;
            case Key::Unknown: break;
        }
        return NoSymbol;
    }

    static unsigned toX11Modifiers(unsigned modifiers) {
        unsigned out = 0;
        if (modifiers & kModCtrl)  { out |= ControlMask; }
        if (modifiers & kModAlt)   { out |= Mod1Mask; }
        if (modifiers & kModShift) { out |= ShiftMask; }
        if (modifiers & kModSuper) { out |= Mod4Mask; }
        return out;
    }

    void workerLoop() {
        for (;;) {
            if (!running_.load()) {
                return;
            }

            // 第一步：先取走 Xlib 已缓冲的事件（XPending 顺带读取 socket），
            // 再进入 select 等待。反过来的顺序会漏掉"已到达但未被读走"的事件。
            std::vector<Entry> matched;
            int fd = -1;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (display_ == nullptr) {
                    return;
                }
                fd = ConnectionNumber(display_);
                while (XPending(display_) > 0) {
                    XEvent event;
                    XNextEvent(display_, &event);
                    if (event.type != KeyPress) {
                        continue;
                    }
                    const unsigned state = event.xkey.state & ~(Mod2Mask | LockMask);
                    for (const auto& entry : entries_) {
                        if (entry.keycode == event.xkey.keycode &&
                            entry.modifiers == state) {
                            matched.push_back(entry);
                        }
                    }
                }
            }
            // 在锁外触发回调，避免占着 X 连接执行用户代码。
            for (const auto& entry : matched) {
                if (entry.callback != nullptr) {
                    entry.callback(entry.id, entry.userData);
                }
            }

            // 第二步：等待新事件；超时 100ms 以保证能及时响应停止请求。
            if (fd >= 0) {
                fd_set readable;
                FD_ZERO(&readable);
                FD_SET(fd, &readable);
                timeval timeout{};
                timeout.tv_usec = 100 * 1000;
                select(fd + 1, &readable, nullptr, nullptr, &timeout);
            }
        }
    }

    Display* display_ = nullptr;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::vector<Entry> entries_;
};

/// X11 抓屏。
///
/// 要点：
///   - 以 root window 为抓取目标，其坐标即虚拟桌面坐标（X11 下从 0,0 起）。
///   - XGetImage 为同步请求，返回时像素已就绪；每次调用独立打开连接，
///     避免与热键监听线程共享 Xlib 连接（Xlib 连接非线程安全）。
///   - 只支持小端机器上的 24/32 bpp 真彩色（目标平台 aarch64 / x86_64 均满足）；
///     其它情况如实报错而不是给出错色图像。
class X11Capture {
public:
    static std::vector<DisplayInfo> displays() {
        std::vector<DisplayInfo> out;
        Display* display = XOpenDisplay(nullptr);
        if (display == nullptr) {
            return out;
        }
        const Window root = DefaultRootWindow(display);

#if defined(JIE2_HAVE_XRANDR)
        int eventBase = 0;
        int errorBase = 0;
        if (XRRQueryExtension(display, &eventBase, &errorBase) != 0) {
            XRRScreenResources* resources =
                XRRGetScreenResourcesCurrent(display, root);
            if (resources != nullptr) {
                for (int i = 0; i < resources->noutput; ++i) {
                    XRROutputInfo* output =
                        XRRGetOutputInfo(display, resources, resources->outputs[i]);
                    if (output == nullptr) {
                        continue;
                    }
                    if (output->connection == RR_Connected && output->crtc != 0) {
                        XRRCrtcInfo* crtc =
                            XRRGetCrtcInfo(display, resources, output->crtc);
                        if (crtc != nullptr) {
                            DisplayInfo info;
                            info.id = output->name != nullptr
                                          ? output->name
                                          : std::to_string(i);
                            info.name = info.id;
                            info.bounds = core::Rect{
                                static_cast<int>(crtc->x),
                                static_cast<int>(crtc->y),
                                static_cast<int>(crtc->width),
                                static_cast<int>(crtc->height)};
                            // X11 无系统级全局缩放：物理像素与坐标一一对应。
                            info.scaleFactor = 1.0;
                            info.isPrimary = (crtc->x == 0 && crtc->y == 0);
                            out.push_back(std::move(info));
                            XRRFreeCrtcInfo(crtc);
                        }
                    }
                    XRRFreeOutputInfo(output);
                }
                XRRFreeScreenResources(resources);
            }
        }
#endif

        if (out.empty()) {
            // 无 XRandR 或枚举失败：把整块虚拟桌面作为单台显示器报告。
            const int screen = DefaultScreen(display);
            DisplayInfo info;
            info.id = ":root";
            info.name = "Virtual desktop";
            info.bounds = core::Rect{0, 0, DisplayWidth(display, screen),
                                     DisplayHeight(display, screen)};
            info.scaleFactor = 1.0;
            info.isPrimary = true;
            out.push_back(std::move(info));
        }
        XCloseDisplay(display);
        return out;
    }

    static CaptureResult capture(const core::Rect& region) {
        if (region.isEmpty()) {
            return CaptureResult::failure("截图区域为空");
        }
        Display* display = XOpenDisplay(nullptr);
        if (display == nullptr) {
            return CaptureResult::failure("无法连接 X 服务器（XOpenDisplay 失败）");
        }
        const int screen = DefaultScreen(display);
        const Window root = RootWindow(display, screen);
        const int screenWidth = DisplayWidth(display, screen);
        const int screenHeight = DisplayHeight(display, screen);

        const core::Rect clipped =
            region.intersected(core::Rect{0, 0, screenWidth, screenHeight});
        if (clipped.isEmpty()) {
            XCloseDisplay(display);
            return CaptureResult::failure("截图区域超出屏幕范围");
        }

        XImage* image = XGetImage(display, root, clipped.x, clipped.y,
                                  static_cast<unsigned>(clipped.width),
                                  static_cast<unsigned>(clipped.height),
                                  AllPlanes, ZPixmap);
        if (image == nullptr) {
            XCloseDisplay(display);
            return CaptureResult::failure("XGetImage 失败（区域不可读或内存不足）");
        }

        // 排障开关：JIE2_X11_DEBUG=1 时输出 XImage 的底层布局，
        // 用于核对位深 / 字节序 / 掩码（不同 X 服务器实现可能有差异）。
        if (std::getenv("JIE2_X11_DEBUG") != nullptr) {
            std::fprintf(
                stderr,
                "[x11] bpp=%d depth=%d byte_order=%d bpl=%d masks r=%08lx g=%08lx b=%08lx\n",
                image->bits_per_pixel, image->depth, image->byte_order,
                image->bytes_per_line,
                static_cast<unsigned long>(image->red_mask),
                static_cast<unsigned long>(image->green_mask),
                static_cast<unsigned long>(image->blue_mask));
            unsigned char probe[16] = {0};
            const int probeLen = image->bytes_per_line < 16 ? image->bytes_per_line : 16;
            std::memcpy(probe, image->data, static_cast<std::size_t>(probeLen));
            std::fprintf(stderr, "[x11] first bytes:");
            for (int i = 0; i < probeLen; ++i) {
                std::fprintf(stderr, " %02x", probe[i]);
            }
            std::fprintf(stderr, "\n");
        }

        core::ImageBuffer buffer(clipped.width, clipped.height,
                                 core::PixelFormat::BGRA8888);
        bool ok = true;
        if (image->bits_per_pixel == 32) {
            // 小端下 XImage 32bpp 的内存序为 B,G,R,X —— 与 BGRA8888 一致，整行拷贝。
            // 第 4 字节（X）在 X11 中未定义（实测恒为 0），截图内容应视为不透明，
            // 因此逐行修正 alpha=255；否则下游按 alpha 合成会得到全透明图。
            for (int y = 0; y < clipped.height; ++y) {
                const char* src = image->data + static_cast<std::ptrdiff_t>(y) * image->bytes_per_line;
                unsigned char* dst = buffer.scanline(y);
                std::memcpy(dst, src, static_cast<std::size_t>(clipped.width) * 4);
                for (int x = 0; x < clipped.width; ++x) {
                    dst[x * 4 + 3] = 255;
                }
            }
        } else if (image->bits_per_pixel == 24) {
            for (int y = 0; y < clipped.height; ++y) {
                const unsigned char* src = reinterpret_cast<const unsigned char*>(
                    image->data + static_cast<std::ptrdiff_t>(y) * image->bytes_per_line);
                unsigned char* dst = buffer.scanline(y);
                for (int x = 0; x < clipped.width; ++x) {
                    dst[x * 4 + 0] = src[x * 3 + 0];
                    dst[x * 4 + 1] = src[x * 3 + 1];
                    dst[x * 4 + 2] = src[x * 3 + 2];
                    dst[x * 4 + 3] = 255;
                }
            }
        } else {
            ok = false;
        }

        XDestroyImage(image);
        XCloseDisplay(display);

        if (!ok) {
            return CaptureResult::failure("不支持的屏幕像素深度（仅支持 24/32 bpp）");
        }
        return CaptureResult::success(std::move(buffer));
    }

    /// 枚举适合吸附的顶层窗口（root 坐标，即虚拟桌面像素坐标）。
    ///
    /// 过滤规则（宁缺毋滥，错误的吸附比不吸附更烦人）：
    ///   - 必须 IsViewable（已映射且在屏上）
    ///   - 排除 override-redirect（菜单、提示、我们自己的测试靶窗等）
    ///   - 排除过小的窗口（< 40x40）
    static std::vector<core::Rect> windows() {
        std::vector<core::Rect> result;
        Display* display = XOpenDisplay(nullptr);
        if (display == nullptr) {
            return result;
        }
        const Window root = DefaultRootWindow(display);
        Window rootReturn = 0;
        Window parentReturn = 0;
        Window* children = nullptr;
        unsigned int childCount = 0;
        if (XQueryTree(display, root, &rootReturn, &parentReturn, &children,
                       &childCount) != 0) {
            for (unsigned int i = 0; i < childCount; ++i) {
                XWindowAttributes attrs{};
                if (XGetWindowAttributes(display, children[i], &attrs) == 0) {
                    continue;
                }
                if (attrs.map_state != IsViewable ||
                    attrs.override_redirect != 0 ||
                    attrs.width < 40 || attrs.height < 40) {
                    continue;
                }
                // 顶层窗口坐标 → root 坐标（无 WM 时等同，有 WM 时补偿边框/重定位）。
                int rootX = 0;
                int rootY = 0;
                Window childReturn = 0;
                if (XTranslateCoordinates(display, children[i], root, 0, 0,
                                          &rootX, &rootY, &childReturn) == 0) {
                    continue;
                }
                result.push_back(core::Rect{rootX, rootY, attrs.width, attrs.height});
            }
        }
        if (children != nullptr) {
            XFree(children);
        }
        XCloseDisplay(display);
        return result;
    }
};

#endif // JIE2_HAVE_X11

} // namespace

class LinuxPlatform final : public Platform {
public:
    std::string name() const override { return "linux"; }

    core::PlatformCapabilities probe() const override {
        const SessionProbe session = detectSession();
        core::PlatformCapabilities caps;
        caps.session = session.type;
        caps.sessionDetail = session.detail;

        switch (session.type) {
            case core::SessionType::X11:
                caps.screenCapture = {core::CapabilityStatus::Available,
                                      "X11 XGetImage / XShm（直通抓屏，无授权弹窗）"};
#if defined(JIE2_HAVE_X11)
                caps.globalHotkey = {core::CapabilityStatus::Available,
                                     "X11 XGrabKey（已编译 X11 后端）"};
#else
                caps.globalHotkey = {core::CapabilityStatus::Unavailable,
                                     "本次构建未包含 X11 后端（缺少 X11 开发库）"};
#endif
                caps.alwaysOnTop = {core::CapabilityStatus::Available,
                                    "override-redirect + _NET_WM_STATE_ABOVE"};
                caps.layerShell = {core::CapabilityStatus::NotApplicable,
                                   "X11 会话用普通置顶窗实现顶部条"};
                caps.clipboardImage = {core::CapabilityStatus::Available,
                                       "X11 剪贴板（M4 接入）"};
                break;

            case core::SessionType::Wayland:
#if defined(JIE2_HAVE_PORTAL_SCREENSHOT)
                {
                    // 抓屏：真实查询截图门户（非交互抓取可能弹授权提示 ——
                    // 这是 portal 的安全模型，如实标注为"降级"而非可用/不可用）。
                    std::string captureDetail;
                    if (WaylandScreenshotPortal::serviceAvailable(&captureDetail)) {
                        caps.screenCapture = {
                            core::CapabilityStatus::Degraded,
                            "经门户截图（可能弹出一次授权提示）：" + captureDetail};
                    } else {
                        caps.screenCapture = {
                            core::CapabilityStatus::Unavailable,
                            "截图门户不可用：" + captureDetail};
                    }
                }
#else
                caps.screenCapture = {
                    core::CapabilityStatus::Unavailable,
                    "本次构建未启用截图门户（需要 libdbus-1 与 zlib 开发库）"};
#endif
#if defined(JIE2_HAVE_DBUS)
                // 全局热键：走 portal GlobalShortcuts（真实探测服务可用性）。
                {
                    std::string detail;
                    if (WaylandPortalHotkeys::serviceAvailable(&detail)) {
                        caps.globalHotkey = {core::CapabilityStatus::Available,
                                             "xdg-desktop-portal GlobalShortcuts：" +
                                                 detail};
                    } else {
                        caps.globalHotkey = {core::CapabilityStatus::Unavailable,
                                             "GlobalShortcuts 门户不可用：" + detail};
                    }
                }
#else
                caps.globalHotkey = {core::CapabilityStatus::Unavailable,
                                     "本次构建未包含 D-Bus 后端（缺少 libdbus-1）"};
#endif
                caps.alwaysOnTop = {core::CapabilityStatus::Degraded,
                                    "取决于合成器：KDE/wlroots 可经 layer-shell 实现；"
                                    "GNOME 无 always-on-top 协议"};
                caps.layerShell = {core::CapabilityStatus::Degraded,
                                   "wlr-layer-shell 仅 KDE/wlroots/Mir 支持，"
                                   "GNOME 不支持；实现计划 M3"};
                caps.clipboardImage = {core::CapabilityStatus::Available,
                                       "Wayland 剪贴板（M4 接入）"};
                break;

            case core::SessionType::Unknown:
            default:
                caps.screenCapture = {core::CapabilityStatus::Unknown,
                                      "未检测到桌面会话"};
                caps.globalHotkey = {core::CapabilityStatus::Unknown,
                                     "未检测到桌面会话"};
                caps.alwaysOnTop = {core::CapabilityStatus::Unknown, ""};
                caps.layerShell = {core::CapabilityStatus::Unknown, ""};
                caps.clipboardImage = {core::CapabilityStatus::Unknown, ""};
                break;
        }
        return caps;
    }

    bool grabHotkey(const HotkeySpec& spec, HotkeyCallback callback,
                    void* userData, std::string* error) override {
        const core::SessionType session = detectSession().type;

        // Wayland：走 portal GlobalShortcuts（若构建带 D-Bus 支持）。
        if (session == core::SessionType::Wayland) {
#if defined(JIE2_HAVE_DBUS)
            return portalHotkeys_.grab(spec, callback, userData, error);
#else
            if (error != nullptr) {
                *error = "当前为 Wayland 会话，但本次构建未包含 D-Bus 后端"
                         "（缺少 libdbus-1 开发库）";
            }
            return false;
#endif
        }

#if defined(JIE2_HAVE_X11)
        if (session != core::SessionType::X11) {
            if (error != nullptr) {
                *error = "当前会话既非 X11 也非 Wayland，无法注册全局热键";
            }
            return false;
        }
        return hotkeys_.grab(spec, callback, userData, error);
#else
        (void)spec;
        (void)callback;
        (void)userData;
        if (error != nullptr) {
            *error = "本次构建未包含 X11 后端（缺少 X11 开发库）";
        }
        return false;
#endif
    }

    void releaseAllHotkeys() override {
#if defined(JIE2_HAVE_X11)
        hotkeys_.releaseAll();
#endif
#if defined(JIE2_HAVE_DBUS)
        portalHotkeys_.releaseAll();
#endif
    }

    // 遮罩窗口层级提升（远程桌面 / 全屏应用场景）。
    //
    // X11 下窗口层级由窗口管理器按 EWMH 决定：Qt 的 StaysOnTop 只映射到
    // _NET_WM_STATE_ABOVE，而全屏应用请求的是 _NET_WM_STATE_FULLSCREEN，
    // 部分 WM（含国产桌面常见的 kwin/mutter/openbox 组合）会把全屏窗口
    // 置于 ABOVE 之上。这里补齐三件事：
    //   ① 追加 FULLSCREEN + ABOVE 状态（让 WM 认可我们也在最上层）；
    //   ② 直接 XRaiseWindow（对不严格遵守 EWMH 的 WM 兜底）；
    //   ③ XSetInputFocus 抢键盘焦点（全局热键触发时本进程未必是焦点）。
    // Wayland 合成器不允许客户端自定层级，如实返回 false。
    bool promoteOverlayWindow(void* nativeWindow) override {
#if defined(JIE2_HAVE_X11)
        if (detectSession().type != core::SessionType::X11) {
            return false;
        }
        Display* display = XOpenDisplay(nullptr);
        if (display == nullptr) {
            return false;
        }
        const Window window = static_cast<Window>(
            reinterpret_cast<std::uintptr_t>(nativeWindow));
        bool ok = false;
        if (window != 0) {
            XRaiseWindow(display, window);
            // 通过 WM 协议追加状态（直接改属性对受管窗口无效）。
            const Atom wmState = XInternAtom(display, "_NET_WM_STATE", False);
            const Atom above = XInternAtom(display, "_NET_WM_STATE_ABOVE", False);
            const Atom fullscreen =
                XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
            if (wmState != None && above != None) {
                XEvent event{};
                event.xclient.type = ClientMessage;
                event.xclient.window = window;
                event.xclient.message_type = wmState;
                event.xclient.format = 32;
                // 1 = _NET_WM_STATE_ADD
                event.xclient.data.l[0] = 1;
                event.xclient.data.l[1] = static_cast<long>(above);
                event.xclient.data.l[2] = static_cast<long>(fullscreen);
                event.xclient.data.l[3] = 2;  // 来源：普通应用
                XSendEvent(display, DefaultRootWindow(display), False,
                           SubstructureRedirectMask | SubstructureNotifyMask,
                           &event);
                XFlush(display);
            }
            XSetInputFocus(display, window, RevertToParent, CurrentTime);
            XFlush(display);
            ok = true;
        }
        XCloseDisplay(display);
        return ok;
#else
        (void)nativeWindow;
        return false;
#endif
    }

    // ---- 抓屏（M1）----

    std::vector<DisplayInfo> displays() const override {
#if defined(JIE2_HAVE_X11)
        if (detectSession().type == core::SessionType::X11) {
            return X11Capture::displays();
        }
#endif
        // Wayland / 无会话：枚举不可用或未实现，返回空并由错误信息说明原因。
        return {};
    }

    CaptureResult captureRegion(const core::Rect& region) const override {
#if defined(JIE2_HAVE_X11)
        if (detectSession().type == core::SessionType::X11) {
            return X11Capture::capture(region);
        }
#endif
        switch (detectSession().type) {
            case core::SessionType::Wayland:
#if defined(JIE2_HAVE_PORTAL_SCREENSHOT)
                // 门户全屏图 + 按 (0,0) 坐标裁剪。
                // 假定说明：门户非交互返回的通常是主屏/合成全屏，其像素原点
                // 为 (0,0)。单显示器与"先整屏截取、再让用户选区域"的流程
                // 下该假定成立；越界时如实报错，不猜测映射。
                return portalCaptureRegion(region);
#else
                return CaptureResult::failure(
                    "Wayland 会话需要截图门户（本次构建未启用：需 libdbus-1 与 zlib）");
#endif
            case core::SessionType::Unknown:
                return CaptureResult::failure(
                    "未检测到桌面会话（DISPLAY / WAYLAND_DISPLAY 均不存在）");
            default:
                break;
        }
        return CaptureResult::failure("当前会话没有可用的抓屏后端");
    }

    CaptureResult captureDisplay(const std::string& displayId) const override {
        for (const auto& display : displays()) {
            if (display.id == displayId) {
                return captureRegion(display.bounds);
            }
        }
        if (detectSession().type == core::SessionType::Wayland) {
            // Wayland 下无法枚举显示器 id —— 无法把 id 解析到目标屏。
            // 不猜（把 id 指向的屏抓成别的屏比失败更糟），如实报错。
            return CaptureResult::failure(
                "Wayland 会话下无法枚举显示器（合成器不暴露），"
                "无法按 id 定位；请使用整屏抓取");
        }
        return CaptureResult::failure("未找到显示器：" + displayId);
    }

    CaptureResult captureAll() const override {
        // Wayland：显示器枚举不可用（合成器不暴露），直接走门户抓全屏。
        if (detectSession().type == core::SessionType::Wayland) {
#if defined(JIE2_HAVE_PORTAL_SCREENSHOT)
            return portalCaptureFullscreen();
#else
            return CaptureResult::failure(
                "Wayland 会话需要截图门户（本次构建未启用：需 libdbus-1 与 zlib）");
#endif
        }
        const auto all = displays();
        if (all.empty()) {
            return CaptureResult::failure("未找到可用显示器");
        }
        int minX = all.front().bounds.x;
        int minY = all.front().bounds.y;
        int maxX = all.front().bounds.right();
        int maxY = all.front().bounds.bottom();
        for (const auto& display : all) {
            minX = std::min(minX, display.bounds.x);
            minY = std::min(minY, display.bounds.y);
            maxX = std::max(maxX, display.bounds.right());
            maxY = std::max(maxY, display.bounds.bottom());
        }
        return captureRegion(core::Rect{minX, minY, maxX - minX, maxY - minY});
    }

    std::vector<core::Rect> windowRects() const override {
#if defined(JIE2_HAVE_X11)
        if (detectSession().type == core::SessionType::X11) {
            return X11Capture::windows();
        }
#endif
        // Wayland 下合成器不向应用暴露其它窗口的几何：如实返回空。
        return {};
    }

    core::OcrResult recognizeText(const core::ImageBuffer&,
                                  const std::string&) const override {
        // Linux 没有统一的内置 OCR 系统能力（GNOME/KDE 各自零散）。
        // 如实报告，并指引到可用的后端 —— 不静默失败。
        return core::OcrResult::failure(
            "Linux 无系统内置文字识别；请在配置中选择 tesseract 后端"
            "（ocr.backend=tesseract）或自定义命令/OpenAI 兼容端点",
            "system");
    }

private:
#if defined(JIE2_HAVE_PORTAL_SCREENSHOT)
    // ---- Wayland 抓屏（portal 路径）----

    /// 经门户抓全屏（非交互模式）。失败原因原样带回。
    CaptureResult portalCaptureFullscreen() const {
        WaylandScreenshotPortal portal;
        core::ImageBuffer image;
        std::string error;
        if (!portal.captureFullscreen(&image, &error)) {
            return CaptureResult::failure("Wayland 门户抓屏失败：" + error);
        }
        return CaptureResult::success(std::move(image));
    }

    /// 门户全屏图 + 裁剪到请求区域。
    /// 坐标假定：图像以 (0,0) 为原点（单显示器/先整屏截取再选区域的流程下
    /// 成立）；越界如实报错，不猜测映射。
    CaptureResult portalCaptureRegion(const core::Rect& region) const {
        if (region.width <= 0 || region.height <= 0) {
            return CaptureResult::failure("截图区域为空");
        }
        CaptureResult full = portalCaptureFullscreen();
        if (!full.ok) {
            return full;
        }
        const core::ImageBuffer& image = full.image;
        if (region.x < 0 || region.y < 0 || region.right() > image.width() ||
            region.bottom() > image.height()) {
            return CaptureResult::failure(
                "请求区域超出门户截图范围（区域 " +
                std::to_string(region.x) + "," + std::to_string(region.y) + " " +
                std::to_string(region.width) + "x" +
                std::to_string(region.height) + "，图像 " +
                std::to_string(image.width()) + "x" +
                std::to_string(image.height()) + "）");
        }
        return CaptureResult::success(core::crop(image, region));
    }

#if defined(JIE2_HAVE_X11)
    X11HotkeyListener hotkeys_;
#endif
    // Wayland 全局热键（portal GlobalShortcuts）。懒初始化：
    // 只有真的在 Wayland 会话里 grab 时才会连接 D-Bus。
    WaylandPortalHotkeys portalHotkeys_;
#else
#if defined(JIE2_HAVE_X11)
    X11HotkeyListener hotkeys_;
#endif
#endif
};

std::unique_ptr<Platform> createLinuxPlatform() {
    return std::make_unique<LinuxPlatform>();
}

} // namespace jie2::platform
