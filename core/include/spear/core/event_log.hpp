// S.P.E.A.R. core — Event log file (要件 §12.1 の補助)
//
// 画面を見ていなかった間の事象を残す。EventBus の全 event を JSON lines で追記し、
// サイズ上限でローテーションする(<path>, <path>.1, <path>.2 ...)。
#pragma once

#include "event.hpp"

#include <cstdio>
#include <mutex>
#include <string>

namespace spear {

class EventLogFile {
public:
    EventLogFile(EventBus& bus, std::string path, std::size_t max_bytes = 8u << 20, int keep = 3);
    ~EventLogFile();
    EventLogFile(const EventLogFile&) = delete;
    EventLogFile& operator=(const EventLogFile&) = delete;
    bool ok() const { return fp_ != nullptr; }
    uint64_t written() const { return written_; }
private:
    void write(const Event& e);
    void rotate_locked();
    EventBus& bus_;
    std::string path_;
    std::size_t max_bytes_;
    int keep_;
    int listener_ = 0;
    std::mutex mu_;
    std::FILE* fp_ = nullptr;
    std::size_t size_ = 0;
    uint64_t written_ = 0;
};

} // namespace spear
