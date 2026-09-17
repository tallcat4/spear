// S.P.E.A.R. core — 装置上の可観測性 (要件 §12.1, §13.1)
//
// フィールドでは装置の画面上で診断を完結させる。CPU 周波数・温度・throttling・ディスク残量・
// 電源状態を sysfs から読み、閾値超過を Core health event として EventBus へ流す。
// M-1(電源管理起因の断続性確認)の記録にもそのまま使う。
#pragma once

#include "event.hpp"

#include <atomic>
#include <string>
#include <thread>

namespace spear {

struct HealthSnapshot {
    uint64_t host_ns = 0;
    double   cpu_mhz_avg = 0;      // scaling_cur_freq 平均
    double   cpu_mhz_min = 0;
    std::string governor;          // "performance" / "powersave"
    double   pkg_temp_c = 0;       // x86_pkg_temp
    uint64_t core_throttle_count = 0;
    uint64_t pkg_throttle_count = 0;
    bool     on_ac = true;
    uint64_t disk_free_bytes = 0;
    double   load1 = 0;            // /proc/loadavg
    bool     usb_autosuspend_on = false; // /sys/module/usbcore/parameters/autosuspend > 0
    long     rtprio_limit = 0;           // RLIMIT_RTPRIO (0 = SCHED_FIFO 不可)
    long     memlock_limit_kb = 0;       // -1 = unlimited
    uint64_t rss_bytes = 0;              // 自プロセス
    uint64_t mem_available_bytes = 0;    // /proc/meminfo MemAvailable
};

HealthSnapshot read_health(const std::string& disk_path = ".");

class HealthMonitor {
public:
    HealthMonitor(EventBus* events, std::string disk_path, std::chrono::milliseconds period = std::chrono::seconds(1));
    ~HealthMonitor();
    HealthSnapshot latest() const;
    void set_disk_low_threshold(uint64_t bytes) { disk_low_ = bytes; }
private:
    void run();
    EventBus* events_;
    std::string disk_path_;
    std::chrono::milliseconds period_;
    std::atomic<uint64_t> disk_low_{1ull << 30}; // 1 GiB
    mutable std::mutex mu_;
    HealthSnapshot latest_;
    std::atomic<bool> stop_{false};
    std::thread th_;
};

} // namespace spear
