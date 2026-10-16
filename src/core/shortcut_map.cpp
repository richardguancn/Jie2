#include "core/shortcut_map.h"

#include <cctype>
#include <cstdlib>

namespace jie2::core {

namespace {

std::string trimCopy(const std::string& input) {
    std::size_t begin = 0;
    std::size_t end = input.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(input[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
        --end;
    }
    return input.substr(begin, end - begin);
}

std::vector<std::string> splitPlus(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    for (const char ch : text) {
        if (ch == '+') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    parts.push_back(current);
    return parts;
}

bool isSingleAlnum(const std::string& token) {
    return token.size() == 1 &&
           std::isalnum(static_cast<unsigned char>(token[0])) != 0;
}

} // namespace

bool parseShortcutSpec(const std::string& text, ShortcutSpec* out) {
    if (out == nullptr) {
        return false;
    }
    const std::string trimmed = trimCopy(text);
    ShortcutSpec spec;
    if (trimmed.empty() || trimmed == "none") {
        *out = spec;  // 禁用（key = 0）
        return true;
    }

    const std::vector<std::string> parts = splitPlus(trimmed);
    // 除了最后一段，其余都必须是修饰键。
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        const std::string token = trimCopy(parts[i]);
        if (token == "ctrl" || token == "control") {
            spec.modifiers |= kScCtrl;
        } else if (token == "alt" || token == "option") {
            spec.modifiers |= kScAlt;
        } else if (token == "shift") {
            spec.modifiers |= kScShift;
        } else if (token == "meta" || token == "cmd" || token == "super" ||
                   token == "win") {
            spec.modifiers |= kScMeta;
        } else {
            return false;  // 未知修饰键
        }
    }

    const std::string key = trimCopy(parts.back());
    if (!isSingleAlnum(key)) {
        return false;  // 键必须是单个字母或数字
    }
    spec.key = std::toupper(static_cast<unsigned char>(key[0]));
    *out = spec;
    return true;
}

const std::vector<std::string>& ShortcutMap::actionIds() {
    // 顺序即文档展示顺序。ID 同时是配置键后缀（shortcut.<id>）。
    static const std::vector<std::string> ids = {
        "tool.text",     "tool.number",  "tool.eraser",   "tool.magnifier",
        "style.dashed",  "style.corner", "shape.ellipse", "arrow.both",
        "arrow.reverse",
    };
    return ids;
}

ShortcutMap ShortcutMap::defaults() {
    ShortcutMap map;
    const auto bind = [&map](const char* action, const char* text) {
        ShortcutSpec spec;
        if (parseShortcutSpec(text, &spec)) {
            map.set(action, spec);
        }
    };
    bind("tool.text", "t");
    bind("tool.number", "n");
    bind("tool.eraser", "e");
    bind("tool.magnifier", "m");
    bind("style.dashed", "d");
    bind("style.corner", "r");
    bind("shape.ellipse", "o");
    bind("arrow.both", "b");
    bind("arrow.reverse", "shift+b");
    return map;
}

ShortcutMap ShortcutMap::fromConfig(const Config& config, std::string* warning) {
    ShortcutMap map = defaults();
    if (warning != nullptr) {
        warning->clear();
    }
    for (const std::string& action : actionIds()) {
        const std::string value = trimCopy(config.get("shortcut." + action));
        if (value.empty()) {
            continue;  // 未配置：保留默认
        }
        ShortcutSpec spec;
        if (!parseShortcutSpec(value, &spec)) {
            if (warning != nullptr) {
                if (!warning->empty()) {
                    *warning += "；";
                }
                *warning += "快捷键「shortcut." + action + "=" + value +
                            "」无法解析，已保留默认";
            }
            continue;
        }
        map.set(action, spec);
    }
    return map;
}

void ShortcutMap::set(const std::string& action, const ShortcutSpec& spec) {
    specs_[action] = spec;
}

ShortcutSpec ShortcutMap::get(const std::string& action) const {
    const auto it = specs_.find(action);
    return it == specs_.end() ? ShortcutSpec{} : it->second;
}

bool ShortcutMap::matches(const std::string& action, int key,
                          unsigned modifiers) const {
    const ShortcutSpec spec = get(action);
    if (!spec.enabled()) {
        return false;
    }
    // 只比较四个修饰键位（忽略 NumLock 等扩展位）。
    constexpr unsigned kMask = kScCtrl | kScAlt | kScShift | kScMeta;
    return key == spec.key && (modifiers & kMask) == spec.modifiers;
}

} // namespace jie2::core
