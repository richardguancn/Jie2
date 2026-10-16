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

} // namespace jie2::platform
