#include "tracker.hpp"

namespace spear::std_t98::secret {

const char* to_string(TrackerStatus s) {
    switch (s) {
    case TrackerStatus::Idle: return "idle";
    case TrackerStatus::Collecting: return "collecting";
    case TrackerStatus::Pending: return "searching";
    case TrackerStatus::Keyed: return "keyed";
    case TrackerStatus::Miss: return "miss";
    }
    return "?";
}

bool Tracker::on_secret_burst(const Burst& b, Request* request) {
    if (!active_) {   // _start_secret_session
        ++session_;
        active_ = true;
        pending_ = false;
        missed_ = false;
        last_request_ = -1;
        window_.clear();
    }
    window_.push_back(b);
    while (static_cast<int>(window_.size()) > cfg_.max_window) window_.pop_front();
    const int64_t idx = burst_index_++;

    // _maybe_send_secret_request
    if (pending_ || static_cast<int>(window_.size()) < cfg_.min_window) return false;
    int take = 0;
    if (last_request_ < 0) take = cfg_.min_window;
    else {
        if (idx - last_request_ < cfg_.recheck_interval || static_cast<int>(window_.size()) < cfg_.max_window) return false;
        take = cfg_.max_window;
    }
    if (request) {
        request->session = session_;
        request->current_key = key_;
        request->bursts.assign(window_.end() - take, window_.end());
    }
    pending_ = true;
    last_request_ = idx;
    return true;
}

void Tracker::on_clear() {
    if (!active_) return;
    active_ = false;
    pending_ = false;
    last_request_ = -1;
    window_.clear();
}

void Tracker::on_result(uint32_t session, uint16_t key) {
    if (session != session_) return;
    pending_ = false;
    if (key > 0) { key_ = key; missed_ = false; }
    else missed_ = true;
}

TrackerStatus Tracker::status() const {
    if (!active_) return TrackerStatus::Idle;
    if (pending_) return TrackerStatus::Pending;
    if (missed_ && key_ == 0) return TrackerStatus::Miss;
    if (key_ > 0) return TrackerStatus::Keyed;
    return TrackerStatus::Collecting;
}

} // namespace spear::std_t98::secret
