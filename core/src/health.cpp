#include "spear/core/health.hpp"

#include <filesystem>
#include <fstream>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace spear {

namespace {
template <class T>
bool read_sys(const std::string& path, T& out) {
    std::ifstream f(path);
    if (!f) return false;
    f >> out;
    return static_cast<bool>(f);
}
} // namespace

HealthSnapshot read_health(const std::string& disk_path) {
    namespace fs = std::filesystem;
    HealthSnapshot s;
    s.host_ns = host_now_ns();

    // CPU 周波数 / governor
    double sum = 0, mn = 1e12; int n = 0;
    for (int i = 0; i < 64; ++i) {
        const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/";
        double khz;
        if (!read_sys(base + "scaling_cur_freq", khz)) break;
        sum += khz / 1000.0; mn = std::min(mn, khz / 1000.0); ++n;
        if (i == 0) read_sys(base + "scaling_governor", s.governor);
    }
    if (n) { s.cpu_mhz_avg = sum / n; s.cpu_mhz_min = mn; }

    // 温度: x86_pkg_temp を探す
    for (int i = 0; i < 32; ++i) {
        const std::string base = "/sys/class/thermal/thermal_zone" + std::to_string(i) + "/";
        std::string type;
        if (!read_sys(base + "type", type)) break;
        if (type == "x86_pkg_temp") { double mc; if (read_sys(base + "temp", mc)) s.pkg_temp_c = mc / 1000.0; break; }
    }
    read_sys("/sys/devices/system/cpu/cpu0/thermal_throttle/core_throttle_count", s.core_throttle_count);
    read_sys("/sys/devices/system/cpu/cpu0/thermal_throttle/package_throttle_count", s.pkg_throttle_count);

    // 電源
    std::error_code ec;
    for (auto& e : fs::directory_iterator("/sys/class/power_supply", ec)) {
        std::string type;
        if (read_sys(e.path().string() + "/type", type) && type == "Mains") {
            int online = 1;
            if (read_sys(e.path().string() + "/online", online)) s.on_ac = online != 0;
        }
    }
    int autosuspend = 0;
    if (read_sys("/sys/module/usbcore/parameters/autosuspend", autosuspend)) s.usb_autosuspend_on = autosuspend > 0;

    struct statvfs vfs{};
    if (statvfs(disk_path.c_str(), &vfs) == 0) s.disk_free_bytes = static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize;
    read_sys("/proc/loadavg", s.load1);

    struct rlimit rl{};
    if (getrlimit(RLIMIT_RTPRIO, &rl) == 0) s.rtprio_limit = static_cast<long>(rl.rlim_cur);
    if (getrlimit(RLIMIT_MEMLOCK, &rl) == 0) s.memlock_limit_kb = rl.rlim_cur == RLIM_INFINITY ? -1 : static_cast<long>(rl.rlim_cur / 1024);
    {
        std::ifstream f("/proc/self/statm");
        uint64_t pages = 0, rss = 0;
        if (f >> pages >> rss) s.rss_bytes = rss * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
    }
    {
        std::ifstream f("/proc/meminfo");
        std::string key; uint64_t val; std::string unit;
        while (f >> key >> val >> unit) if (key == "MemAvailable:") { s.mem_available_bytes = val * 1024; break; }
    }
    return s;
}

HealthMonitor::HealthMonitor(EventBus* events, std::string disk_path, std::chrono::milliseconds period)
    : events_(events), disk_path_(std::move(disk_path)), period_(period), th_([this] { run(); }) {}

HealthMonitor::~HealthMonitor() {
    stop_ = true;
    th_.join();
}

HealthSnapshot HealthMonitor::latest() const {
    std::lock_guard lk(mu_);
    return latest_;
}

void HealthMonitor::run() {
    uint64_t last_throttle = 0;
    bool disk_low_reported = false;
    bool first = true;
    while (!stop_) {
        auto s = read_health(disk_path_);
        {
            std::lock_guard lk(mu_);
            latest_ = s;
        }
        if (events_) {
            const uint64_t thr = s.core_throttle_count + s.pkg_throttle_count;
            if (!first && thr != last_throttle)
                events_->emit(EventKind::ThermalThrottle, "health", "throttle count increased", {}, static_cast<int64_t>(thr));
            last_throttle = thr;
            if (s.disk_free_bytes < disk_low_ && !disk_low_reported) {
                disk_low_reported = true;
                events_->emit(EventKind::DiskLow, "health", "free space below threshold", {}, static_cast<int64_t>(s.disk_free_bytes));
            } else if (s.disk_free_bytes >= disk_low_) disk_low_reported = false;
        }
        first = false;
        for (int i = 0; i < 10 && !stop_; ++i) std::this_thread::sleep_for(period_ / 10);
    }
}

} // namespace spear
