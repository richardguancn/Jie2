#include "platform/platform.h"

#include <memory>

namespace jie2::platform {

#if defined(__APPLE__)
std::unique_ptr<Platform> createMacPlatform();
#elif defined(__linux__)
std::unique_ptr<Platform> createLinuxPlatform();
#elif defined(_WIN32)
std::unique_ptr<Platform> createWindowsPlatform();
#endif

namespace {

/// 未支持平台的兜底实现：如实报告"一切未知"，而不是假装可用。
class UnsupportedPlatform final : public Platform {
public:
    std::string name() const override { return "unsupported"; }

    core::PlatformCapabilities probe() const override {
        core::PlatformCapabilities caps;
        caps.session = core::SessionType::Unknown;
        caps.sessionDetail = "当前平台尚未实现「J2」的平台集成层";
        caps.screenCapture = {core::CapabilityStatus::Unknown, ""};
        caps.globalHotkey = {core::CapabilityStatus::Unknown, ""};
        caps.alwaysOnTop = {core::CapabilityStatus::Unknown, ""};
        caps.layerShell = {core::CapabilityStatus::Unknown, ""};
        caps.clipboardImage = {core::CapabilityStatus::Unknown, ""};
        return caps;
    }

    bool grabHotkey(const HotkeySpec&, HotkeyCallback, void*,
                    std::string* error) override {
        if (error != nullptr) {
            *error = "当前平台尚未实现全局热键";
        }
        return false;
    }

    void releaseAllHotkeys() override {}

    std::vector<DisplayInfo> displays() const override { return {}; }

    CaptureResult captureRegion(const core::Rect&) const override {
        return CaptureResult::failure("当前平台尚未实现抓屏");
    }

    CaptureResult captureDisplay(const std::string&) const override {
        return CaptureResult::failure("当前平台尚未实现抓屏");
    }

    CaptureResult captureAll() const override {
        return CaptureResult::failure("当前平台尚未实现抓屏");
    }

    std::vector<core::Rect> windowRects() const override { return {}; }

    core::OcrResult recognizeText(const core::ImageBuffer&,
                                  const std::string&) const override {
        return core::OcrResult::failure("当前平台尚未实现系统文字识别");
    }
};

} // namespace

std::unique_ptr<Platform> createPlatform() {
#if defined(__APPLE__)
    return createMacPlatform();
#elif defined(__linux__)
    return createLinuxPlatform();
#elif defined(_WIN32)
    return createWindowsPlatform();
#else
    return std::make_unique<UnsupportedPlatform>();
#endif
}

} // namespace jie2::platform
