#include "spear/core/tap.hpp"

#include <cstdlib>
#include <filesystem>

namespace spear::tap {

namespace {
std::string& directory() {
    static std::string dir = [] {
        const char* e = std::getenv("SPEAR_TAP_DIR");
        return std::string(e ? e : "tap");
    }();
    return dir;
}
bool& enabled_flag() {
    static bool en = [] {
        const char* e = std::getenv("SPEAR_TAP");
        return !(e && std::string(e) == "0");
    }();
    return en;
}
} // namespace

void Channel::set_directory(std::string dir) { directory() = std::move(dir); }
bool Channel::enabled() { return enabled_flag(); }

Channel::Channel(std::string id, DataType dtype) : id_(std::move(id)), dtype_(dtype) {}

Channel::~Channel() {
    if (fp_) std::fclose(fp_);
}

void Channel::write(const void* data, std::size_t bytes) {
    if (!enabled()) return;
    std::lock_guard lk(mu_);
    if (!tried_) {
        tried_ = true;
        std::error_code ec;
        std::filesystem::create_directories(directory(), ec);
        const std::string path = directory() + "/" + id_ + "." + std::string(to_string(dtype_)) + ".tap";
        fp_ = std::fopen(path.c_str(), "wb");
        if (!fp_) std::fprintf(stderr, "[tap] cannot open %s\n", path.c_str());
    }
    if (fp_) { std::fwrite(data, 1, bytes, fp_); std::fflush(fp_); } // クラッシュしても直前まで残す
}

} // namespace spear::tap
