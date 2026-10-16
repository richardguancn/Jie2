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
//     --height N        条带高度（默认 210，与 Qt 衣架一致）
//     --timeout-ms N    运行 N 毫秒后自动退出（0 = 常驻；测试用）
//     --once            渲染一帧后退出（CI 用）
//
// 渲染内容（tendedero 样式，与 Qt 衣架一致）：透明面板 + 抛物线晾衣绳
// （两端淡出）+ 铝质衣夹 + 带玻璃边框与固定倾斜角的缩略图卡片。

#include <wayland-client.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
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

// ---- 视觉参数（与 Qt 衣架一致，全部对齐 tendedero 的 LineView/PeggedView）----
constexpr int kStripHeightDefault = 210;  // photoMaxHeight + 106
constexpr int kPhotoMaxWidth = 136;       // cardWidth(150) - 14
constexpr int kPhotoMaxHeight = 104;
constexpr int kCardSpacing = 174;         // cardWidth + 24
constexpr double kRopeTop = 10;
constexpr double kPinAbove = 9.5;         // 衣夹顶高出绳子的距离
constexpr double kClipWidth = 9;
constexpr double kClipHeight = 26;
constexpr double kCardBelowClipTop = 14;  // 衣夹与卡片重叠 12
constexpr int kFrameInset = 4;            // 玻璃边框厚度
constexpr double kFrameRadius = 16;
constexpr double kPhotoRadius = 12;       // 同心圆角：16 - 4

// 主题（v2.6：主题色跟随系统主题）——
// 主进程解析主题后通过环境变量 JIE2_THEME=dark|light 传给本进程；
// 未设置时保持既有深色观感。
// tendedero 的绳子与衣夹是中性色（深浅壁纸通用），主题只体现在
// 卡片玻璃底与空态标记上（ARGB）。
struct Palette {
    std::uint32_t cardGlass;
    std::uint32_t emptyMark;
};

constexpr Palette kDarkPalette{
    0x961E1E24,   // (30,30,36,150)
    0x9628282E,   // (40,40,46,150)
};
constexpr Palette kLightPalette{
    0xA5F5F6FB,   // (245,246,251,165)
    0xA5FAFAFD,   // (250,250,253,165)
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

// ---- 绘制辅助（透明底 + 逐像素 src-over）----

/// 逐像素 src-over 混合（面板是透明底，alpha 必须正确累积）。
/// 颜色参数：sr/sg/sb ∈ [0,255]，sa ∈ [0,1]。
void blendPixel(std::uint32_t* pixels, int width, int height, int x, int y,
                double sr, double sg, double sb, double sa) {
    if (sa <= 0.0 || x < 0 || y < 0 || x >= width || y >= height) {
        return;
    }
    std::uint32_t* dst = &pixels[static_cast<std::size_t>(y) * width + x];
    const double da = ((*dst >> 24) & 0xFF) / 255.0;
    const double dr = (*dst >> 16) & 0xFF;
    const double dg = (*dst >> 8) & 0xFF;
    const double db = *dst & 0xFF;
    const double oa = sa + da * (1.0 - sa);
    if (oa <= 0.0) {
        *dst = 0;
        return;
    }
    const double orr = (sr * sa + dr * da * (1.0 - sa)) / oa;
    const double org = (sg * sa + dg * da * (1.0 - sa)) / oa;
    const double orb = (sb * sa + db * da * (1.0 - sa)) / oa;
    *dst = (static_cast<std::uint32_t>(oa * 255.0 + 0.5) << 24) |
           (static_cast<std::uint32_t>(orr + 0.5) << 16) |
           (static_cast<std::uint32_t>(org + 0.5) << 8) |
           static_cast<std::uint32_t>(orb + 0.5);
}

/// 圆角矩形的有符号距离（负值在内部）。
double roundedRectSdf(double px, double py, double x, double y, double w,
                      double h, double radius) {
    const double qx = std::fabs(px - (x + w / 2)) - (w / 2 - radius);
    const double qy = std::fabs(py - (y + h / 2)) - (h / 2 - radius);
    const double ax = std::max(qx, 0.0);
    const double ay = std::max(qy, 0.0);
    return std::sqrt(ax * ax + ay * ay) + std::min(std::max(qx, qy), 0.0) -
           radius;
}

/// 填充圆角矩形（1px 抗锯齿边缘）。
void fillRounded(std::uint32_t* buf, int bw, int bh, double x, double y,
                 double w, double h, double radius, double r, double g,
                 double b, double a) {
    const int x0 = std::max(0, static_cast<int>(std::floor(x - 1)));
    const int y0 = std::max(0, static_cast<int>(std::floor(y - 1)));
    const int x1 = std::min(bw, static_cast<int>(std::ceil(x + w + 1)));
    const int y1 = std::min(bh, static_cast<int>(std::ceil(y + h + 1)));
    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            const double d =
                roundedRectSdf(px + 0.5, py + 0.5, x, y, w, h, radius);
            const double coverage =
                std::clamp(0.5 - d, 0.0, 1.0);
            if (coverage > 0.0) {
                blendPixel(buf, bw, bh, px, py, r, g, b, a * coverage);
            }
        }
    }
}

/// 描边圆角矩形：t ∈ [0,1] 沿纵向渐变（顶部受光），线宽 width。
void strokeRoundedV(std::uint32_t* buf, int bw, int bh, double x, double y,
                    double w, double h, double radius, double width,
                    double r0, double g0, double b0, double a0, double a1) {
    const int x0 = std::max(0, static_cast<int>(std::floor(x - width - 1)));
    const int y0 = std::max(0, static_cast<int>(std::floor(y - width - 1)));
    const int x1 =
        std::min(bw, static_cast<int>(std::ceil(x + w + width + 1)));
    const int y1 =
        std::min(bh, static_cast<int>(std::ceil(y + h + width + 1)));
    for (int py = y0; py < y1; ++py) {
        const double t = h > 0 ? (py - y) / h : 0.0;
        const double alpha = a0 + (a1 - a0) * t;
        for (int px = x0; px < x1; ++px) {
            const double d = std::fabs(
                roundedRectSdf(px + 0.5, py + 0.5, x, y, w, h, radius));
            const double coverage = std::clamp(width / 2 + 0.5 - d, 0.0, 1.0);
            if (coverage > 0.0) {
                blendPixel(buf, bw, bh, px, py, r0, g0, b0,
                           alpha * coverage);
            }
        }
    }
}

/// 由路径哈希推出固定倾斜角（度，[-2.5, 2.5]），与 Qt 衣架同一公式。
double tiltForPath(const std::string& path) {
    std::uint32_t hash = 0;
    for (const char c : path) {
        hash = hash * 31 + static_cast<std::uint8_t>(c);
    }
    return static_cast<double>(hash % 501) / 100.0 - 2.5;
}

/// 晾衣绳抛物线 y（tendedero：ropeTop + 4·sag·f·(1-f)）。
double ropeYAt(double x, int width) {
    const double sag = std::min(30.0, width * 0.018);
    const double f = x / width;
    return kRopeTop + 4.0 * sag * f * (1.0 - f);
}

/// 画晾衣绳：三层描边（投影 + 中性灰芯 + 顶部高光），两端淡出。
void drawRope(std::uint32_t* pixels, int width, int height) {
    for (int x = -20; x < width + 20; ++x) {
        const double y = ropeYAt(x, width);
        // 端头淡出：0.08..0.92 内完整，之外线性到透明。
        const double fadeIn = std::clamp(x / (0.08 * width), 0.0, 1.0);
        const double fadeOut =
            std::clamp((width - x) / (0.08 * width), 0.0, 1.0);
        const double fade = fadeIn * fadeOut;
        blendPixel(pixels, width, height, x,
                   static_cast<int>(std::lround(y + 1.2)), 0, 0, 0,
                   0.22 * fade);
        blendPixel(pixels, width, height, x,
                   static_cast<int>(std::lround(y)), 140, 140, 140, fade);
        blendPixel(pixels, width, height, x,
                   static_cast<int>(std::lround(y - 0.35)), 255, 255, 255,
                   0.45 * fade);
    }
}

/// 把一张卡片（影子 + 玻璃边框 + 照片 + 衣夹）绘制到局部缓冲，
/// 再按倾斜角旋转贴到面板上。锚点 = 衣夹顶中（局部坐标 (w/2, 0)）。
void drawCard(std::uint32_t* pixels, int width, int height, double centerX,
              const core::ImageBuffer& image, double tilt) {
    // 照片等比适配进 136×104，玻璃边框贴着照片。
    const double scale =
        std::min(static_cast<double>(kPhotoMaxWidth) / image.width(),
                 static_cast<double>(kPhotoMaxHeight) / image.height());
    const int photoW =
        std::max(1, static_cast<int>(std::lround(image.width() * scale)));
    const int photoH =
        std::max(1, static_cast<int>(std::lround(image.height() * scale)));
    const double cardW = photoW + kFrameInset * 2;
    const double cardH = photoH + kFrameInset * 2;

    // 局部缓冲：含上方衣夹（26pt）与四周阴影/旋转余量。
    const int bw = static_cast<int>(std::ceil(cardW)) + 64;
    const int bh = static_cast<int>(std::ceil(kCardBelowClipTop + cardH)) + 48;
    std::vector<std::uint32_t> local(static_cast<std::size_t>(bw) * bh, 0);
    const double cardX = bw / 2.0 - cardW / 2;
    const double cardY = kCardBelowClipTop;

    const double ga = ((gPalette->cardGlass >> 24) & 0xFF) / 255.0;
    const double gr = (gPalette->cardGlass >> 16) & 0xFF;
    const double gg = (gPalette->cardGlass >> 8) & 0xFF;
    const double gb = gPalette->cardGlass & 0xFF;

    // 投影（三层渐淡偏移，近似柔和阴影：黑 18%、下移 5）
    fillRounded(local.data(), bw, bh, cardX - 4, cardY + 7, cardW + 8,
                cardH + 8, kFrameRadius + 4, 0, 0, 0, 0.06);
    fillRounded(local.data(), bw, bh, cardX - 2, cardY + 6, cardW + 4,
                cardH + 4, kFrameRadius + 2, 0, 0, 0, 0.09);
    fillRounded(local.data(), bw, bh, cardX, cardY + 5, cardW, cardH,
                kFrameRadius, 0, 0, 0, 0.18);
    // 玻璃底
    fillRounded(local.data(), bw, bh, cardX, cardY, cardW, cardH,
                kFrameRadius, gr, gg, gb, ga);
    // 玻璃描边：顶部受光纵向渐变（白 55% → 12%）
    strokeRoundedV(local.data(), bw, bh, cardX, cardY, cardW, cardH,
                   kFrameRadius, 0.75, 255, 255, 255, 0.55, 0.12);
    // 外侧细暗边（黑 10%）
    strokeRoundedV(local.data(), bw, bh, cardX - 0.5, cardY - 0.5, cardW + 1,
                   cardH + 1, kFrameRadius + 0.5, 0.5, 0, 0, 0, 0.10, 0.10);

    // 照片（同心圆角 12）
    const core::ImageBuffer thumb = core::scaleNearest(image, photoW, photoH);
    const double photoX = cardX + kFrameInset;
    const double photoY = cardY + kFrameInset;
    for (int py = 0; py < photoH; ++py) {
        for (int px = 0; px < photoW; ++px) {
            const double d = roundedRectSdf(photoX + px + 0.5,
                                            photoY + py + 0.5, photoX, photoY,
                                            photoW, photoH, kPhotoRadius);
            const double coverage = std::clamp(0.5 - d, 0.0, 1.0);
            if (coverage <= 0.0) {
                continue;
            }
            const core::Rgba src = thumb.pixel(px, py);
            blendPixel(local.data(), bw, bh,
                       static_cast<int>(photoX) + px,
                       static_cast<int>(photoY) + py, src.r, src.g, src.b,
                       (src.a / 255.0) * coverage);
        }
    }
    // 照片内缘描边（白 18%）
    strokeRoundedV(local.data(), bw, bh, photoX, photoY, photoW, photoH,
                   kPhotoRadius, 0.5, 255, 255, 255, 0.18, 0.18);

    // 衣夹（z 序在卡片之上）：投影 + 拉丝金属 + 受光描边 + 绳槽
    const double clipX = bw / 2.0 - kClipWidth / 2;
    fillRounded(local.data(), bw, bh, clipX, 1.5, kClipWidth, kClipHeight,
                3.5, 0, 0, 0, 0.30);
    for (int py = 0; py < static_cast<int>(kClipHeight) + 1; ++py) {
        for (int px = 0; px < static_cast<int>(kClipWidth) + 1; ++px) {
            const double d = roundedRectSdf(clipX + px + 0.5, py + 0.5, clipX,
                                            0, kClipWidth, kClipHeight, 3.5);
            const double coverage = std::clamp(0.5 - d, 0.0, 1.0);
            if (coverage <= 0.0) {
                continue;
            }
            // 拉丝金属：水平渐变（亮斑偏左），对齐 tendedero 的 stops
            const double t = (px + 0.5) / kClipWidth;
            double v;
            if (t < 0.35) {
                v = 179 + (237 - 179) * (t / 0.35);
            } else if (t < 0.65) {
                v = 237 + (209 - 237) * ((t - 0.35) / 0.30);
            } else {
                v = 209 + (158 - 209) * ((t - 0.65) / 0.35);
            }
            blendPixel(local.data(), bw, bh, static_cast<int>(clipX) + px, py,
                       v, v, v, coverage);
        }
    }
    strokeRoundedV(local.data(), bw, bh, clipX, 0, kClipWidth, kClipHeight,
                   3.5, 0.6, 255, 255, 255, 0.90, 0.0);
    // 描边下段补暗（tendedero 是白→黑渐变；上面只画了白，补一道底部暗边）
    fillRounded(local.data(), bw, bh, bw / 2.0 - 2.5, 8.5, 5, 1.4, 0.7, 0, 0,
                0, 0.32);

    // 旋转贴到面板：锚点 = 衣夹顶中（绳子上方 kPinAbove 处）。
    const double anchorX = centerX;
    const double anchorY = ropeYAt(centerX, width) - kPinAbove;
    const double rad = tilt * 3.14159265358979323846 / 180.0;
    const double cosT = std::cos(rad);
    const double sinT = std::sin(rad);
    // 目标包围盒：旋转局部缓冲四角（局部坐标以锚点为原点）。
    double minX = 0, maxX = 0, minY = 0, maxY = 0;
    for (const auto& corner :
         {std::pair{-bw / 2.0, 0.0}, {bw / 2.0, 0.0},
          {-bw / 2.0, static_cast<double>(bh)},
          {bw / 2.0, static_cast<double>(bh)}}) {
        const double rx = corner.first * cosT - corner.second * sinT;
        const double ry = corner.first * sinT + corner.second * cosT;
        minX = std::min(minX, rx);
        maxX = std::max(maxX, rx);
        minY = std::min(minY, ry);
        maxY = std::max(maxY, ry);
    }
    for (int dy = static_cast<int>(std::floor(minY));
         dy <= static_cast<int>(std::ceil(maxY)); ++dy) {
        for (int dx = static_cast<int>(std::floor(minX));
             dx <= static_cast<int>(std::ceil(maxX)); ++dx) {
            // 逆映射回局部缓冲（y 向下、屏幕顺时针为正角）。
            const double lx = dx * cosT + dy * sinT + bw / 2.0;
            const double ly = -dx * sinT + dy * cosT;
            const int sx = static_cast<int>(std::lround(lx));
            const int sy = static_cast<int>(std::lround(ly));
            if (sx < 0 || sy < 0 || sx >= bw || sy >= bh) {
                continue;
            }
            const std::uint32_t src =
                local[static_cast<std::size_t>(sy) * bw + sx];
            const double sa = ((src >> 24) & 0xFF) / 255.0;
            if (sa <= 0.0) {
                continue;
            }
            blendPixel(pixels, width, height,
                       static_cast<int>(std::lround(anchorX)) + dx,
                       static_cast<int>(std::lround(anchorY)) + dy,
                       (src >> 16) & 0xFF, (src >> 8) & 0xFF, src & 0xFF, sa);
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

/// 渲染一帧：透明面板 + 抛物线晾衣绳 + 衣夹卡片（tendedero 样式）。
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

    // 1) 透明底板（tendedero 形态：只有绳子、衣夹和卡片可见，
    //    桌面从其余区域透出来）。
    std::memset(data, 0, static_cast<std::size_t>(width) * height * 4);

    // 2) 抛物线晾衣绳（两端淡出）
    drawRope(pixels, width, height);

    // 3) 卡片与缩略图（居中排布；解码失败的条目直接不占位）
    std::vector<std::pair<std::string, core::ImageBuffer>> cards;
    for (const std::string& path : items) {
        core::ImageBuffer image = loadThumbnail(path);
        if (!image.isEmpty()) {
            cards.emplace_back(path, std::move(image));
        }
    }
    if (cards.empty()) {
        // 空态：绳子中点下方的胶囊标记（无字体渲染能力，用形状示意）
        const double cx = width / 2.0;
        const double cy = ropeYAt(cx, width) + 34;
        fillRounded(pixels, width, height, cx - 60, cy - 12, 120, 24, 12,
                    (gPalette->emptyMark >> 16) & 0xFF,
                    (gPalette->emptyMark >> 8) & 0xFF,
                    gPalette->emptyMark & 0xFF,
                    ((gPalette->emptyMark >> 24) & 0xFF) / 255.0);
    } else {
        // 居中排布；张数过多时间距压缩，两端不出屏（同 Qt 衣架）。
        const int count = static_cast<int>(cards.size());
        const int gaps = std::max(count - 1, 0);
        const double step =
            gaps > 0
                ? std::min(static_cast<double>(kCardSpacing),
                           std::max(0, width - 200) / static_cast<double>(gaps))
                : kCardSpacing;
        for (int i = 0; i < count; ++i) {
            const double cx =
                width / 2.0 - gaps * step / 2.0 + i * step;
            drawCard(pixels, width, height, cx, cards[static_cast<std::size_t>(i)].second,
                     tiltForPath(cards[static_cast<std::size_t>(i)].first));
        }
    }

    wl_surface_attach(state->surface, buffer, 0, 0);
    wl_surface_damage_buffer(state->surface, 0, 0, width, height);
    wl_surface_commit(state->surface);
    wl_display_flush(state->display);
    std::fprintf(stdout, "[jie2-line] 渲染 %dx%d，%zu 张\n", width, height,
                 cards.size());
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
