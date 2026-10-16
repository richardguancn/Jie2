// 「截2」Wayland 顶部条（layer-shell 衣架渲染进程）
//
// 背景与定位（评估报告 3.2 / B 项）：
//   Wayland 会话下，普通 xdg-toplevel 窗口**不能定位**（合成器不接受
//   move 请求），也没有 always-on-top 协议 —— 所以 Qt 衣架窗口在
//   Wayland 上只能降级。要做"贴屏幕顶边的条带"，正确的协议是
//   wlr-layer-shell（gnome 不支持，KDE/wlroots 支持）。
//
// 为什么是独立进程：
//   Qt6 不暴露底层 wl_surface，无法给 Qt 窗口设置 layer 角色
//   （LayerShellQt 是 Qt5）。因此本程序是纯 wayland-client 的小型
//   渲染器：主程序把"要挂的图"写进 items 文件，本进程轮询文件变化
//   并重绘。数据是单向的（主 -> 渲染），不引入双向协议复杂度。
//
// 使用：
//   jie2-line [--items FILE] [--height N] [--timeout-ms N] [--once]
//     --items FILE      条目文件（每行一个 PNG 路径）；默认
//                       $XDG_CONFIG_HOME/jie2/line-items.txt
//     --height N        条带高度（默认 150，与 Qt 衣架一致）
//     --timeout-ms N    运行 N 毫秒后自动退出（0 = 常驻；测试用）
//     --once            渲染一帧后退出（CI 用）
//
// 渲染内容：深色条带 + 顶部"晾衣绳" + 每张图的缩略图卡片。

#include <wayland-client.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

#include "core/image_buffer.h"
#include "core/image_ops.h"
#include "platform/linux/png_decode.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

namespace {

namespace core = jie2::core;

// ---- 视觉参数（与 Qt 衣架一致，便于对照）----
constexpr int kStripHeightDefault = 150;
constexpr int kCardWidth = 150;
constexpr int kCardHeight = 104;
constexpr int kCardGap = 14;
constexpr int kMarginLeft = 16;
constexpr int kRopeY = 14;

// 主题（v2.6：主题色跟随系统主题）——
// 主进程解析主题后通过环境变量 JIE2_THEME=dark|light 传给本进程；
// 未设置时保持既有深色观感。
struct Palette {
    std::uint32_t bg;
    std::uint32_t rope;
    std::uint32_t cardBg;
    std::uint32_t emptyMark;
};

constexpr Palette kDarkPalette{
    0xE61A1A1E,   // (26,26,30,230)
    0xFF9696A0,   // (150,150,160,255)
    0xFF28282E,   // (40,40,46,255)
    0xFF78787E,
};
constexpr Palette kLightPalette{
    0xF5F6F6FA,   // (246,246,250,245)
    0xFF787884,   // (120,120,132,255)
    0xFFE2E4EA,   // (226,228,234,255)
    0xFF3C3E46,   // (60,62,70,255)
};

const Palette* gPalette = &kDarkPalette;

/// 从环境变量解析主题（只认 dark/light，其余按深色）。
void applyThemeFromEnv() {
    const char* theme = std::getenv("JIE2_THEME");
    if (theme != nullptr && std::strcmp(theme, "light") == 0) {
        gPalette = &kLightPalette;
    } else {
        gPalette = &kDarkPalette;
    }
}

struct WlState {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    zwlr_layer_shell_v1* layerShell = nullptr;
    zwlr_layer_surface_v1* layerSurface = nullptr;
    wl_surface* surface = nullptr;
    wl_buffer* buffer = nullptr;
    void* shmData = nullptr;
    std::size_t shmSize = 0;
    int width = 0;
    int height = 0;
    bool configured = false;
    bool closed = false;
    bool bufferReleased = true;
};

// ---- registry ----
void onGlobal(void* data, wl_registry* registry, std::uint32_t name,
              const char* interface, std::uint32_t version) {
    auto* state = static_cast<WlState*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
        state->compositor = static_cast<wl_compositor*>(wl_registry_bind(
            registry, name, &wl_compositor_interface, std::min(version, 4u)));
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
        state->shm = static_cast<wl_shm*>(
            wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        state->layerShell = static_cast<zwlr_layer_shell_v1*>(wl_registry_bind(
            registry, name, &zwlr_layer_shell_v1_interface, 1));
    }
}

void onGlobalRemove(void*, wl_registry*, std::uint32_t) {}

const wl_registry_listener kRegistryListener = {onGlobal, onGlobalRemove};

// ---- layer surface ----
void onLayerConfigure(void* data, zwlr_layer_surface_v1* surface,
                      std::uint32_t serial, std::uint32_t width,
                      std::uint32_t height) {
    auto* state = static_cast<WlState*>(data);
    // 双缓冲配置必须先 ack 再提交表面状态。
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    // 我们请求了 size=(0, H) 且锚定 top|left|right：
    // 宽度由合成器给出；高度应当等于请求值（合成器可调整）。
    if (width > 0) {
        state->width = static_cast<int>(width);
    }
    state->height = height > 0 ? static_cast<int>(height) : state->height;
    state->configured = true;
    std::fprintf(stdout, "[jie2-line] configure %dx%d\n", state->width,
                 state->height);
    std::fflush(stdout);
}

void onLayerClosed(void* data, zwlr_layer_surface_v1*) {
    auto* state = static_cast<WlState*>(data);
    state->closed = true;
    std::fprintf(stdout, "[jie2-line] compositor 通知关闭\n");
    std::fflush(stdout);
}

const zwlr_layer_surface_v1_listener kLayerSurfaceListener = {
    onLayerConfigure, onLayerClosed};

// ---- buffer ----
void onBufferRelease(void* data, wl_buffer*) {
    auto* state = static_cast<WlState*>(data);
    state->bufferReleased = true;
}

const wl_buffer_listener kBufferListener = {onBufferRelease};

/// 创建一块 shm 支持的 ARGB8888 缓冲。
wl_buffer* createBuffer(WlState* state, int width, int height, void** dataOut,
                        std::string* error) {
    const int stride = width * 4;
    const std::size_t size = static_cast<std::size_t>(stride) * height;

    // 优先 memfd_create（无文件系统痕迹）；退回 /dev/shm。
    int fd = -1;
#ifdef MFD_CLOEXEC
    fd = memfd_create("jie2-line", MFD_CLOEXEC);
#endif
    if (fd < 0) {
        char tmpl[] = "/tmp/jie2-line-XXXXXX";
        fd = mkstemp(tmpl);
        if (fd >= 0) {
            unlink(tmpl);
        }
    }
    if (fd < 0) {
        *error = "无法创建共享内存（memfd/mkstemp 都失败）";
        return nullptr;
    }
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        *error = "ftruncate 失败";
        close(fd);
        return nullptr;
    }
    void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        *error = "mmap 失败";
        close(fd);
        return nullptr;
    }

    wl_shm_pool* pool = wl_shm_create_pool(state->shm, fd, static_cast<int>(size));
    wl_buffer* buffer = wl_shm_pool_create_buffer(
        pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);  // buffer 已持有引用

    // 清理旧缓冲。
    if (state->shmData != nullptr) {
        munmap(state->shmData, state->shmSize);
    }
    if (state->buffer != nullptr) {
        wl_buffer_destroy(state->buffer);
    }
    state->buffer = buffer;
    state->shmData = data;
    state->shmSize = size;
    state->bufferReleased = false;
    wl_buffer_add_listener(buffer, &kBufferListener, state);
    *dataOut = data;
    return buffer;
}

// ---- 绘制辅助 ----
inline void putPixel(std::uint32_t* pixels, int width, int x, int y,
                     std::uint32_t color) {
    if (x < 0 || y < 0 || x >= width) {
        return;
    }
    pixels[static_cast<std::size_t>(y) * width + x] = color;
}

/// 把图像以 "src over dst" 合成到目标缓冲（忽略目标 alpha 的简化混合，
/// 适用于"不透明底 + 可能带透明度的缩略图"这个场景）。
void blendImage(std::uint32_t* pixels, int bufferWidth, int originX,
                int originY, const core::ImageBuffer& image) {
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const core::Rgba src = image.pixel(x, y);
            const int dx = originX + x;
            const int dy = originY + y;
            if (dx < 0 || dy < 0 || dx >= bufferWidth) {
                continue;
            }
            std::uint32_t* dst =
                &pixels[static_cast<std::size_t>(dy) * bufferWidth + dx];
            const std::uint32_t dstColor = *dst;
            const std::uint32_t inv = 255 - src.a;
            const std::uint32_t r =
                (src.r * src.a + ((dstColor >> 16) & 0xFF) * inv) / 255;
            const std::uint32_t g =
                (src.g * src.a + ((dstColor >> 8) & 0xFF) * inv) / 255;
            const std::uint32_t b =
                (src.b * src.a + (dstColor & 0xFF) * inv) / 255;
            *dst = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

/// 读取 items 文件（每行一个路径）。不存在时返回空列表（不是错误）。
std::vector<std::string> readItems(const std::string& path) {
    std::vector<std::string> items;
    FILE* file = std::fopen(path.c_str(), "r");
    if (file == nullptr) {
        return items;
    }
    char line[4096];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        std::string text(line);
        while (!text.empty() &&
               (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
            text.pop_back();
        }
        if (!text.empty() && text[0] != '#') {
            items.push_back(text);
        }
        if (items.size() >= 12) {
            break;  // 与 Qt 衣架的上限一致
        }
    }
    std::fclose(file);
    return items;
}

std::int64_t fileMtime(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) {
        return -1;
    }
    return static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000000000 +
           st.st_mtim.tv_nsec;
}

std::string defaultItemsPath() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && xdg[0] != '\0') {
        return std::string(xdg) + "/jie2/line-items.txt";
    }
    const char* home = std::getenv("HOME");
    return std::string(home != nullptr ? home : "/tmp") +
           "/.config/jie2/line-items.txt";
}

/// 读取并解码一张 PNG；失败返回空图（不中断渲染）。
core::ImageBuffer loadThumbnail(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return {};
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0 || size > 64 * 1024 * 1024) {
        std::fclose(file);
        return {};
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    const std::size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    if (read != bytes.size()) {
        return {};
    }
    core::ImageBuffer image;
    std::string error;
    if (!jie2::platform::decodePng(bytes.data(), bytes.size(), &image, &error)) {
        std::fprintf(stderr, "[jie2-line] 解码失败 %s: %s\n", path.c_str(),
                     error.c_str());
        return {};
    }
    return image;
}

/// 渲染一帧：背景 + 晾衣绳 + 卡片与缩略图。
void renderFrame(WlState* state, const std::vector<std::string>& items) {
    if (state->width <= 0 || state->height <= 0) {
        return;
    }
    // 等待上一帧缓冲被合成器释放（最多 ~1 秒；超时则放弃本帧，避免撕裂）。
    for (int i = 0; i < 100 && !state->bufferReleased; ++i) {
        usleep(10 * 1000);
        wl_display_dispatch_pending(state->display);
        wl_display_flush(state->display);
    }
    if (!state->bufferReleased) {
        std::fprintf(stderr, "[jie2-line] 上一帧缓冲未释放，跳过本帧\n");
        return;
    }

    void* data = nullptr;
    std::string error;
    wl_buffer* buffer =
        createBuffer(state, state->width, state->height, &data, &error);
    if (buffer == nullptr) {
        std::fprintf(stderr, "[jie2-line] 创建缓冲失败：%s\n", error.c_str());
        return;
    }
    auto* pixels = static_cast<std::uint32_t*>(data);
    const int width = state->width;
    const int height = state->height;

    // 1) 背景
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            pixels[static_cast<std::size_t>(y) * width + x] = gPalette->bg;
        }
    }
    // 2) 晾衣绳
    for (int y = kRopeY; y < kRopeY + 2 && y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            pixels[static_cast<std::size_t>(y) * width + x] = gPalette->rope;
        }
    }

    // 3) 卡片与缩略图
    if (items.empty()) {
        // 空态：中线一小段标记
        for (int y = height / 2; y < height / 2 + 3; ++y) {
            for (int x = width / 2 - 30; x < width / 2 + 30; ++x) {
                putPixel(pixels, width, x, y, gPalette->emptyMark);
            }
        }
    } else {
        for (std::size_t i = 0; i < items.size(); ++i) {
            const int cardX =
                kMarginLeft + static_cast<int>(i) * (kCardWidth + kCardGap);
            const int cardY = 22;
            if (cardX >= width) {
                break;  // 超出屏幕宽度
            }
            // 卡片底
            for (int y = cardY; y < cardY + kCardHeight && y < height; ++y) {
                for (int x = cardX; x < cardX + kCardWidth && x < width; ++x) {
                    putPixel(pixels, width, x, y, gPalette->cardBg);
                }
            }
            // 挂线（绳子到卡片）
            const int centerX = cardX + kCardWidth / 2;
            for (int y = kRopeY + 2; y < cardY; ++y) {
                putPixel(pixels, width, centerX, y, gPalette->rope);
            }
            // 缩略图：等比缩放到卡片内（留 12px 边距）
            core::ImageBuffer image = loadThumbnail(items[i]);
            if (image.isEmpty()) {
                continue;
            }
            const int maxW = kCardWidth - 12;
            const int maxH = kCardHeight - 12;
            const double scale = std::min(
                static_cast<double>(maxW) / image.width(),
                static_cast<double>(maxH) / image.height());
            const int targetW = std::max(1, static_cast<int>(image.width() * scale));
            const int targetH = std::max(1, static_cast<int>(image.height() * scale));
            const core::ImageBuffer thumb =
                core::scaleNearest(image, targetW, targetH);
            blendImage(pixels, width,
                       cardX + (kCardWidth - targetW) / 2,
                       cardY + (kCardHeight - targetH) / 2, thumb);
        }
    }

    wl_surface_attach(state->surface, buffer, 0, 0);
    wl_surface_damage_buffer(state->surface, 0, 0, width, height);
    wl_surface_commit(state->surface);
    wl_display_flush(state->display);
    std::fprintf(stdout, "[jie2-line] 渲染 %dx%d，%zu 张\n", width, height,
                 items.size());
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv) {
    applyThemeFromEnv();  // 主题（v2.6）：由主进程经 JIE2_THEME 传入

    std::string itemsPath;
    int height = kStripHeightDefault;
    int timeoutMs = 0;
    bool once = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--items" && i + 1 < argc) {
            itemsPath = argv[++i];
        } else if (arg == "--height" && i + 1 < argc) {
            height = std::atoi(argv[++i]);
        } else if (arg == "--timeout-ms" && i + 1 < argc) {
            timeoutMs = std::atoi(argv[++i]);
        } else if (arg == "--once") {
            once = true;
        } else if (arg == "--help" || arg == "-h") {
            std::printf(
                "jie2-line —— Wayland layer-shell 顶部条（截2 衣架渲染进程）\n"
                "用法：jie2-line [--items FILE] [--height N] [--timeout-ms N] [--once]\n");
            return 0;
        } else {
            std::fprintf(stderr, "未知参数：%s（--help 查看用法）\n", arg.c_str());
            return 2;
        }
    }
    if (itemsPath.empty()) {
        itemsPath = defaultItemsPath();
    }
    if (height < 40 || height > 600) {
        std::fprintf(stderr, "--height 超出合理范围（40..600）\n");
        return 2;
    }

    WlState state;
    state.height = height;
    state.display = wl_display_connect(nullptr);
    if (state.display == nullptr) {
        std::fprintf(stderr,
                     "无法连接 Wayland（WAYLAND_DISPLAY=%s）。\n"
                     "本程序需要 wlroots/KDE 系的 Wayland 合成器（支持 "
                     "wlr-layer-shell）。GNOME 不支持该协议。\n",
                     std::getenv("WAYLAND_DISPLAY")
                         ? std::getenv("WAYLAND_DISPLAY")
                         : "(未设置)");
        return 1;
    }

    state.registry = wl_display_get_registry(state.display);
    wl_registry_add_listener(state.registry, &kRegistryListener, &state);
    wl_display_roundtrip(state.display);

    if (state.compositor == nullptr || state.shm == nullptr ||
        state.layerShell == nullptr) {
        std::fprintf(stderr,
                     "合成器缺少必需协议：compositor=%s shm=%s layer_shell=%s\n",
                     state.compositor ? "有" : "缺", state.shm ? "有" : "缺",
                     state.layerShell ? "有" : "缺");
        if (state.layerShell == nullptr) {
            std::fprintf(stderr,
                         "提示：wlr-layer-shell 只在 KDE/wlroots（Sway 等）"
                         "上可用；GNOME 不支持。\n");
        }
        return 3;
    }

    state.surface = wl_compositor_create_surface(state.compositor);
    state.layerSurface = zwlr_layer_shell_v1_get_layer_surface(
        state.layerShell, state.surface, nullptr, ZWLR_LAYER_SHELL_V1_LAYER_TOP,
        "jie2-line");
    zwlr_layer_surface_v1_add_listener(state.layerSurface,
                                       &kLayerSurfaceListener, &state);
    // size=(0, H)：宽度交给合成器（我们锚定 top|left|right）。
    zwlr_layer_surface_v1_set_size(state.layerSurface, 0,
                                   static_cast<std::uint32_t>(height));
    zwlr_layer_surface_v1_set_anchor(
        state.layerSurface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    // 悬浮不占位（对齐 Qt 衣架的语义）。
    zwlr_layer_surface_v1_set_exclusive_zone(state.layerSurface, 0);
    // 不接受键盘焦点（纯展示）。
    zwlr_layer_surface_v1_set_keyboard_interactivity(state.layerSurface, 0);
    wl_surface_commit(state.surface);

    while (!state.configured) {
        if (wl_display_dispatch(state.display) < 0) {
            std::fprintf(stderr, "事件循环失败（configure 前断开）\n");
            return 1;
        }
    }

    renderFrame(&state, readItems(itemsPath));

    if (once) {
        wl_display_roundtrip(state.display);
        wl_display_flush(state.display);
        return 0;
    }

    // 主循环：等 Wayland 事件 + 轮询 items 文件。
    std::int64_t lastMtime = fileMtime(itemsPath);
    struct timespec start{};
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!state.closed) {
        pollfd pfd{};
        pfd.fd = wl_display_get_fd(state.display);
        pfd.events = POLLIN;
        const int result = poll(&pfd, 1, 400);
        if (result > 0 && (pfd.revents & POLLIN) != 0) {
            if (wl_display_dispatch(state.display) < 0) {
                break;
            }
        } else if (result >= 0) {
            wl_display_dispatch_pending(state.display);
            wl_display_flush(state.display);
        } else if (errno != EINTR) {
            break;
        }

        const std::int64_t mtime = fileMtime(itemsPath);
        if (mtime != lastMtime) {
            lastMtime = mtime;
            renderFrame(&state, readItems(itemsPath));
        }

        if (timeoutMs > 0) {
            struct timespec now{};
            clock_gettime(CLOCK_MONOTONIC, &now);
            const long long elapsed =
                (now.tv_sec - start.tv_sec) * 1000LL +
                (now.tv_nsec - start.tv_nsec) / 1000000LL;
            if (elapsed > timeoutMs) {
                std::fprintf(stdout, "[jie2-line] 到达超时（%dms），退出\n",
                             timeoutMs);
                break;
            }
        }
    }

    std::fprintf(stdout, "[jie2-line] 退出\n");
    return 0;
}
