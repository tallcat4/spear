// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 原本: src/correction.rs
#include "correction.hpp"

#include "common.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>

namespace spear::jr_train::voice {
namespace {

float unit(float x) { return rs::clamp(x, 0.f, 1.f); }

std::array<uint8_t, 79> group(const std::array<uint8_t, 158>& bits, std::size_t half) {
    std::array<uint8_t, 79> out{};
    std::copy_n(bits.begin() + static_cast<std::ptrdiff_t>(half * 39), 39, out.begin());
    std::copy_n(bits.begin() + static_cast<std::ptrdiff_t>(78 + half * 20), 20, out.begin() + 39);
    std::copy_n(bits.begin() + static_cast<std::ptrdiff_t>(118 + half * 20), 20, out.begin() + 59);
    return out;
}
std::size_t read(const std::array<uint8_t, 79>& g, std::initializer_list<std::size_t> positions) {
    std::size_t r = 0;
    for (std::size_t p : positions) r = 2 * r + g[p];
    return r;
}
void features(const Controls& values, std::size_t half, float* x) {
    std::copy_n(values.begin() + static_cast<std::ptrdiff_t>(6 + half * 32), 32, x);
    x[32] = values[4];
    x[33] = std::log(rs::clamp(values[5], 20.f, 160.f));
    x[34] = values[half * 2];
    x[35] = values[half * 2 + 1];
}

} // namespace

Controls Correction::process(const std::array<uint8_t, 158>& bits, const Controls& input, Numeric& numeric) {
    const auto& d = correction_model();
    for (uint8_t b : bits)
        if (b > 1) throw Failure{};
    for (float x : input)
        if (!std::isfinite(x)) throw Failure{};
    Controls values = input;
    const float frame_level = (input[0] + input[1] + input[2] + input[3]) / 4.f;
    float top = -std::numeric_limits<float>::infinity();
    for (std::size_t i = 0; i < 4; ++i) top = rs::max(top, input[i]);
    const float positive_gate = unit((-10.f - top) / 8.f);
    constexpr std::array<float, 4> kGainWeights = {1.f / 2.734375f, 0.75f / 2.734375f, 0.5625f / 2.734375f, 0.421875f / 2.734375f};
    for (std::size_t slot = 0; slot < 4; ++slot) {
        const auto g = group(bits, slot / 2);
        std::size_t code = 0;
        for (std::size_t j = 0; j < 7; ++j) code = 2 * code + g[20 + 12 * (slot % 2) + j];
        if (!has_gain_) {
            gain_history_.fill(code);
        } else {
            std::copy_backward(gain_history_.begin(), gain_history_.begin() + 3, gain_history_.begin() + 4);
            gain_history_[0] = code;
        }
        const float level = input[slot];
        std::array<float, 12> x = {level,
                                   (has_gain_ ? previous_level_ : level) - level,
                                   (slot == 3 ? level : input[slot + 1]) - level,
                                   input[70 + slot],
                                   std::log(rs::clamp(input[74 + slot], 20.f, 160.f)),
                                   static_cast<float>(slot % 2),
                                   input[70 + slot] * level,
                                   frame_level,
                                   0.f,
                                   0.f,
                                   0.f,
                                   0.f};
        std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(6 + (slot / 2) * 32), 4, x.begin() + 8);
        float delta = 0.f;
        for (std::size_t t = 0; t < 4; ++t) delta += kGainWeights[t] * d.GAIN_WEIGHT[gain_history_[t]];
        for (std::size_t j = 0; j < 12; ++j) delta += rs::clamp((x[j] - d.GAIN_MEAN[j]) / d.GAIN_SCALE[j], -6.f, 6.f) * d.GAIN_WEIGHT[128 + j];
        delta += d.GAIN_WEIGHT[140];
        delta = unit((level + 60.f) / 10.f) * rs::clamp(delta, -6.f, 6.f);
        values[slot] += rs::min(delta, 0.f) + rs::max(delta, 0.f) * positive_gate;
        previous_level_ = level;
        has_gain_ = true;
    }
    constexpr std::array<float, 4> kSpectralWeights = {1.f / 1.875f, 0.5f / 1.875f, 0.25f / 1.875f, 0.125f / 1.875f};
    for (std::size_t half = 0; half < 2; ++half) {
        const auto g = group(bits, half);
        const std::size_t main = read(g, {1, 2, 3, 4, 5, 6, 7});
        const std::array<std::size_t, 3> categories = {static_cast<std::size_t>(g[0]) * 256 + main, static_cast<std::size_t>(g[0]) * 256 + 128 + read(g, {8, 9, 10, 11, 40, 42}),
                                                       static_cast<std::size_t>(g[0]) * 256 + 192 + read(g, {39, 41, 59, 60, 61, 62})};
        if (!has_spectrum_) {
            spectral_history_.fill(categories);
            main_history_.fill(main);
        } else {
            std::copy_backward(spectral_history_.begin(), spectral_history_.begin() + 3, spectral_history_.begin() + 4);
            spectral_history_[0] = categories;
            std::copy_backward(main_history_.begin(), main_history_.begin() + 2, main_history_.begin() + 3);
            main_history_[0] = main;
        }
        const float gate = unit(((values[half * 2] + values[half * 2 + 1]) / 2.f + 60.f) / 10.f);
        std::array<float, 145> x{};
        std::array<float, 32> delta{};
        features(values, half, x.data());
        for (std::size_t j = 0; j < 16; ++j) {
            float value = 0.f;
            for (std::size_t t = 0; t < 4; ++t)
                for (std::size_t field = 0; field < 3; ++field) value += kSpectralWeights[t] * d.linear_coefficient(spectral_history_[t][field] * 16 + j);
            for (std::size_t i = 0; i < 36; ++i) value += rs::clamp((x[i] - d.LINEAR_MEAN[i]) / d.LINEAR_SCALE[i], -6.f, 6.f) * d.linear_coefficient((512 + i) * 16 + j);
            value += d.linear_coefficient(548 * 16 + j);
            delta[j] = 0.5f * value * gate;
        }
        numeric.bound_correction(delta.data());
        for (std::size_t j = 0; j < 32; ++j) values[6 + half * 32 + j] += delta[j];
        features(values, half, x.data());
        for (std::size_t j = 0; j < 79; ++j) x[36 + j] = 1.f - 2.f * static_cast<float>(g[j]);
        for (std::size_t t = 0; t < 3; ++t)
            for (std::size_t j = 0; j < 10; ++j) x[115 + t * 10 + j] = static_cast<float>(d.MAIN_TABLE[main_history_[t] * 10 + j]) / 8192.f;
        for (std::size_t j = 0; j < 145; ++j) x[j] = (x[j] - d.STUDENT_X_MEAN[j]) / d.STUDENT_X_SCALE[j];
        std::array<float, 24> first{};
        std::array<float, 12> second{};
        std::array<float, 16> prediction{};
        d.STUDENT_LAYER_0_WEIGHT.dense(x, d.STUDENT_LAYER_0_BIAS.data(), first.data());
        for (auto& v : first) v = std::tanh(v);
        d.STUDENT_LAYER_1_WEIGHT.dense(first, d.STUDENT_LAYER_1_BIAS.data(), second.data());
        for (auto& v : second) v = std::tanh(v);
        d.STUDENT_LAYER_2_WEIGHT.dense(second, d.STUDENT_LAYER_2_BIAS.data(), prediction.data());
        delta.fill(0.f);
        for (std::size_t j = 0; j < 16; ++j) delta[j] = (prediction[j] * d.STUDENT_Y_SCALE[j] + d.STUDENT_Y_MEAN[j]) * gate;
        numeric.bound_correction(delta.data());
        for (std::size_t j = 0; j < 32; ++j) values[6 + half * 32 + j] += delta[j];
        has_spectrum_ = true;
    }
    for (float v : values)
        if (!std::isfinite(v)) throw Failure{};
    return values;
}

} // namespace spear::jr_train::voice
