#include "spear/core/event.hpp"

namespace spear {

uint64_t host_now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string_view to_string(EventKind k) {
    switch (k) {
    case EventKind::ConsumerOverflow: return "consumer_overflow";
    case EventKind::ConsumerDrop:     return "consumer_drop";
    case EventKind::StartupStage:     return "startup_stage";
    case EventKind::Overflow:         return "overflow";
    case EventKind::OutOfSequence:    return "out_of_sequence";
    case EventKind::Timeout:          return "timeout";
    case EventKind::Discontinuity:    return "discontinuity";
    case EventKind::Underflow:        return "underflow";
    case EventKind::Retune:           return "retune";
    case EventKind::Disconnected:     return "disconnected";
    case EventKind::Reconnected:      return "reconnected";
    case EventKind::ClockReset:       return "clock_reset";
    case EventKind::ThermalThrottle:  return "thermal_throttle";
    case EventKind::DiskLow:          return "disk_low";
    case EventKind::DeviceState:      return "device_state";
    case EventKind::UhdLog:           return "uhd_log";
    case EventKind::TimeReference:    return "time_reference";
    case EventKind::LoUnlock:         return "lo_unlock";
    case EventKind::Info:             return "info";
    case EventKind::Warning:          return "warning";
    case EventKind::Error:            return "error";
    }
    return "?";
}

EventBus::EventBus(std::size_t history)
    : history_cap_(history), counts_(static_cast<std::size_t>(EventKind::Error) + 1, 0),
      th_([this] { run(); }) {}

EventBus::~EventBus() {
    {
        std::lock_guard lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    th_.join();
}

void EventBus::emit(Event ev) {
    if (ev.host_ns == 0) ev.host_ns = host_now_ns();
    {
        std::lock_guard lk(mu_);
        ++counts_[static_cast<std::size_t>(ev.kind)];
        history_.push_front(ev);
        if (history_.size() > history_cap_) history_.pop_back();
        queue_.push_back(std::move(ev));
    }
    cv_.notify_all();
}

void EventBus::emit(EventKind k, std::string source, std::string detail, SampleRange range, int64_t value) {
    emit(Event{k, std::move(source), range, 0, std::move(detail), value});
}

int EventBus::subscribe(Listener l) {
    std::lock_guard lk(mu_);
    const int id = next_id_++;
    listeners_.emplace_back(id, std::move(l));
    return id;
}

void EventBus::unsubscribe(int id) {
    std::lock_guard lk(mu_);
    std::erase_if(listeners_, [id](auto& p) { return p.first == id; });
}

std::vector<Event> EventBus::history() const {
    std::lock_guard lk(mu_);
    return {history_.begin(), history_.end()};
}

uint64_t EventBus::count(EventKind k) const {
    std::lock_guard lk(mu_);
    return counts_[static_cast<std::size_t>(k)];
}

void EventBus::flush() {
    std::unique_lock lk(mu_);
    cv_.wait(lk, [this] { return queue_.empty() && in_flight_ == 0; });
}

void EventBus::run() {
    std::unique_lock lk(mu_);
    for (;;) {
        cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
        if (stop_ && queue_.empty()) return;
        Event ev = std::move(queue_.front());
        queue_.pop_front();
        auto listeners = listeners_; // listener 内での subscribe/unsubscribe を許す
        ++in_flight_;
        lk.unlock();
        for (auto& [id, l] : listeners) l(ev);
        lk.lock();
        --in_flight_;
        cv_.notify_all();
    }
}

} // namespace spear
