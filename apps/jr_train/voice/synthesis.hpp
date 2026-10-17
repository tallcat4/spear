// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 4 フレームのパラメータ先読みを持つ、有界の共通合成パイプライン。
// 原本: src/synthesis.rs、src/synthesis_parameters.rs、src/source_features.rs。
#pragma once

#include "celp/celp.hpp"
#include "excitation.hpp"
#include "plan.hpp"
#include "vocoder.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <optional>

namespace spear::jr_train::voice {

// ---- source_features.rs ----
class SourceFeatures {
public:
    std::array<std::array<float, 570>, 2> process(const PayloadBytes& payload, const Controls& c);

private:
    PlanFeatures physical_;
    std::array<std::array<float, 190>, 2> previous_{};
    std::size_t age_ = 0;
};

struct SynthInput {
    uint64_t key = 0;
    PayloadBytes payload{};
    Controls controls{};
    bool fade_in = false, fade_out = false;
};
struct SynthOutput {
    uint64_t index = 0;
    PcmFrame pcm{};
};

class Pipeline {
public:
    bool has_pending() const { return received_ != returned_; }
    std::optional<SynthOutput> push(const SynthInput& input);
    std::optional<SynthOutput> finish();

private:
    struct SourceFrame {
        SynthInput input;
        Controls changed{};
        std::array<float, 320> source{};
        double strength = 0;
        std::array<std::array<float, 633>, 2> features{};
        std::array<std::array<float, 22>, 2> native_descriptor{};
        std::array<std::array<float, 24>, 2> gain_features{};
        std::size_t age = 0;
    };
    struct SelectedFrame {
        SynthInput input;
        std::array<std::array<float, 633>, 2> features{};
        std::array<std::array<float, 22>, 2> native_descriptor{};
        std::array<std::array<float, 24>, 2> gain_features{};
        std::size_t age = 0;
    };
    struct TemporalFrame {
        SynthInput input;
        std::array<float, 320> source{};
        std::array<float, 4> delta{};
        std::array<std::array<float, 128>, 2> embedding{};
    };
    struct GainFrame {
        SynthInput input;
        std::array<float, 320> source{};
        std::array<double, 4> gain{};
    };

    std::optional<SynthOutput> read();
    void choose(const PlanOutput& original);
    void resynthesize(const PlanOutput& selected);
    void advance_temporal(bool end);
    void emit(const GainFrame& frame, std::optional<double> following);

    celp::Native native_;
    SourceFeatures features_;
    Sources sources_;
    Vocoder base_synthesis_, candidate_synthesis_;
    Planner original_, selected_;
    std::deque<SourceFrame> source_queue_;
    std::deque<SelectedFrame> selected_queue_;
    Vocoder guard_trial_, gain_baseline_, final_synthesis_;
    std::deque<TemporalFrame> temporal_;
    std::size_t temporal_begin_ = 0, temporal_count_ = 0, temporal_next_ = 0;
    std::optional<GainFrame> pending_gain_;
    std::optional<double> previous_gain_;
    std::deque<SynthOutput> output_;
    std::size_t received_ = 0, returned_ = 0;
    std::optional<uint64_t> last_;
    bool finished_ = false;
};

} // namespace spear::jr_train::voice
