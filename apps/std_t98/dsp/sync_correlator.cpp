#include "sync_correlator.hpp"

#include <algorithm>

namespace spear::std_t98 {

SyncCorrelator::SyncCorrelator(double ratio, int packet_len) : packet_len_(packet_len) {
    for (float s : kSyncWord) energy_ += s * s;
    set_threshold_ratio(ratio);
    packet_.reserve(static_cast<std::size_t>(packet_len_));
}

void SyncCorrelator::set_threshold_ratio(double ratio) {
    threshold_ = energy_ * ratio;
    st_.threshold = threshold_;
}

void SyncCorrelator::process(std::span<const float> symbols, uint64_t first_symbol_index, const FrameCallback& on_frame) {
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        const float s = symbols[i];
        if (collecting_) {
            packet_.push_back(s);
            if (packet_.size() >= static_cast<std::size_t>(packet_len_)) {
                on_frame(packet_, packet_start_index_);
                collecting_ = false;
                packet_.clear();
            }
            continue;
        }
        std::rotate(shift_.begin(), shift_.begin() + 1, shift_.end());
        shift_[9] = s;
        double sse = 0;
        for (int k = 0; k < 10; ++k) { const double d = shift_[k] - kSyncWord[k]; sse += d * d; }
        st_.best_metric = std::min(st_.best_metric, sse);
        if (sse <= threshold_) {
            st_.detections++;
            st_.last_metric = sse;
            collecting_ = true;
            packet_.assign(shift_.begin(), shift_.end());
            packet_start_index_ = first_symbol_index + i + 1 - 10;
        }
    }
}

} // namespace spear::std_t98
