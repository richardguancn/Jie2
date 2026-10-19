#pragma once
#include <string>

namespace jie2::core {
struct NamedKey { const char* name; int key; };
inline constexpr NamedKey namedKeys[] = {
    {"space", 32},
    {"plus", 43},
    {"tab", 16777217},
    {"enter", 16777220},
    {"escape", 16777216},
    {"backspace", 16777219},
    {"delete", 16777223},
    {"insert", 16777222},
    {"home", 16777232},
    {"end", 16777233},
    {"left", 16777234},
    {"right", 16777236},
    {"up", 16777235},
    {"down", 16777237},
    {"pageup", 16777238},
    {"pagedown", 16777239},
    {"minus", 45},
    {"equal", 61},
    {"bracketleft", 91},
    {"bracketright", 93},
    {"backslash", 92},
    {"semicolon", 59},
    {"apostrophe", 39},
    {"comma", 44},
    {"period", 46},
    {"slash", 47},
    {"grave", 96},
    {"f1", 16777264},
    {"f2", 16777265},
    {"f3", 16777266},
    {"f4", 16777267},
    {"f5", 16777268},
    {"f6", 16777269},
    {"f7", 16777270},
    {"f8", 16777271},
    {"f9", 16777272},
    {"f10", 16777273},
    {"f11", 16777274},
    {"f12", 16777275},
    {"f13", 16777276},
    {"f14", 16777277},
    {"f15", 16777278},
    {"f16", 16777279},
    {"f17", 16777280},
    {"f18", 16777281},
    {"f19", 16777282},
    {"f20", 16777283},
    {"f21", 16777284},
    {"f22", 16777285},
    {"f23", 16777286},
    {"f24", 16777287},
    {"f25", 16777288},
    {"f26", 16777289},
    {"f27", 16777290},
    {"f28", 16777291},
    {"f29", 16777292},
    {"f30", 16777293},
    {"f31", 16777294},
    {"f32", 16777295},
    {"f33", 16777296},
    {"f34", 16777297},
    {"f35", 16777298},
};
inline int keyFromText(const std::string& text) {
    if (text.size() == 1 && text[0] >= 33 && text[0] <= 126) {
        const char c = text[0]; return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
    }
    for (const auto& entry : namedKeys) if (text == entry.name) return entry.key;
    return 0;
}
inline std::string keyText(int key) {
    for (const auto& entry : namedKeys) if (key == entry.key) return entry.name;
    if (key >= 33 && key <= 126) return std::string(1, char(key >= 'A' && key <= 'Z' ? key - 'A' + 'a' : key));
    return {};
}
}
