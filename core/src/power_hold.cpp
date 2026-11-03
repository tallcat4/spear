#include "spear/core/power_hold.hpp"

#include <systemd/sd-bus.h>

namespace spear {

namespace {
constexpr const char* kService = "org.freedesktop.UPower.PowerProfiles";
constexpr const char* kPath = "/org/freedesktop/UPower/PowerProfiles";
constexpr const char* kInterface = "org.freedesktop.UPower.PowerProfiles";
constexpr uint64_t kCallTimeoutUs = 2'000'000;            // ppd が応答しなくても App の起動を待たせない
constexpr auto kRetry = std::chrono::seconds(10);          // 掛け直しの最短間隔
} // namespace

PerformanceHold::PerformanceHold(EventBus* events) : events_(events) {}

PerformanceHold::~PerformanceHold() {
    std::lock_guard lk(mu_);
    drop_locked();
    if (bus_) sd_bus_flush_close_unref(bus_);
}

void PerformanceHold::acquire(const std::string& reason) {
    std::lock_guard lk(mu_);
    wanted_ = true;
    reason_ = reason;
    if (!cookie_) hold_locked();
}

void PerformanceHold::release() {
    std::lock_guard lk(mu_);
    wanted_ = false;
    mismatch_reported_ = false;
    drop_locked();
}

void PerformanceHold::check(const std::string& epp) {
    std::lock_guard lk(mu_);
    if (!wanted_ || epp.empty()) return;
    if (epp == "performance") {
        mismatch_reported_ = false;
        return;
    }
    if (std::chrono::steady_clock::now() - last_try_ < kRetry) return;
    // 要求中なのに performance でない: ppd が hold を外した(SetProfile)、まだ取れていない、または ppd が EPP を書けない。
    // 掛け直しは続け、event は performance でなくなったときに 1 回だけ出す
    const bool had = cookie_.has_value();
    drop_locked(false);
    const bool ok = hold_locked();
    if (ok && events_ && !mismatch_reported_)
        events_->emit(EventKind::Warning, "power", std::string(had ? "EPP is " : "performance hold acquired late; EPP is ") + epp + " while holding performance; re-acquiring every 10 s");
    if (ok) mismatch_reported_ = true;
}

bool PerformanceHold::wanted() const {
    std::lock_guard lk(mu_);
    return wanted_;
}

bool PerformanceHold::held() const {
    std::lock_guard lk(mu_);
    return cookie_.has_value();
}

bool PerformanceHold::hold_locked() {
    last_try_ = std::chrono::steady_clock::now();
    if (!bus_) {
        if (sd_bus_open_system(&bus_) < 0) {
            bus_ = nullptr;
            if (events_ && !warned_) events_->emit(EventKind::Warning, "power", "performance hold: cannot open the system bus");
            warned_ = true;
            return false;
        }
        sd_bus_set_method_call_timeout(bus_, kCallTimeoutUs);
    }
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    uint32_t cookie = 0;
    int r = sd_bus_call_method(bus_, kService, kPath, kInterface, "HoldProfile", &err, &reply, "sss", "performance", reason_.c_str(), "spear");
    if (r >= 0) r = sd_bus_message_read(reply, "u", &cookie);
    const std::string why = err.message ? err.message : (r < 0 ? "error " + std::to_string(-r) : "");
    sd_bus_message_unref(reply);
    sd_bus_error_free(&err);
    if (r < 0) {
        if (events_ && !warned_) events_->emit(EventKind::Warning, "power", "performance hold refused: " + why);
        warned_ = true;
        return false;
    }
    cookie_ = cookie;
    warned_ = false;
    if (events_ && !mismatch_reported_) events_->emit(EventKind::Info, "power", "performance hold acquired: " + reason_, {}, cookie);
    return true;
}

void PerformanceHold::drop_locked(bool report) {
    if (!cookie_ || !bus_) {
        cookie_.reset();
        return;
    }
    // ppd が既に外していれば失敗する(無視してよい)
    sd_bus_call_method(bus_, kService, kPath, kInterface, "ReleaseProfile", nullptr, nullptr, "u", *cookie_);
    if (events_ && report) events_->emit(EventKind::Info, "power", "performance hold released", {}, *cookie_);
    cookie_.reset();
}

} // namespace spear
