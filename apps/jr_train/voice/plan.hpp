// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 固定の計画器: 1 フレームの追加先読みで、現在 3 通り × 将来 3 通りの試し合成から選ぶ。
// 原本: src/planner.rs、src/plan_features.rs、src/plan_target.rs、src/plan_acoustics.rs。
#pragma once

#include "predictor.hpp"
#include "vocoder.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace spear::jr_train::voice {

// ---- plan_features.rs(計画器用の特徴量。規範的なビット割当ではなく仮説)----
class PlanFeatures {
public:
    std::array<float, 220> process(const PayloadBytes& payload, const Controls& controls, bool reset);

private:
    std::array<std::array<std::array<float, 10>, 3>, 2> spectral_history_{};
    std::array<float, 4> gain_history_{};
    std::array<float, 28> previous_{}, average_{};
};

// ---- plan_target.rs ----
struct Target {
    std::array<float, 50> mean{};
    std::array<std::array<float, 141>, 3> risk{};
    std::array<float, 3> missing{};
};
class ModelState {
public:
    Target process(const std::array<float, 220>& features, const Controls& controls, bool reset);

private:
    std::array<float, 4> previous_{}, older_{};
};

// ---- plan_acoustics.rs ----
float frame_cost(const PcmFrame& pcm, const Target& target, const PcmFrame* past, const PcmFrame* before);

// ---- planner.rs ----
struct PlanInput {
    uint64_t key = 0;
    PayloadBytes payload{};
    Controls controls{};
    std::array<float, 320> source{}, weight{}, offset{};
    bool fade_in = false, fade_out = false;
};
struct PlanOutput {
    uint64_t index = 0;
    PcmFrame pcm{};
    std::array<float, 320> source{};
};
class Planner {
public:
    std::optional<PlanOutput> push(const PlanInput& input);
    std::optional<PlanOutput> finish() { return render(nullptr, nullptr); }

private:
    struct Pending {
        PlanInput input;
        Target target;
        bool reset = false;
    };
    std::optional<PlanOutput> render(const PlanInput* ahead_input, const Target* ahead_target);
    PlanFeatures features_;
    ModelState model_;
    Vocoder state_;
    std::optional<Pending> pending_;
    std::optional<PcmFrame> previous_, older_;
};

} // namespace spear::jr_train::voice
