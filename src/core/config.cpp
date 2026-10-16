#include "core/config.h"

#include <cctype>
#include <sstream>

namespace jie2::core {

namespace {

std::string trim(const std::string& input) {
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

} // namespace

void Config::set(const std::string& key, const std::string& value) {
    if (key.empty()) {
        return;
    }
    entries_[key] = value;
}

std::string Config::get(const std::string& key, const std::string& fallback) const {
    const auto it = entries_.find(key);
    return it == entries_.end() ? fallback : it->second;
}

int Config::getInt(const std::string& key, int fallback) const {
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        return fallback;
    }
    try {
        std::size_t consumed = 0;
        const int value = std::stoi(it->second, &consumed);
        return consumed == it->second.size() ? value : fallback;
    } catch (...) {
        return fallback;
    }
}

bool Config::getBool(const std::string& key, bool fallback) const {
    const std::string value = get(key);
    if (value == "true" || value == "1") {
        return true;
    }
    if (value == "false" || value == "0") {
        return false;
    }
    return fallback;
}

std::string Config::serialize() const {
    std::ostringstream out;
    out << "# jie2 config\n";
    for (const auto& [key, value] : entries_) {
        out << key << '=' << value << '\n';
    }
    return out.str();
}

Config Config::parse(const std::string& text) {
    Config config;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
            continue;
        }
        const std::size_t equals = trimmed.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = trim(trimmed.substr(0, equals));
        const std::string value = trim(trimmed.substr(equals + 1));
        if (!key.empty()) {
            config.entries_[key] = value;
        }
    }
    return config;
}

} // namespace jie2::core
