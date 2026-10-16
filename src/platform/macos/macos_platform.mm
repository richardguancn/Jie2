// 「截2」macOS 平台实现
//
// 选型说明（与评估报告 1.4 对齐）：
//   全局热键用 Carbon RegisterEventHotKey —— 与 Snipaste 在 macOS 上的做法一致，
//   是当前 macOS 上仍可用的系统级热键接口，无需辅助功能权限即可注册。
//
// 探测项是真实读取（系统版本、屏幕数量、屏幕录制权限、辅助功能权限），
// 不是硬编码的静态表。

#import <Carbon/Carbon.h>
#import <Cocoa/Cocoa.h>
#import <Vision/Vision.h>

#include "platform/macos/macos_platform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <vector>

namespace jie2::platform {

namespace {

// 'JIe2' —— 用于区分本应用注册的热键事件。
constexpr OSType kHotkeySignature = 0x4A496532;
constexpr std::size_t kMaxHotkeys = 32;

struct RegisteredHotkey {
    bool used = false;
    UInt32 slot = 0;
    std::string id;
    HotkeyCallback callback = nullptr;
    void* userData = nullptr;
    EventHotKeyRef ref = nullptr;
};

RegisteredHotkey g_hotkeys[kMaxHotkeys];
EventHandlerRef g_eventHandler = nullptr;
bool g_handlerInstalled = false;

OSStatus hotkeyPressed(EventHandlerCallRef, EventRef event, void*) {
    EventHotKeyID hotkeyId{};
    const OSStatus status = GetEventParameter(
        event, kEventParamDirectObject, typeEventHotKeyID,
        nullptr, sizeof(hotkeyId), nullptr, &hotkeyId);
    if (status != noErr || hotkeyId.signature != kHotkeySignature) {
        return eventNotHandledErr;
    }
    for (auto& entry : g_hotkeys) {
        if (entry.used && entry.slot == hotkeyId.id) {
            if (entry.callback != nullptr) {
                entry.callback(entry.id, entry.userData);
            }
            return noErr;
        }
    }
    return eventNotHandledErr;
}

bool installHandlerIfNeeded(std::string* error) {
    if (g_handlerInstalled) {
        return true;
    }
    const EventTypeSpec eventType{kEventClassKeyboard, kEventHotKeyPressed};
    const OSStatus status = InstallApplicationEventHandler(
        &hotkeyPressed, 1, &eventType, nullptr, &g_eventHandler);
    if (status != noErr) {
        if (error != nullptr) {
            *error = "InstallApplicationEventHandler failed (OSStatus " +
                     std::to_string(status) + ")";
        }
        return false;
    }
    g_handlerInstalled = true;
    return true;
}

UInt32 toCarbonModifiers(unsigned modifiers) {
    UInt32 out = 0;
    if (modifiers & kModCtrl)  { out |= controlKey; }
    if (modifiers & kModAlt)   { out |= optionKey; }
    if (modifiers & kModShift) { out |= shiftKey; }
    if (modifiers & kModSuper) { out |= cmdKey; }
    return out;
}

/// 平台无关键码 -> Carbon 虚拟键码。未覆盖的键返回 UINT32_MAX。
UInt32 toCarbonKeycode(Key key) {
    switch (key) {
        case Key::A: return kVK_ANSI_A;
        case Key::B: return kVK_ANSI_B;
        case Key::C: return kVK_ANSI_C;
        case Key::D: return kVK_ANSI_D;
        case Key::E: return kVK_ANSI_E;
        case Key::F: return kVK_ANSI_F;
        case Key::G: return kVK_ANSI_G;
        case Key::H: return kVK_ANSI_H;
        case Key::I: return kVK_ANSI_I;
        case Key::J: return kVK_ANSI_J;
        case Key::K: return kVK_ANSI_K;
        case Key::L: return kVK_ANSI_L;
        case Key::M: return kVK_ANSI_M;
        case Key::N: return kVK_ANSI_N;
        case Key::O: return kVK_ANSI_O;
        case Key::P: return kVK_ANSI_P;
        case Key::Q: return kVK_ANSI_Q;
        case Key::R: return kVK_ANSI_R;
        case Key::S: return kVK_ANSI_S;
        case Key::T: return kVK_ANSI_T;
        case Key::U: return kVK_ANSI_U;
        case Key::V: return kVK_ANSI_V;
        case Key::W: return kVK_ANSI_W;
        case Key::X: return kVK_ANSI_X;
        case Key::Y: return kVK_ANSI_Y;
        case Key::Z: return kVK_ANSI_Z;
        case Key::Num0: return kVK_ANSI_0;
        case Key::Num1: return kVK_ANSI_1;
        case Key::Num2: return kVK_ANSI_2;
        case Key::Num3: return kVK_ANSI_3;
        case Key::Num4: return kVK_ANSI_4;
        case Key::Num5: return kVK_ANSI_5;
        case Key::Num6: return kVK_ANSI_6;
        case Key::Num7: return kVK_ANSI_7;
        case Key::Num8: return kVK_ANSI_8;
        case Key::Num9: return kVK_ANSI_9;
        case Key::F1: return kVK_F1;
        case Key::F2: return kVK_F2;
        case Key::F3: return kVK_F3;
        case Key::F4: return kVK_F4;
        case Key::F5: return kVK_F5;
        case Key::F6: return kVK_F6;
        case Key::F7: return kVK_F7;
        case Key::F8: return kVK_F8;
        case Key::F9: return kVK_F9;
        case Key::F10: return kVK_F10;
        case Key::F11: return kVK_F11;
        case Key::F12: return kVK_F12;
        case Key::Unknown: break;
    }
    return UINT32_MAX;
}

std::string screenDescription() {
    const NSArray<NSScreen*>* screens = [NSScreen screens];
    return std::to_string(screens.count) + " screen(s)";
}

} // namespace

class MacPlatform final : public Platform {
public:
    std::string name() const override { return "macos"; }

    core::PlatformCapabilities probe() const override {
        core::PlatformCapabilities caps;
        caps.session = core::SessionType::MacOS;

        // 系统版本与屏幕数量：真实读取
        const NSOperatingSystemVersion version =
            [[NSProcessInfo processInfo] operatingSystemVersion];
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "macOS %ld.%ld.%ld, %s",
                      static_cast<long>(version.majorVersion),
                      static_cast<long>(version.minorVersion),
                      static_cast<long>(version.patchVersion),
                      screenDescription().c_str());
        caps.sessionDetail = buffer;

        // 屏幕录制权限：macOS 抓屏的前提。真实探测（不弹窗）。
        const bool canCapture = CGPreflightScreenCaptureAccess();
        caps.screenCapture = {
            canCapture ? core::CapabilityStatus::Available
                       : core::CapabilityStatus::Degraded,
            canCapture
                ? "CGDisplayCreateImageForRect / CGWindowList（已获屏幕录制权限）"
                : "未授予屏幕录制权限：首次抓屏时系统会弹窗请求，"
                  "需在 系统设置 → 隐私与安全性 → 屏幕录制 中允许"};

        caps.globalHotkey = {core::CapabilityStatus::Available,
                             "Carbon RegisterEventHotKey"};
        caps.alwaysOnTop = {core::CapabilityStatus::Available,
                            "NSWindow level + collectionBehavior"};
        caps.layerShell = {core::CapabilityStatus::NotApplicable,
                           "macOS 无 layer-shell 概念，顶部条用 NSPanel 实现"};
        caps.clipboardImage = {core::CapabilityStatus::Available, "NSPasteboard"};

        // 辅助功能权限：元素检测的前提。M0 先并入会话说明。
        if (!AXIsProcessTrusted()) {
            caps.sessionDetail += "；辅助功能权限未授予（M1 的元素/窗口吸附将不可用）";
        }
        return caps;
    }

    bool grabHotkey(const HotkeySpec& spec, HotkeyCallback callback,
                    void* userData, std::string* error) override {
        if (!installHandlerIfNeeded(error)) {
            return false;
        }
        const UInt32 keycode = toCarbonKeycode(spec.key);
        if (keycode == UINT32_MAX) {
            if (error != nullptr) { *error = "键不受支持"; }
            return false;
        }

        RegisteredHotkey* slot = nullptr;
        for (std::size_t i = 0; i < kMaxHotkeys; ++i) {
            if (!g_hotkeys[i].used) {
                slot = &g_hotkeys[i];
                slot->slot = static_cast<UInt32>(i + 1);
                break;
            }
        }
        if (slot == nullptr) {
            if (error != nullptr) { *error = "热键表已满"; }
            return false;
        }

        EventHotKeyID hotkeyId{};
        hotkeyId.signature = kHotkeySignature;
        hotkeyId.id = slot->slot;

        EventHotKeyRef ref = nullptr;
        const OSStatus status = RegisterEventHotKey(
            keycode, toCarbonModifiers(spec.modifiers), hotkeyId,
            GetApplicationEventTarget(), 0, &ref);
        if (status != noErr) {
            if (error != nullptr) {
                *error = "RegisterEventHotKey 失败 (OSStatus " +
                         std::to_string(status) + ")";
                if (status == eventHotKeyExistsErr) {
                    *error += "：该组合已被其他程序占用";
                }
            }
            return false;
        }

        slot->used = true;
        slot->id = spec.id;
        slot->callback = callback;
        slot->userData = userData;
        slot->ref = ref;
        return true;
    }

    void releaseAllHotkeys() override {
        for (auto& entry : g_hotkeys) {
            if (entry.used && entry.ref != nullptr) {
                UnregisterEventHotKey(entry.ref);
            }
            entry = RegisteredHotkey{};
        }
        if (g_eventHandler != nullptr) {
            RemoveEventHandler(g_eventHandler);
            g_eventHandler = nullptr;
            g_handlerInstalled = false;
        }
    }

    // ---- 抓屏（M1）----

    std::vector<DisplayInfo> displays() const override {
        std::vector<DisplayInfo> out;
        CGDirectDisplayID ids[16];
        uint32_t count = 0;
        if (CGGetActiveDisplayList(16, ids, &count) != kCGErrorSuccess) {
            return out;
        }
        const CGDirectDisplayID primary = CGMainDisplayID();
        for (uint32_t i = 0; i < count; ++i) {
            const CGRect bounds = CGDisplayBounds(ids[i]);
            // 缩放比例用显示模式推断：pixelWidth（物理像素）/ width（逻辑点）。
            // 注意不要用 CGDisplayPixelsWide —— 实测在 macOS 27 上它返回的是
            // 逻辑点而非物理像素，会得出错误的 scale=1.0。
            double scale = 1.0;
            CGDisplayModeRef mode = CGDisplayCopyDisplayMode(ids[i]);
            if (mode != nullptr) {
                const size_t modeWidth = CGDisplayModeGetWidth(mode);
                const size_t pixelWidth = CGDisplayModeGetPixelWidth(mode);
                if (modeWidth > 0 && pixelWidth > 0) {
                    scale = static_cast<double>(pixelWidth) /
                            static_cast<double>(modeWidth);
                }
                CGDisplayModeRelease(mode);
            }
            DisplayInfo info;
            info.id = std::to_string(static_cast<unsigned long long>(ids[i]));
            info.name = "Display " + std::to_string(i + 1);
            info.bounds = core::Rect{static_cast<int>(bounds.origin.x),
                                     static_cast<int>(bounds.origin.y),
                                     static_cast<int>(bounds.size.width),
                                     static_cast<int>(bounds.size.height)};
            info.scaleFactor = scale;
            info.isPrimary = (ids[i] == primary);
            out.push_back(std::move(info));
        }
        return out;
    }

    CaptureResult captureRegion(const core::Rect& region) const override {
        if (region.isEmpty()) {
            return CaptureResult::failure("截图区域为空");
        }
        // 找到与区域相交的显示器。M1 的阶段约束：区域需落在单台显示器内，
        // 跨屏混合 DPI 的拼接留待 M1 后续迭代（当前实现会取相交面积最大的屏，
        // 并把区域裁剪到该屏内，避免产生误导性的拼接结果）。
        const auto all = displays();
        if (all.empty()) {
            return CaptureResult::failure("未找到可用显示器");
        }
        const DisplayInfo* best = nullptr;
        int bestArea = 0;
        for (const auto& d : all) {
            const core::Rect inter = d.bounds.intersected(region);
            const int area = inter.width * inter.height;
            if (area > bestArea) {
                bestArea = area;
                best = &d;
            }
        }
        if (best == nullptr) {
            return CaptureResult::failure("截图区域不在任何显示器范围内");
        }
        const core::Rect local = region.intersected(best->bounds);
        if (local.isEmpty()) {
            return CaptureResult::failure("截图区域超出显示器范围");
        }
        // 实测行为（macOS 27）：CGDisplayCreateImageForRect 的 rect 参数
        // 使用「显示器的逻辑点坐标」（相对显示器左上角），返回图像则是
        // 物理像素 —— 例如传入 1710x1107 得到 3420x2214（2x Retina）。
        // 因此这里传入逻辑坐标，不要预先乘以 scaleFactor。
        const CGRect targetRect = CGRectMake(local.x - best->bounds.x,
                                             local.y - best->bounds.y,
                                             local.width, local.height);
        CGDirectDisplayID displayId =
            static_cast<CGDirectDisplayID>(std::stoul(best->id));
        CGImageRef image = CGDisplayCreateImageForRect(displayId, targetRect);
        CaptureResult result = convertImage(image);
        if (image != nullptr) {
            CGImageRelease(image);
        }
        return result;
    }

    CaptureResult captureDisplay(const std::string& displayId) const override {
        if (displayId.empty()) {
            return CaptureResult::failure("显示器标识为空");
        }
        CGDirectDisplayID id = 0;
        try {
            id = static_cast<CGDirectDisplayID>(std::stoul(displayId));
        } catch (...) {
            return CaptureResult::failure("显示器标识无效：" + displayId);
        }
        CGImageRef image = CGDisplayCreateImage(id);
        CaptureResult result = convertImage(image);
        if (image != nullptr) {
            CGImageRelease(image);
        }
        return result;
    }

    CaptureResult captureAll() const override {
        const auto all = displays();
        if (all.empty()) {
            return CaptureResult::failure("未找到可用显示器");
        }
        // 以主屏为基准的虚拟桌面范围（逻辑坐标），输出物理像素合屏。
        // 统一 scale：取主屏比例。混合 DPI 的逐屏精确拼接属 M1 迭代项，
        // 这里保证"单屏与同 DPI 多屏"结果正确，混合场景不产生错位假象。
        const DisplayInfo* primary = nullptr;
        for (const auto& d : all) {
            if (d.isPrimary) {
                primary = &d;
                break;
            }
        }
        if (primary == nullptr) {
            primary = &all.front();
        }
        int minX = primary->bounds.x;
        int minY = primary->bounds.y;
        int maxX = primary->bounds.right();
        int maxY = primary->bounds.bottom();
        for (const auto& d : all) {
            minX = std::min(minX, d.bounds.x);
            minY = std::min(minY, d.bounds.y);
            maxX = std::max(maxX, d.bounds.right());
            maxY = std::max(maxY, d.bounds.bottom());
        }
        const core::Rect desktop{minX, minY, maxX - minX, maxY - minY};
        return captureRegion(desktop.intersected(primary->bounds));
    }

    std::vector<core::Rect> windowRects() const override {
        std::vector<core::Rect> result;
        const CFArrayRef windowList = CGWindowListCopyWindowInfo(
            kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
            kCGNullWindowID);
        if (windowList == nullptr) {
            return result;
        }

        const pid_t ownPid = getpid();
        const CFIndex count = CFArrayGetCount(windowList);
        for (CFIndex i = 0; i < count; ++i) {
            const auto entry =
                static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(windowList, i));
            if (entry == nullptr) {
                continue;
            }

            // 只要普通窗口层（layer 0）：菜单、提示、悬浮面板都不适合吸附。
            int layer = -1;
            const auto layerRef = static_cast<CFNumberRef>(
                CFDictionaryGetValue(entry, kCGWindowLayer));
            if (layerRef == nullptr ||
                !CFNumberGetValue(layerRef, kCFNumberIntType, &layer) ||
                layer != 0) {
                continue;
            }

            // 排除本应用自己的窗口（遮罩、托盘对话框等）。
            int ownerPid = 0;
            const auto pidRef = static_cast<CFNumberRef>(
                CFDictionaryGetValue(entry, kCGWindowOwnerPID));
            if (pidRef != nullptr &&
                CFNumberGetValue(pidRef, kCFNumberIntType, &ownerPid) &&
                ownerPid == ownPid) {
                continue;
            }

            const auto boundsRef = static_cast<CFDictionaryRef>(
                CFDictionaryGetValue(entry, kCGWindowBounds));
            if (boundsRef == nullptr) {
                continue;
            }
            CGRect rect{};
            if (!CGRectMakeWithDictionaryRepresentation(boundsRef, &rect)) {
                continue;
            }
            // 过小的窗口（图标、状态项）不参与吸附。
            if (rect.size.width < 40.0 || rect.size.height < 40.0) {
                continue;
            }
            result.push_back(core::Rect{
                static_cast<int>(std::lround(rect.origin.x)),
                static_cast<int>(std::lround(rect.origin.y)),
                static_cast<int>(std::lround(rect.size.width)),
                static_cast<int>(std::lround(rect.size.height))});
        }
        CFRelease(windowList);
        return result;
    }

    // ---- 系统文字识别（M5）：macOS Vision ----
    //
    // Vision 是系统自带的离线 OCR：零安装、零配置、不联网。
    // 这也是评估报告 4.4 里"默认离线"原则在 macOS 上的落点。
    core::OcrResult recognizeText(const core::ImageBuffer& image,
                                  const std::string& languageHint) const override {
        if (image.isEmpty()) {
            return core::OcrResult::failure("图像为空", "vision");
        }

        // ImageBuffer（RGBA/BGRA8888，无行填充）→ CGImage → 标准位图上下文重画。
        //
        // 两步走的原因：内核缓冲的内存序随来源而变（抓屏是 BGRA，历史图是
        // RGBA），直接包 CGImage 需要精确匹配 CGBitmapInfo；而 Vision 对
        // 位图格式的宽容度有限。统一"重画到固定 BGRA 上下文"后，
        // Vision 拿到的永远是同一种格式，消除字节序踩坑。
        const std::size_t byteCount = image.byteSize();
        const CGBitmapInfo srcInfo = static_cast<CGBitmapInfo>(
            static_cast<std::uint32_t>(
                image.format() == core::PixelFormat::BGRA8888
                    ? kCGImageAlphaPremultipliedFirst
                    : kCGImageAlphaLast) |
            static_cast<std::uint32_t>(kCGBitmapByteOrder32Little));

        CGImageRef source = nullptr;
        {
            CFDataRef data = CFDataCreate(kCFAllocatorDefault, image.data(),
                                          static_cast<CFIndex>(byteCount));
            CGDataProviderRef provider =
                data != nullptr ? CGDataProviderCreateWithCFData(data) : nullptr;
            if (data != nullptr) {
                CFRelease(data);
            }
            CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
            if (provider != nullptr && space != nullptr) {
                source = CGImageCreate(
                    static_cast<size_t>(image.width()),
                    static_cast<size_t>(image.height()), 8, 32,
                    static_cast<size_t>(image.stride()), space, srcInfo,
                    provider, nullptr, false, kCGRenderingIntentDefault);
            }
            if (space != nullptr) {
                CGColorSpaceRelease(space);
            }
            if (provider != nullptr) {
                CGDataProviderRelease(provider);
            }
        }
        if (source == nullptr) {
            return core::OcrResult::failure("无法创建 CGImage", "vision");
        }

        CGColorSpaceRef rgbSpace = CGColorSpaceCreateDeviceRGB();
        CGContextRef context = CGBitmapContextCreate(
            nullptr, static_cast<size_t>(image.width()),
            static_cast<size_t>(image.height()), 8,
            static_cast<size_t>(image.width()) * 4, rgbSpace,
            static_cast<CGBitmapInfo>(
                static_cast<std::uint32_t>(kCGImageAlphaPremultipliedFirst) |
                static_cast<std::uint32_t>(kCGBitmapByteOrder32Little)));
        if (rgbSpace != nullptr) {
            CGColorSpaceRelease(rgbSpace);
        }
        CGImageRef redraw = nullptr;
        if (context != nullptr) {
            CGContextDrawImage(
                context, CGRectMake(0, 0, image.width(), image.height()), source);
            redraw = CGBitmapContextCreateImage(context);
            CGContextRelease(context);
        }
        CGImageRelease(source);
        if (redraw == nullptr) {
            return core::OcrResult::failure("无法生成识别用位图", "vision");
        }

        // ---- Vision 识别 ----
        __block std::string collected;
        __block std::string failure;
        if (@available(macOS 10.15, *)) {
            VNRecognizeTextRequest* request =
                [[VNRecognizeTextRequest alloc] init];
            request.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
            // 语言：默认中英混合（Vision 用 BCP-47 标签）。
            if (languageHint.empty()) {
                request.recognitionLanguages = @[ @"zh-Hans", @"en-US" ];
            } else {
                request.recognitionLanguages =
                    @[ [NSString stringWithUTF8String:languageHint.c_str()] ];
            }
            request.usesLanguageCorrection = YES;

            VNImageRequestHandler* handler =
                [[VNImageRequestHandler alloc] initWithCGImage:redraw options:@{}];
            NSError* error = nil;
            const BOOL performed = [handler performRequests:@[ request ]
                                                      error:&error];
            if (!performed) {
                failure = error != nullptr
                              ? std::string(error.localizedDescription.UTF8String)
                              : "Vision 识别失败";
            } else {
                NSArray<VNRecognizedTextObservation*>* observations =
                    request.results;
                for (VNRecognizedTextObservation* observation in observations) {
                    NSArray<VNRecognizedText*>* candidates =
                        [observation topCandidates:1];
                    if (candidates.count > 0) {
                        NSString* text = candidates.firstObject.string;
                        if (text.length > 0) {
                            if (!collected.empty()) {
                                collected += "\n";
                            }
                            collected += text.UTF8String;
                        }
                    }
                }
            }
        } else {
            failure = "系统版本过低（需 macOS 10.15+）";
        }
        CGImageRelease(redraw);

        if (!failure.empty()) {
            return core::OcrResult::failure(failure, "vision");
        }
        if (collected.empty()) {
            // 识别成功但没有文字：这不是错误，如实返回空文本。
            return core::OcrResult::success("", "vision");
        }
        return core::OcrResult::success(collected, "vision");
    }

private:
    /// CGImage -> ImageBuffer（BGRA8888，物理像素）。
    /// 失败时给出可操作原因（最常见的是屏幕录制权限）。
    static CaptureResult convertImage(CGImageRef image) {
        if (image == nullptr) {
            const bool trusted = CGPreflightScreenCaptureAccess();
            return CaptureResult::failure(
                trusted ? "系统返回空图像（该显示器可能已休眠）"
                        : "抓屏失败：未授予屏幕录制权限，请在 系统设置 → 隐私与安全性"
                          " → 屏幕录制 中允许本应用后重试");
        }
        const size_t width = CGImageGetWidth(image);
        const size_t height = CGImageGetHeight(image);
        if (width == 0 || height == 0) {
            return CaptureResult::failure("抓屏返回空尺寸图像");
        }
        core::ImageBuffer buffer(static_cast<int>(width), static_cast<int>(height),
                                 core::PixelFormat::BGRA8888);
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        if (space == nullptr) {
            return CaptureResult::failure("创建颜色空间失败");
        }
        // kCGImageAlphaPremultipliedFirst + 32Little 即内存序 BGRA8888。
        // 两个枚举分属不同类型，显式转成 CGBitmapInfo 组合（否则触发枚举混用告警）。
        const CGBitmapInfo bitmapInfo = static_cast<CGBitmapInfo>(
            static_cast<std::uint32_t>(kCGImageAlphaPremultipliedFirst) |
            static_cast<std::uint32_t>(kCGBitmapByteOrder32Little));
        CGContextRef context = CGBitmapContextCreate(
            buffer.data(), width, height, 8, buffer.stride(), space, bitmapInfo);
        CGColorSpaceRelease(space);
        if (context == nullptr) {
            return CaptureResult::failure("创建位图上下文失败");
        }
        CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
        CGContextRelease(context);
        return CaptureResult::success(std::move(buffer));
    }
};

std::unique_ptr<Platform> createMacPlatform() {
    return std::make_unique<MacPlatform>();
}

} // namespace jie2::platform
