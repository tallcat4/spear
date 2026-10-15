// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 共通の音源・フィルタ合成(制限の前に利得を有界に補間)。原本: src/vocoder.rs。
#pragma once

#include "numeric.hpp"
#include "predictor.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace spear::jr_train::voice {

using PcmFrame = std::array<int16_t, 320>;

__extension__ typedef unsigned __int128 u128;   // GCC / Clang の拡張(-Wpedantic 抑止)

// 決定的な雑音(PCG 系の 128 bit 状態)
class VoiceNoise {
public:
    uint64_t next_u64();
    float uniform();

private:
    u128 state_ = (static_cast<u128>(0xd335243fe783c82bull) << 64) | 0x844bb47f168b1cd2ull;
};

class Vocoder {
public:
    std::size_t original_choice = 4;
    Numeric numeric;
    std::array<float, 320> source_trace{};

    void reset() { *this = Vocoder{}; }
    // action: 0..=8(nullopt なら original_choice)。失敗は Failure
    PcmFrame process_explicit(const Controls& parameters, std::optional<std::size_t> action, const std::array<float, 320>* source, const std::array<float, 320>* curve,
                              bool restore);

private:
    static bool limit(float* values, std::size_t n);
    void excitation(float fraction, float period, float* output);
    void apply_policy(Controls& values, std::optional<std::size_t> action);
    void apply_spectrum(Controls& values);

    VoiceNoise noise_;
    std::array<float, 28> previous_features_{}, feature_average_{};
    std::array<float, 32> spectrum_average_{};
    std::array<float, 255> history_{};
    float phase_ = 0.f, previous_pulse_ = 0.f, fraction_ = 0.f, power_ = 0.f, level_ = 0.f;
    bool has_features_ = false, has_spectrum_ = false, has_excitation_ = false, has_power_ = false, has_level_ = false;
};

} // namespace spear::jr_train::voice
