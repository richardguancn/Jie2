#pragma once

#include <map>
#include <string>

// 「J2」配置存储（M0 极简实现）
//
// 键值对 + INI 风格文本序列化。选这个形态而不是 JSON 的原因：
// 内核层不允许依赖第三方库，而 M0 的配置项都是标量，自解析成本最低。
// 后续里程碑若出现结构化配置需求（标注样式集合、OCR 后端列表），
// 再在 UI 层引入 JSON，内核此处保持只读视图即可。

namespace jie2::core {

class Config {
public:
    void set(const std::string& key, const std::string& value);
    std::string get(const std::string& key, const std::string& fallback = {}) const;
    int getInt(const std::string& key, int fallback = 0) const;
    bool getBool(const std::string& key, bool fallback = false) const;

    const std::map<std::string, std::string>& entries() const { return entries_; }
    bool empty() const { return entries_.empty(); }

    std::string serialize() const;

    /// 解析失败的行会被跳过（宽松解析），保证损坏的配置文件不会导致启动失败。
    static Config parse(const std::string& text);

private:
    std::map<std::string, std::string> entries_;
};

} // namespace jie2::core
