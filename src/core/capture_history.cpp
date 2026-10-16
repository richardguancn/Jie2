#include "core/capture_history.h"

#include <algorithm>
#include <sstream>

namespace jie2::core {

namespace {

std::string trim(const std::string& input) {
    std::size_t begin = 0;
    std::size_t end = input.size();
    while (begin < end && (input[begin] == ' ' || input[begin] == '\t' ||
                           input[begin] == '\r' || input[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (input[end - 1] == ' ' || input[end - 1] == '\t' ||
                           input[end - 1] == '\r' || input[end - 1] == '\n')) {
        --end;
    }
    return input.substr(begin, end - begin);
}

int parsePositiveInt(const std::string& text) {
    try {
        const long long value = std::stoll(text);
        return value > 0 ? static_cast<int>(value) : 0;
    } catch (...) {
        return 0;
    }
}

} // namespace

CaptureHistory::CaptureHistory(std::size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity) {}

void CaptureHistory::setCapacity(std::size_t capacity) {
    capacity_ = capacity == 0 ? 1 : capacity;
    while (entries_.size() > capacity_) {
        entries_.erase(entries_.begin());
    }
}

void CaptureHistory::append(HistoryEntry entry) {
    if (entry.path.empty()) {
        return;
    }
    // 同一文件再次成功截图时置顶而不是重复记录。
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const HistoryEntry& existing) {
                                      return existing.path == entry.path;
                                  }),
                   entries_.end());
    entries_.push_back(std::move(entry));
    while (entries_.size() > capacity_) {
        entries_.erase(entries_.begin());
    }
}

const HistoryEntry* CaptureHistory::latest() const {
    return entries_.empty() ? nullptr : &entries_.back();
}

const HistoryEntry* CaptureHistory::fromLatest(std::size_t offset) const {
    if (offset >= entries_.size()) {
        return nullptr;
    }
    return &entries_[entries_.size() - 1 - offset];
}

bool CaptureHistory::remove(const std::string& path) {
    const auto it = std::find_if(
        entries_.begin(), entries_.end(),
        [&path](const HistoryEntry& entry) { return entry.path == path; });
    if (it == entries_.end()) {
        return false;
    }
    entries_.erase(it);
    return true;
}

std::string CaptureHistory::serialize() const {
    std::ostringstream out;
    out << "# jie2 capture history (oldest first)\n";
    for (const auto& entry : entries_) {
        out << entry.path << '|' << entry.timestamp << '|' << entry.width
            << '|' << entry.height << '\n';
    }
    return out.str();
}

CaptureHistory CaptureHistory::parse(const std::string& text, std::size_t capacity) {
    CaptureHistory history(capacity);
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }
        // 路径可能包含 '|'（罕见但合法）；从右往左切四个字段，多出的归入路径。
        std::size_t last = trimmed.rfind('|');
        if (last == std::string::npos) {
            continue;
        }
        std::size_t third = trimmed.rfind('|', last - 1);
        if (third == std::string::npos) {
            continue;
        }
        std::size_t second = trimmed.rfind('|', third - 1);
        if (second == std::string::npos) {
            continue;
        }
        HistoryEntry entry;
        entry.path = trimmed.substr(0, second);
        entry.timestamp = trimmed.substr(second + 1, third - second - 1);
        entry.width = parsePositiveInt(trimmed.substr(third + 1, last - third - 1));
        entry.height = parsePositiveInt(trimmed.substr(last + 1));
        if (!entry.path.empty()) {
            history.append(std::move(entry));
        }
    }
    return history;
}

} // namespace jie2::core
