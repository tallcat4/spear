// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 有界の先読みによる特徴量予測と、再帰(GRU)による音声制御値。原本: src/predictor.rs。
#pragma once

#include "numeric.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace spear::jr_train::voice {

using Controls = std::array<float, 82>;
using PayloadBytes = std::array<uint8_t, 20>;
inline constexpr std::size_t kLookahead = 3;

struct FeatureRecord {
    std::array<float, 198> features{};
    std::array<float, 4> periods{};
};

struct Prediction {
    Controls controls{};
    std::array<uint8_t, 158> bits{};
};

std::size_t native_bit(std::size_t half, std::size_t local);

class Predictor {
public:
    void reset();
    std::size_t pending() const { return count_ - cursor_; }
    Prediction emit();
    std::optional<Prediction> decode(const PayloadBytes& payload);

private:
    const FeatureRecord& at(int32_t index) const;
    std::array<float, 17> baseline(int32_t index) const;
    std::array<float, 70> gru(const std::array<float, 81>& features);
    void stabilize_pitch(int32_t index, Controls& values) const;
    void refine(int32_t index, Controls& values);

    std::array<FeatureRecord, 8> buffer_{};
    std::array<float, 64> state_{};
    std::optional<std::array<float, 17>> previous_context_;
    Numeric numeric_;
    std::size_t count_ = 0, cursor_ = 0;
};

} // namespace spear::jr_train::voice
