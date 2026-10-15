// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 原本: crates/receiver-core/src/monitor.rs
#include "call_monitor.hpp"

#include <algorithm>
#include <tuple>

namespace spear::jr_train {

namespace {
constexpr uint64_t kCallGapUs = 1'000'000;
constexpr uint64_t kAcquisitionUs = 120'000;
uint64_t sat_sub(uint64_t a, uint64_t b) { return a > b ? a - b : 0; }
} // namespace

CallMonitor::CallMonitor(std::vector<std::string> ids) {
    const auto now = now_();
    for (auto& id : ids) tracks_.push_back(Track{std::move(id), 0, now, {}, std::nullopt});
}

void CallMonitor::update(std::size_t channel, uint64_t progress_us, const std::vector<Activity>& activity) {
    auto& t = tracks_.at(channel);
    t.progress = progress_us;
    t.updated = now_();
    t.pending.insert(t.pending.end(), activity.begin(), activity.end());
    advance();
}

void CallMonitor::finish() {
    finished_ = true;
    advance();
}

void CallMonitor::advance() {
    const auto now = now_();
    const auto live = [&](const Track& t) { return now - t.updated < std::chrono::seconds(1); };
    std::optional<uint64_t> watermark;
    for (const auto& t : tracks_) {
        if (!live(t)) continue;
        const uint64_t w = sat_sub(t.progress, finished_ ? 0 : kAcquisitionUs);
        watermark = watermark ? std::min(*watermark, w) : w;
    }
    for (auto& t : tracks_) {
        if (!live(t)) {
            t.call.reset();
            t.pending.clear();
            continue;
        }
        if (!watermark) continue;
        while (!t.pending.empty() && t.pending.front().time_us <= *watermark) {
            const Activity a = t.pending.front();
            t.pending.pop_front();
            if (a.kind == Activity::Reset) {
                t.call.reset();
            } else if (!t.call || sat_sub(a.time_us, t.call->last) >= kCallGapUs) {
                t.call = Call{a.time_us, a.time_us};
            } else {
                t.call->last = a.time_us;
            }
        }
        if (t.call && sat_sub(*watermark, t.call->last) >= kCallGapUs) t.call.reset();
    }
}

std::pair<std::optional<std::string>, std::vector<std::string>> CallMonitor::snapshot() {
    advance();
    std::vector<std::tuple<uint64_t, std::size_t, std::string>> calls;
    for (std::size_t i = 0; i < tracks_.size(); ++i)
        if (tracks_[i].call) calls.emplace_back(tracks_[i].call->start, i, tracks_[i].id);
    std::sort(calls.begin(), calls.end(), [](const auto& a, const auto& b) { return std::tie(std::get<0>(a), std::get<1>(a)) < std::tie(std::get<0>(b), std::get<1>(b)); });
    std::pair<std::optional<std::string>, std::vector<std::string>> out;
    if (!calls.empty()) out.first = std::get<2>(calls.front());
    for (auto& c : calls) out.second.push_back(std::get<2>(c));
    return out;
}

} // namespace spear::jr_train
