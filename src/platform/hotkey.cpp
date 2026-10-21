#include "platform/hotkey.h"

namespace jie2::platform {

namespace {

const char* keyName(Key key) {
    switch (key) {
        case Key::A: return "A"; case Key::B: return "B"; case Key::C: return "C";
        case Key::D: return "D"; case Key::E: return "E"; case Key::F: return "F";
        case Key::G: return "G"; case Key::H: return "H"; case Key::I: return "I";
        case Key::J: return "J"; case Key::K: return "K"; case Key::L: return "L";
        case Key::M: return "M"; case Key::N: return "N"; case Key::O: return "O";
        case Key::P: return "P"; case Key::Q: return "Q"; case Key::R: return "R";
        case Key::S: return "S"; case Key::T: return "T"; case Key::U: return "U";
        case Key::V: return "V"; case Key::W: return "W"; case Key::X: return "X";
        case Key::Y: return "Y"; case Key::Z: return "Z";
        case Key::Num0: return "0"; case Key::Num1: return "1";
        case Key::Num2: return "2"; case Key::Num3: return "3";
        case Key::Num4: return "4"; case Key::Num5: return "5";
        case Key::Num6: return "6"; case Key::Num7: return "7";
        case Key::Num8: return "8"; case Key::Num9: return "9";
        case Key::F1: return "F1"; case Key::F2: return "F2";
        case Key::F3: return "F3"; case Key::F4: return "F4";
        case Key::F5: return "F5"; case Key::F6: return "F6";
        case Key::F7: return "F7"; case Key::F8: return "F8";
        case Key::F9: return "F9"; case Key::F10: return "F10";
        case Key::F11: return "F11"; case Key::F12: return "F12";
        case Key::Space: return "space";
        case Key::Tab: return "tab";
        case Key::Return: return "enter";
        case Key::Escape: return "escape";
        case Key::Backspace: return "backspace";
        case Key::Delete: return "delete";
        case Key::Insert: return "insert";
        case Key::Home: return "home";
        case Key::End: return "end";
        case Key::Left: return "left";
        case Key::Right: return "right";
        case Key::Up: return "up";
        case Key::Down: return "down";
        case Key::PageUp: return "pageup";
        case Key::PageDown: return "pagedown";
        case Key::Minus: return "minus";
        case Key::Equal: return "equal";
        case Key::BracketLeft: return "bracketleft";
        case Key::BracketRight: return "bracketright";
        case Key::Backslash: return "backslash";
        case Key::Semicolon: return "semicolon";
        case Key::Apostrophe: return "apostrophe";
        case Key::Comma: return "comma";
        case Key::Period: return "period";
        case Key::Slash: return "slash";
        case Key::Grave: return "grave";
        case Key::F13: return "F13";
        case Key::F14: return "F14";
        case Key::F15: return "F15";
        case Key::F16: return "F16";
        case Key::F17: return "F17";
        case Key::F18: return "F18";
        case Key::F19: return "F19";
        case Key::F20: return "F20";
        case Key::Unknown: break;
    }
    return "?";
}

} // namespace

std::string describeHotkey(const HotkeySpec& spec) {
    std::string text;
    if (spec.modifiers & kModCtrl)  { text += "Ctrl+"; }
    if (spec.modifiers & kModAlt)   { text += "Alt+"; }
    if (spec.modifiers & kModShift) { text += "Shift+"; }
    if (spec.modifiers & kModSuper) { text += "Super+"; }
    text += keyName(spec.key);
    return text;
}

namespace {

std::string lowercase(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            out.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

Key keyFromName(const std::string& word) {
    if (word.size() == 1 && word[0] >= 'a' && word[0] <= 'z') {
        return static_cast<Key>(static_cast<unsigned>(Key::A) +
                                static_cast<unsigned>(word[0] - 'a'));
    }
    if (word.size() == 1 && word[0] >= '0' && word[0] <= '9') {
        return static_cast<Key>(static_cast<unsigned>(Key::Num0) +
                                static_cast<unsigned>(word[0] - '0'));
    }
    if (word.size() >= 2 && word[0] == 'f') {
        int number = 0;
        for (std::size_t i = 1; i < word.size(); ++i) {
            if (word[i] < '0' || word[i] > '9') {
                return Key::Unknown;
            }
            if (number > 20) return Key::Unknown;
            number = number * 10 + (word[i] - '0');
        }
        if (number >= 1 && number <= 20) {
            return static_cast<Key>(static_cast<unsigned>(Key::F1) +
                                    static_cast<unsigned>(number - 1));
        }
    }
    if (word == "space") return Key::Space;
    if (word == "tab") return Key::Tab;
    if (word == "enter") return Key::Return;
    if (word == "escape") return Key::Escape;
    if (word == "backspace") return Key::Backspace;
    if (word == "delete") return Key::Delete;
    if (word == "insert") return Key::Insert;
    if (word == "home") return Key::Home;
    if (word == "end") return Key::End;
    if (word == "left") return Key::Left;
    if (word == "right") return Key::Right;
    if (word == "up") return Key::Up;
    if (word == "down") return Key::Down;
    if (word == "pageup") return Key::PageUp;
    if (word == "pagedown") return Key::PageDown;
    if (word == "minus") return Key::Minus;
    if (word == "equal") return Key::Equal;
    if (word == "bracketleft") return Key::BracketLeft;
    if (word == "bracketright") return Key::BracketRight;
    if (word == "backslash") return Key::Backslash;
    if (word == "semicolon") return Key::Semicolon;
    if (word == "apostrophe") return Key::Apostrophe;
    if (word == "comma") return Key::Comma;
    if (word == "period") return Key::Period;
    if (word == "slash") return Key::Slash;
    if (word == "grave") return Key::Grave;
    if (word == "esc") return Key::Escape;
    if (word == "return") return Key::Return;
    return Key::Unknown;
}

} // namespace

bool parseHotkeyText(const std::string& text, HotkeySpec* spec) {
    HotkeySpec parsed;
    std::string word;
    auto flush = [&]() -> bool {
        if (word.empty()) {
            return true;  // 连续分隔符按一个处理
        }
        const std::string lower = lowercase(word);
        if (lower == "ctrl" || lower == "control") {
            if (parsed.modifiers & kModCtrl) { return false; }
            parsed.modifiers |= kModCtrl;
            return true;
        }
        if (lower == "alt" || lower == "option") {
            if (parsed.modifiers & kModAlt) { return false; }
            parsed.modifiers |= kModAlt;
            return true;
        }
        if (lower == "shift") {
            if (parsed.modifiers & kModShift) { return false; }
            parsed.modifiers |= kModShift;
            return true;
        }
        if (lower == "cmd" || lower == "command" || lower == "super" ||
            lower == "win" || lower == "meta") {
            if (parsed.modifiers & kModSuper) { return false; }
            parsed.modifiers |= kModSuper;
            return true;
        }
        // 不是修饰键 → 只能是主键，且只能出现一次。
        if (parsed.key != Key::Unknown) {
            return false;
        }
        parsed.key = keyFromName(lower);
        return parsed.key != Key::Unknown;
    };
    for (const char ch : text) {
        if (ch == '+' || ch == ' ' || ch == '\t') {
            if (!flush()) { return false; }
            word.clear();
        } else {
            word.push_back(ch);
        }
    }
    if (!flush()) { return false; }
    if (parsed.key == Key::Unknown) {
        return false;
    }
    if (spec != nullptr) {
        spec->modifiers = parsed.modifiers;
        spec->key = parsed.key;
    }
    return true;
}

std::string displayHotkey(const HotkeySpec& spec) {
    std::string text;
#if defined(__APPLE__)
    const char* superName = "Cmd+";
#elif defined(_WIN32)
    const char* superName = "Win+";
#else
    const char* superName = "Super+";
#endif
    if (spec.modifiers & kModCtrl)  { text += "Ctrl+"; }
    if (spec.modifiers & kModAlt)   { text += "Alt+"; }
    if (spec.modifiers & kModShift) { text += "Shift+"; }
    if (spec.modifiers & kModSuper) { text += superName; }
    text += keyName(spec.key);
    return text;
}

std::string defaultHotkeyText(char key) {
    std::string text;
#if defined(__APPLE__)
    text = "cmd+alt+";
    text.push_back(key);
#elif defined(__linux__)
    // Linux（银河麒麟 UKUI / Ubuntu 等）：Ctrl+Alt+T 默认为系统终端快捷键。
    // 为避免 XGrabKey 发生 BadAccess 冲突导致热键失效，Linux 默认将截图键设为 F1（对齐 Snipaste 习惯）。
    if (key == 't' || key == 'T') {
        return "f1";
    }
    text = "ctrl+alt+";
    text.push_back(key);
#else
    text = "ctrl+alt+";
    text.push_back(key);
#endif
    return text;
}

} // namespace jie2::platform
