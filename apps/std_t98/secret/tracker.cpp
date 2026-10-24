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

bool Tracker::on_secret_burst(const Burst& b, Request* request, const Valid4& valid) {
    if (!active_) {   // _start_secret_session
        ++session_;
        active_ = true;
        pending_ = false;
        missed_ = false;
        last_request_ = -1;
        window_.clear();
        validity_.clear();
        session_bursts_ = 0;
    }
    window_.push_back(b);
    validity_.push_back(valid);
    ++session_bursts_;
    while (static_cast<int>(window_.size()) > cfg_.max_window) { window_.pop_front(); validity_.pop_front(); }
    const int64_t idx = burst_index_++;

    // _maybe_send_secret_request
    if ((!cfg_.streaming && pending_) || static_cast<int>(window_.size()) < cfg_.min_window) return false;
    int take = 0;
    if (cfg_.streaming) take = static_cast<int>(window_.size());
    else if (last_request_ < 0) take = cfg_.min_window;
    else {
        if (idx - last_request_ < cfg_.recheck_interval || static_cast<int>(window_.size()) < cfg_.max_window) return false;
        take = cfg_.max_window;
    }
    if (request) {
        request->session = session_;
        request->current_key = key_;
        request->bursts.assign(window_.end() - take, window_.end());
        request->first_burst = session_bursts_ - static_cast<uint64_t>(take);
        request->valid.assign(validity_.end() - take, validity_.end());
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
    validity_.clear();
    if (cfg_.streaming) { key_ = 0; series_ = 0; confirmed_ = false; }
}

void Tracker::on_result(uint32_t session, uint16_t key) {
    if (session != session_) return;
    pending_ = false;
    if (key > 0) { key_ = key; missed_ = false; }
    else missed_ = true;
}

void Tracker::on_guided_result(uint32_t session, std::optional<SeriesKey> id) {
    if (!active_ || session != session_) return;
    pending_ = false;
    confirmed_ = id.has_value();
    key_ = id ? id->key : 0;
    series_ = id ? id->series : 0;
    missed_ = !id;
}

TrackerStatus Tracker::status() const {
    if (!active_) return TrackerStatus::Idle;
    if (pending_) return TrackerStatus::Pending;
    if (missed_ && key_ == 0) return TrackerStatus::Miss;
    if (key_ > 0 || confirmed_) return TrackerStatus::Keyed;
    return TrackerStatus::Collecting;
}

} // namespace spear::std_t98::secret
