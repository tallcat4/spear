#include "spear/core/event_log.hpp"

#include <chrono>
#include <filesystem>

namespace spear {

namespace {
std::string jstr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\t': o += "\\t";  break;
        default:   if (static_cast<unsigned char>(c) < 0x20) o += ' '; else o += c;
        }
    }
    return o + "\"";
}
} // namespace

EventLogFile::EventLogFile(EventBus& bus, std::string path, std::size_t max_bytes, int keep)
    : bus_(bus), path_(std::move(path)), max_bytes_(max_bytes), keep_(keep) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path_).parent_path(), ec);
    fp_ = std::fopen(path_.c_str(), "ab");
    if (fp_) {
        std::error_code ec2;
        size_ = static_cast<std::size_t>(std::filesystem::file_size(path_, ec2));
        listener_ = bus_.subscribe([this](const Event& e) { write(e); });
    }
}

EventLogFile::~EventLogFile() {
    if (listener_) bus_.unsubscribe(listener_);
    std::lock_guard lk(mu_);
    if (fp_) std::fclose(fp_);
    fp_ = nullptr;
}

void EventLogFile::rotate_locked() {
    std::fclose(fp_);
    fp_ = nullptr;
    std::error_code ec;
    for (int i = keep_ - 1; i >= 1; --i)
        std::filesystem::rename(path_ + "." + std::to_string(i), path_ + "." + std::to_string(i + 1), ec);
    std::filesystem::rename(path_, path_ + ".1", ec);
    fp_ = std::fopen(path_.c_str(), "ab");
    size_ = 0;
}

void EventLogFile::write(const Event& e) {
    std::lock_guard lk(mu_);
    if (!fp_) return;
    if (size_ > max_bytes_) rotate_locked();
    if (!fp_) return;
    const auto utc = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const int n = std::fprintf(fp_,
        "{\"utc_ns\":%lld,\"host_ns\":%llu,\"kind\":%s,\"source\":%s,\"detail\":%s,"
        "\"generation\":%llu,\"begin\":%llu,\"end\":%llu,\"value\":%lld}\n",
        (long long)utc, (unsigned long long)e.host_ns, jstr(std::string(to_string(e.kind))).c_str(),
        jstr(e.source).c_str(), jstr(e.detail).c_str(), (unsigned long long)e.range.generation,
        (unsigned long long)e.range.begin, (unsigned long long)e.range.end, (long long)e.value);
    if (n > 0) { size_ += static_cast<std::size_t>(n); ++written_; }
    std::fflush(fp_);
}

} // namespace spear
