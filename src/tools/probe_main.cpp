// 「J2」CLI 探测工具（无 GUI 依赖）
//
// 用途：CI 验证、无桌面会话环境诊断、平台能力快速检查。
// 之所以独立于 GUI 可执行文件：让内核与平台层在没有 Qt / 没有桌面会话的
// 环境里也能被构建和运行 —— 这正是三层解耦要保证的事情之一。
//
// 参数：
//   --test-hotkey            注册全局热键并做重复注册检测
//   --wait-activation-ms N   注册热键后等待回调触发（验证 Activated 信号，
//                            Wayland portal 路径用；N 毫秒内未触发即失败）
//   --capture                抓取全部显示器合屏，输出尺寸与角点采样
//   --capture-save <file>    与 --capture 配合：把结果保存为 PPM(P6)
//   --displays               仅列出显示器
//   --windows                列出可吸附的窗口矩形（M1 窗口吸附验证）

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/capability.h"
#include "core/image_ops.h"
#include "platform/platform.h"

namespace {

// 热键回调：可能在任意平台线程触发（Wayland portal 的 D-Bus 线程、
// X11 监听线程），因此用原子量 + 互斥锁记录，主线程轮询读取。
std::atomic<int>& activationCount() {
    static std::atomic<int> count{0};
    return count;
}
std::mutex& activationMutex() {
    static std::mutex mutex;
    return mutex;
}
std::string& activationId() {
    static std::string id;
    return id;
}

void probeHotkeyCallback(const std::string& id, void*) {
    {
        std::lock_guard<std::mutex> lock(activationMutex());
        activationId() = id;
    }
    activationCount().fetch_add(1);
    std::printf("hotkey-activated: %s\n", id.c_str());
    std::fflush(stdout);
}

int testHotkey(jie2::platform::Platform& platform, int waitActivationMs) {
    jie2::platform::HotkeySpec spec;
    spec.id = "probe";
    spec.modifiers = jie2::platform::kModCtrl | jie2::platform::kModAlt;
    spec.key = jie2::platform::Key::T;

    std::string error;
    const bool ok =
        platform.grabHotkey(spec, &probeHotkeyCallback, nullptr, &error);
    std::printf("hotkey %s: %s", jie2::platform::describeHotkey(spec).c_str(),
                ok ? "ok" : "failed");
    if (!ok) {
        std::printf(" (%s)", error.c_str());
    }
    std::printf("\n");

    if (ok && waitActivationMs > 0) {
        // 等待回调（验证 Activated 信号链路：Wayland portal 的关键一跳）。
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(waitActivationMs);
        while (std::chrono::steady_clock::now() < deadline) {
            if (activationCount().load() > 0) {
                std::printf("wait-activation: fired (%d 次)\n",
                            activationCount().load());
                platform.releaseAllHotkeys();
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::printf("wait-activation: not-fired within %dms\n", waitActivationMs);
        platform.releaseAllHotkeys();
        return 1;
    }

    if (ok) {
        // 二次注册同一组合：若首次注册真的在系统层面生效，
        // 系统应拒绝重复注册（macOS 的 eventHotKeyExistsErr 即可作独立佐证）；
        // X11 下同一客户端的重复 grab 是合法 no-op。据平台语义解读即可。
        std::string duplicateError;
        const bool duplicate = platform.grabHotkey(
            spec, &probeHotkeyCallback, nullptr, &duplicateError);
        std::printf("duplicate-register: %s", duplicate ? "accepted" : "rejected");
        if (!duplicate) {
            std::printf(" (%s)", duplicateError.c_str());
        }
        std::printf("\n");
    }
    platform.releaseAllHotkeys();
    return ok ? 0 : 1;
}

void listWindows(jie2::platform::Platform& platform) {
    const auto windows = platform.windowRects();
    std::printf("windows: %zu\n", windows.size());
    for (const auto& window : windows) {
        std::printf("  %dx%d+%d+%d\n", window.width, window.height, window.x,
                    window.y);
    }
}

void listDisplays(jie2::platform::Platform& platform) {
    const auto displays = platform.displays();
    std::printf("displays: %zu\n", displays.size());
    for (const auto& display : displays) {
        std::printf("  %s (%s): x=%d y=%d %dx%d scale=%.2f%s\n",
                    display.id.c_str(), display.name.c_str(), display.bounds.x,
                    display.bounds.y, display.bounds.width, display.bounds.height,
                    display.scaleFactor, display.isPrimary ? " [primary]" : "");
    }
}

bool savePpm(const jie2::core::ImageBuffer& image, const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        std::printf("保存失败：无法写入 %s\n", path.c_str());
        return false;
    }
    std::fprintf(file, "P6\n%d %d\n255\n", image.width(), image.height());
    std::vector<unsigned char> row(static_cast<std::size_t>(image.width()) * 3);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const jie2::core::Rgba px = image.pixel(x, y);
            row[static_cast<std::size_t>(x) * 3 + 0] = px.r;
            row[static_cast<std::size_t>(x) * 3 + 1] = px.g;
            row[static_cast<std::size_t>(x) * 3 + 2] = px.b;
        }
        if (std::fwrite(row.data(), 1, row.size(), file) != row.size()) {
            std::fclose(file);
            std::printf("保存失败：写入中断 %s\n", path.c_str());
            return false;
        }
    }
    std::fclose(file);
    return true;
}

void samplePixel(const jie2::core::ImageBuffer& image, int x, int y,
                 const char* label) {
    const jie2::core::Rgba px = image.pixel(x, y);
    std::printf("  %-10s (%4d,%4d) = #%02X%02X%02X a=%d\n", label, x, y, px.r,
                px.g, px.b, px.a);
}

int capture(jie2::platform::Platform& platform, const std::string& savePath,
            const jie2::core::Rect* region) {
    const auto displays = platform.displays();
    std::printf("displays: %zu\n", displays.size());

    const jie2::platform::CaptureResult result =
        region != nullptr ? platform.captureRegion(*region) : platform.captureAll();
    if (!result.ok) {
        std::printf("capture: failed (%s)\n", result.error.c_str());
        return 1;
    }
    const jie2::core::ImageBuffer& image = result.image;
    std::printf("capture: ok %dx%d %s\n", image.width(), image.height(),
                image.format() == jie2::core::PixelFormat::BGRA8888 ? "bgra" : "rgba");

    if (!image.isEmpty()) {
        samplePixel(image, 0, 0, "top-left");
        samplePixel(image, image.width() - 1, 0, "top-right");
        samplePixel(image, image.width() / 2, image.height() / 2, "center");
        samplePixel(image, 0, image.height() - 1, "bottom-left");
    }

    if (!savePath.empty()) {
        if (!savePpm(image, savePath)) {
            return 1;
        }
        std::printf("saved: %s\n", savePath.c_str());
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    bool hotkey = false;
    bool doCapture = false;
    bool doDisplays = false;
    bool doWindows = false;
    std::string savePath;
    jie2::core::Rect region;
    bool hasRegion = false;

    int waitActivationMs = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--test-hotkey") == 0) {
            hotkey = true;
        } else if (std::strcmp(argv[i], "--wait-activation-ms") == 0 &&
                   i + 1 < argc) {
            waitActivationMs = std::atoi(argv[++i]);
            hotkey = true;  // 等待激活隐含注册热键
        } else if (std::strcmp(argv[i], "--capture") == 0) {
            doCapture = true;
        } else if (std::strcmp(argv[i], "--displays") == 0) {
            doDisplays = true;
        } else if (std::strcmp(argv[i], "--windows") == 0) {
            doWindows = true;
        } else if (std::strcmp(argv[i], "--capture-save") == 0 && i + 1 < argc) {
            savePath = argv[++i];
            doCapture = true;
        } else if (std::strcmp(argv[i], "--capture-region") == 0 && i + 1 < argc) {
            // 格式：x,y,w,h（平台全局坐标系）
            int x = 0;
            int y = 0;
            int w = 0;
            int h = 0;
            if (std::sscanf(argv[++i], "%d,%d,%d,%d", &x, &y, &w, &h) == 4) {
                region = jie2::core::Rect{x, y, w, h};
                hasRegion = true;
                doCapture = true;
            } else {
                std::printf("--capture-region 参数格式应为 x,y,w,h\n");
                return 2;
            }
        }
    }

    auto platform = jie2::platform::createPlatform();
    std::printf("jie2-probe %s\n", JIE2_VERSION);
    std::printf("implementation: %s\n", platform->name().c_str());

    const jie2::core::PlatformCapabilities caps = platform->probe();
    std::printf("%s", caps.describe().c_str());
    std::printf("has-limitations: %s\n", caps.hasLimitations() ? "true" : "false");

    // 显示器布局：抓屏与选择遮罩都依赖这套坐标，诊断时首先看它。
    const auto displays = platform->displays();
    std::printf("displays: %zu\n", displays.size());
    for (const auto& display : displays) {
        std::printf("  - %s [%s] %dx%d+%d+%d scale=%.2f%s\n",
                    display.id.c_str(), display.name.c_str(),
                    display.bounds.width, display.bounds.height,
                    display.bounds.x, display.bounds.y,
                    display.scaleFactor,
                    display.isPrimary ? " (primary)" : "");
    }

    int failures = 0;

    if (doDisplays) {
        listDisplays(*platform);
    }
    if (doWindows) {
        listWindows(*platform);
    }
    if (doCapture) {
        failures += capture(*platform, savePath, hasRegion ? &region : nullptr);
    }
    if (hotkey) {
        failures += testHotkey(*platform, waitActivationMs);
    }

    return failures == 0 ? 0 : 1;
}
