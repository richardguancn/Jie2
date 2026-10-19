#pragma once

#include <map>
#include <string>
#include <vector>

#include "core/config.h"

// 「J2」应用内快捷键映射（专业版对齐项 v2.11.3：自定义部分应用内快捷键）
//
// 设计说明：
//   - 纯 C++、不依赖 Qt（core 层的约定）；键值与 Qt::Key 一致（包含功能键、方向键与符号键），
//     UI 层直接传 event->key() 即可，不需要两张映射表。
//   - 配置格式："t"、"shift+b"、"ctrl+alt+p"；"none" 或空串 = 禁用该动作。
//   - 用户设置的工具快捷键优先于同键的固定操作。

namespace jie2::core {

/// 修饰键位（core 自带一份，避免 core 反向依赖 platform 层）。
enum ShortcutModifier : unsigned {
    kScCtrl = 1u << 0,
    kScAlt = 1u << 1,
    kScShift = 1u << 2,
    kScMeta = 1u << 3,
};

/// 一次按键绑定。
struct ShortcutSpec {
    int key = 0;               // Qt-compatible key；0 = 未绑定（该动作键盘快捷键禁用）
    unsigned modifiers = 0;    // ShortcutModifier 位组合

    bool enabled() const { return key != 0; }
    friend bool operator==(const ShortcutSpec&, const ShortcutSpec&) = default;
};

/// 解析 "x" / "shift+b" / "ctrl+alt+p" / "none" / ""。
/// 返回是否解析成功（不合法时 out 不被修改）。
bool parseShortcutSpec(const std::string& text, ShortcutSpec* out);

/// 动作 → 按键 映射。动作 ID 见 actionIds()。
class ShortcutMap {
public:
    /// 出厂默认：与既有行为完全一致（T/N/E/M/D/R/O/B、Shift+B）。
    static ShortcutMap defaults();

    /// 从配置读取（键名 `shortcut.<动作ID>`）。只覆盖显式出现的键；
    /// 解析失败时保留默认值并把原因写入 warning（若非空）。
    static ShortcutMap fromConfig(const Config& config, std::string* warning);

    /// 全部可配置动作 ID（用于文档与配置界面）。
    static const std::vector<std::string>& actionIds();

    void set(const std::string& action, const ShortcutSpec& spec);
    ShortcutSpec get(const std::string& action) const;

    /// 是否命中某动作：键值与修饰键都要精确匹配
    /// （按了 Ctrl+T 不会触发绑为 "t" 的动作）。
    /// modifiers 为 kSc* 位组合；超出 4 位的标志会被忽略。
    bool matches(const std::string& action, int key, unsigned modifiers) const;

private:
    std::map<std::string, ShortcutSpec> specs_;
};

} // namespace jie2::core
