// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 原本: src/vocoder.rs
#include "vocoder.hpp"

#include "common.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace spear::jr_train::voice {

uint64_t VoiceNoise::next_u64() {
    const u128 mul = (static_cast<u128>(0x2360ed051fc65da4ull) << 64) | 0x4385df649fccf645ull;
    const u128 inc = (static_cast<u128>(0x8206c549833fe2efull) << 64) | 0x4a7c4f547dcdc3afull;
    state_ = state_ * mul + inc;
    const auto hi = static_cast<uint64_t>(state_ >> 64);
    return std::rotr(hi ^ static_cast<uint64_t>(state_), static_cast<int>(hi >> 58));
}

float VoiceNoise::uniform() {
    const float s = std::sqrt(3.f);
    return -s + 2.f * s * (static_cast<float>(next_u64() >> 11) * static_cast<float>(1.0 / 9007199254740992.0));
}

bool Vocoder::limit(float* values, std::size_t n) {
    constexpr float kCeiling = 32767.f / 32768.f;
    float peak = 0.f;
    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(values[i])) return false;
        peak = rs::max(peak, std::abs(values[i]));
    }
    if (peak > kCeiling)
        for (std::size_t i = 0; i < n; ++i) values[i] = rs::clamp(values[i] * (kCeiling / peak), -kCeiling, kCeiling);
    return true;
}

void Vocoder::excitation(float fraction, float period, float* output) {
    float mapped = (fraction == 0.f || fraction == 1.f) ? fraction : -std::expm1(4.f * std::log1p(-fraction));
    if (has_excitation_) mapped = 0.0625f * fraction_ + (1.f - 0.0625f) * mapped;
    fraction_ = mapped;
    has_excitation_ = true;
    const float root_period = std::sqrt(period);
    const float normalization = std::sqrt(1.f - 1.f / period);
    for (std::size_t i = 0; i < 80; ++i) {
        const float pulse = ((std::floor(phase_ + static_cast<float>(i + 1) / period) - std::floor(phase_ + static_cast<float>(i) / period)) * root_period - 1.f / root_period) /
                            normalization;
        const float smoothed = 0.875f * pulse + 0.125f * previous_pulse_;
        previous_pulse_ = pulse;
        const float noise = noise_.uniform();
        output[i] = mapped == 0.f ? noise : std::sqrt(mapped) * smoothed + std::sqrt(1.f - mapped) * noise;
    }
    phase_ = std::fmod(phase_ + 80.f / period, 1.f);
}

void Vocoder::apply_policy(Controls& values, std::optional<std::size_t> action) {
    const auto& m = predictor_model();
    std::array<float, 28> current{};
    for (std::size_t i = 0; i < 4; ++i) {
        current[i] = rs::clamp(values[i], -120.f, -3.f);
        current[4 + i] = rs::clamp(values[70 + i], 0.f, 1.f);
        current[8 + i] = std::log(rs::clamp(values[74 + i], 20.f, 160.f));
    }
    for (std::size_t i = 0; i < 8; ++i) {
        current[12 + i] = rs::clamp(values[6 + i], -4.f, 4.f);
        current[20 + i] = rs::clamp(values[38 + i], -4.f, 4.f);
    }
    std::array<float, 84> input{};
    for (std::size_t i = 0; i < 28; ++i) {
        input[i] = current[i];
        input[28 + i] = has_features_ ? current[i] - previous_features_[i] : 0.f;
        input[56 + i] = has_features_ ? 0.75f * feature_average_[i] + 0.25f * current[i] : current[i];
        previous_features_[i] = current[i];
        feature_average_[i] = input[56 + i];
    }
    has_features_ = true;
    normalize_in_place(input.data(), m.VOICING_X_MEAN, m.VOICING_X_SCALE, true);
    std::array<float, 2> costs{};
    affine(input, m.VOICING_WEIGHT, m.VOICING_BIAS.data, costs.data());
    const float low = rs::clamp(costs[0], -40.f, 40.f), high = rs::clamp(costs[1], -40.f, 40.f);
    original_choice = (low < 0.f && low <= high) ? 0 : (high < 0.f ? 8 : 4);
    const std::size_t selected = action.value_or(original_choice);
    for (std::size_t i = 70; i < 74; ++i) {
        const std::size_t choice = i < 72 ? selected / 3 : selected % 3;
        if (choice == 1) continue;
        const float exponent = choice == 0 ? 0.5f : 2.f;
        float& x = values[i];
        x = rs::clamp(-std::expm1(exponent * std::log1p(-rs::clamp(x, 0.f, 1.f))), 0.f, 1.f);
    }
}

void Vocoder::apply_spectrum(Controls& values) {
    std::array<float, 64> original{};
    std::copy_n(values.begin() + 6, 64, original.begin());
    for (std::size_t half = 0; half < 2; ++half) {
        std::array<float, 32> proposal{}, delta{};
        for (std::size_t i = 0; i < 32; ++i) {
            const float normalized = static_cast<float>(std::max(static_cast<int32_t>(i) + 1 - 8, 0)) / 24.f;
            const float value = original[half * 32 + i] * std::exp(-0.5f * normalized * normalized);
            proposal[i] = has_spectrum_ ? 0.75f * value + 0.25f * spectrum_average_[i] : value;
            delta[i] = proposal[i] - original[half * 32 + i];
        }
        numeric.bound_correction(delta.data());
        for (std::size_t i = 0; i < 32; ++i) values[6 + half * 32 + i] = original[half * 32 + i] + delta[i];
        spectrum_average_ = proposal;
        has_spectrum_ = true;
    }
}

PcmFrame Vocoder::process_explicit(const Controls& parameters, std::optional<std::size_t> action, const std::array<float, 320>* source, const std::array<float, 320>* curve,
                                   bool restore) {
    if (action && *action > 8) throw Failure{};
    for (float x : parameters)
        if (!std::isfinite(x)) throw Failure{};
    Controls values = parameters;
    apply_policy(values, action);
    apply_spectrum(values);
    std::array<float, 4> levels{};
    for (std::size_t i = 0; i < 4; ++i) levels[i] = rs::clamp(values[i], -120.f, -3.f);
    std::array<float, 320> pcm{};
    std::array<float, 256> impulse{};
    for (std::size_t block = 0; block < 4; ++block) {
        if (block % 2 == 0 && !numeric.envelope_impulse(values.data() + 6 + (block / 2) * 32, impulse)) throw Failure{};
        const float slope = rs::clamp(values[78 + block] + levels[block] - (has_level_ ? level_ : levels[block]), -18.f, 18.f);
        std::array<float, 335> extended{};
        std::copy(history_.begin(), history_.end(), extended.begin());
        excitation(rs::clamp(values[70 + block], 0.f, 1.f), rs::clamp(values[74 + block], 20.f, 160.f), extended.data() + 255);
        if (source) {
            const float* src = source->data() + block * 80;
            float power = 0.f;
            for (std::size_t i = 0; i < 80; ++i) power = power + src[i] * src[i];
            if (!std::isfinite(power)) throw Failure{};
            if (power != 0.f) std::copy_n(src, 80, extended.begin() + 255);
        }
        std::copy_n(extended.begin() + 255, 80, source_trace.begin() + static_cast<std::ptrdiff_t>(block * 80));
        float slope_gain = std::pow(10.f, -slope / 40.f);
        const float slope_step = std::pow(10.f, slope / (20.f * 79.f));
        std::array<float, 80> shaped{};
        for (std::size_t i = 0; i < 80; ++i)
            for (std::size_t j = 0; j < 128; ++j) shaped[i] += extended[255 + i - j] * impulse[j];
        for (auto& s : shaped) {
            s *= slope_gain;
            slope_gain *= slope_step;
        }
        std::copy(extended.begin() + 80, extended.end(), history_.begin());
        float power = 0.f;
        for (float s : shaped) power = power + s * s;
        power /= 80.f;
        if (!std::isfinite(power) || power <= 0.f) throw Failure{};
        power_ = has_power_ ? 0.95f * power_ + (1.f - 0.95f) * power : power;
        has_power_ = true;
        const float bound = std::pow(10.f, 0.6f);
        const float denominator = rs::clamp(power_, power / bound, power * bound);
        const float gain = std::pow(10.f, levels[block] / 20.f) / std::sqrt(denominator);
        for (auto& s : shaped) s *= gain;
        if (curve) {
            for (std::size_t n = 0; n < 80; ++n) {
                const float value = (*curve)[block * 80 + n];
                if (!std::isfinite(value) || value <= 0.f || value > 4.f) throw Failure{};
                shaped[n] *= value;
            }
        }
        if (!limit(shaped.data(), shaped.size())) throw Failure{};
        std::copy(shaped.begin(), shaped.end(), pcm.begin() + static_cast<std::ptrdiff_t>(block * 80));
        level_ = levels[block];
        has_level_ = true;
    }
    float own = 0.f;
    for (float x : pcm) own = own + x * x;
    own /= 320.f;
    float target = 0.f;
    for (float l : levels) target = target + std::pow(10.f, l / 10.f);
    target /= 4.f;
    const float restoration = restore ? std::sqrt(target / own) : 1.f;
    for (auto& x : pcm) x *= restoration;
    if (!limit(pcm.data(), pcm.size())) throw Failure{};
    PcmFrame out{};
    for (std::size_t i = 0; i < 320; ++i) out[i] = sample(pcm[i]);
    return out;
}

} // namespace spear::jr_train::voice
