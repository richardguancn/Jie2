#include "core/capability.h"

#include <sstream>

namespace jie2::core {

const char* toString(SessionType type) {
    switch (type) {
        case SessionType::MacOS:   return "macOS";
        case SessionType::X11:     return "X11";
        case SessionType::Wayland: return "Wayland";
        case SessionType::Windows: return "Windows";
        case SessionType::Unknown: break;
    }
    return "unknown";
}

const char* toString(CapabilityStatus status) {
    switch (status) {
        case CapabilityStatus::Available:     return "available";
        case CapabilityStatus::Degraded:      return "degraded";
        case CapabilityStatus::Unavailable:   return "unavailable";
        case CapabilityStatus::NotApplicable: return "n/a";
        case CapabilityStatus::Unknown:       break;
    }
    return "unknown";
}

namespace {

bool needsAttention(const Capability& capability) {
    return capability.status == CapabilityStatus::Degraded ||
           capability.status == CapabilityStatus::Unavailable;
}

void appendLine(std::ostringstream& out, const char* label, const Capability& capability) {
    out << label << ": " << toString(capability.status);
    if (!capability.detail.empty()) {
        out << " — " << capability.detail;
    }
    out << '\n';
}

} // namespace

bool PlatformCapabilities::hasLimitations() const {
    return needsAttention(screenCapture) || needsAttention(globalHotkey) ||
           needsAttention(alwaysOnTop) || needsAttention(layerShell) ||
           needsAttention(clipboardImage);
}

std::string PlatformCapabilities::describe() const {
    std::ostringstream out;
    out << "session: " << toString(session);
    if (!sessionDetail.empty()) {
        out << " (" << sessionDetail << ")";
    }
    out << '\n';
    appendLine(out, "screen-capture", screenCapture);
    appendLine(out, "global-hotkey ", globalHotkey);
    appendLine(out, "always-on-top ", alwaysOnTop);
    appendLine(out, "layer-shell   ", layerShell);
    appendLine(out, "clipboard-img ", clipboardImage);
    return out.str();
}

} // namespace jie2::core
