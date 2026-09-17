// 同期語検出とフレーム収集(../std-t98-tools/core/rf/sync_word_correlator.py の SSE モード移植)。
// 直近 10 シンボルと同期語の二乗誤差 ≤ energy × ratio なら検出し、続く 182 シンボルを集めて 192 のフレームを出す。
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace spear::std_t98 {

inline constexpr std::array<float, 10> kSyncWord = {-3, +1, -3, +3, -3, -3, +3, +3, -1, +3};

struct SyncStats {
    uint64_t detections = 0;
    double last_metric = 0;     // 検出時の SSE
    double best_metric = 1e9;   // これまでの最小 SSE(閾値にどれだけ近づいたか)
    double threshold = 0;
};

class SyncCorrelator {
public:
    using FrameCallback = std::function<void(std::span<const float> symbols /*192*/, uint64_t first_symbol_index)>;
    explicit SyncCorrelator(double error_threshold_ratio = 0.2, int packet_len = 192);
    void set_threshold_ratio(double ratio);
    // symbols を 1 つずつ処理。symbol_index は provenance 用の通し番号(最初のシンボルの index)
    void process(std::span<const float> symbols, uint64_t first_symbol_index, const FrameCallback& on_frame);
    const SyncStats& stats() const { return st_; }
    bool collecting() const { return collecting_; }

private:
    double energy_ = 0, threshold_ = 0;
    int packet_len_;
    std::array<float, 10> shift_{};
    std::vector<float> packet_;
    bool collecting_ = false;
    uint64_t packet_start_index_ = 0;
    SyncStats st_;
};

} // namespace spear::std_t98
