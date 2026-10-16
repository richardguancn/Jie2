#pragma once

#include <cstddef>
#include <string>
#include <vector>

// 「截2」截图历史（M1：专业版对齐项 —— "截图记录回放"）
//
// 职责单一：维护"最近 N 张成功截图"的有序列表（最新在末尾），
// 并提供 , / . 回放所需的按偏移访问。纯内存 + 文本序列化 ——
// 文件读写由 UI 层负责，因此本类可在无文件系统的环境下单测。

namespace jie2::core {

struct HistoryEntry {
    std::string path;       // 截图文件路径
    std::string timestamp;  // "YYYYMMDD-HHMMSS"（排序与展示用）
    int width = 0;          // 物理像素宽
    int height = 0;         // 物理像素高
};

class CaptureHistory {
public:
    explicit CaptureHistory(std::size_t capacity = 16);

    std::size_t capacity() const { return capacity_; }

    /// 设置容量；设为 0 视为 1。缩容时从最旧一端丢弃。
    void setCapacity(std::size_t capacity);

    /// 追加一条为"最新"。重复路径会被先移除再加到末尾（回放同一文件时置顶）。
    /// 超过容量时丢弃最旧的。
    void append(HistoryEntry entry);

    const std::vector<HistoryEntry>& entries() const { return entries_; }
    std::size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }

    /// 最新一条；空列表返回 nullptr。
    const HistoryEntry* latest() const;

    /// 按"距最新"的偏移取：0=最新，1=前一张，2=再前一张……
    /// 越界返回 nullptr。Snipaste 的 , / . 回放即在此序列上前后移动。
    const HistoryEntry* fromLatest(std::size_t offset) const;

    /// 移除指定路径的条目（衣架右键"取下"用）。返回是否发生了移除。
    bool remove(const std::string& path);

    void clear() { entries_.clear(); }

    /// 每行一条："path|timestamp|width|height"。宽松解析（坏行跳过）。
    std::string serialize() const;
    static CaptureHistory parse(const std::string& text, std::size_t capacity = 16);

private:
    std::size_t capacity_;
    std::vector<HistoryEntry> entries_;
};

} // namespace jie2::core
