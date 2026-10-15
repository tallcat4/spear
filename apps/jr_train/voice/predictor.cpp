// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 原本: src/predictor.rs
#include "predictor.hpp"

#include "common.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace spear::jr_train::voice {
namespace {

std::size_t read_positions(const uint8_t* bits, const std::size_t* positions, std::size_t n) {
    std::size_t r = 0;
    for (std::size_t i = 0; i < n; ++i) r = 2 * r + bits[positions[i]];
    return r;
}
std::size_t read_list(const uint8_t* bits, std::initializer_list<std::size_t> positions) { return read_positions(bits, positions.begin(), positions.size()); }

FeatureRecord encode_features(const PayloadBytes& payload) {
    const auto& m = predictor_model();
    FeatureRecord result;
    std::array<uint8_t, 158> bits{};
    for (std::size_t i = 0; i < 158; ++i) bits[i] = (payload[i / 8] >> (i % 8)) & 1;
    for (std::size_t half = 0; half < 2; ++half) {
        std::array<uint8_t, 79> local{};
        std::copy_n(bits.begin() + static_cast<std::ptrdiff_t>(half * 39), 39, local.begin());
        std::copy_n(bits.begin() + static_cast<std::ptrdiff_t>(78 + half * 20), 20, local.begin() + 39);
        std::copy_n(bits.begin() + static_cast<std::ptrdiff_t>(118 + half * 20), 20, local.begin() + 59);
        const auto i = static_cast<int32_t>(read_list(local.data(), {12, 13, 14, 15, 16, 17, 46, 44}));
        const auto j = static_cast<int32_t>(read_list(local.data(), {27, 28, 29, 54, 52}));
        const int32_t anchor = i < 197 ? (i + 2) / 3 + 19 : i - 112;
        const std::array<float, 2> pitch = {i < 197 ? static_cast<float>(i + 58) / 3.f : static_cast<float>(i) - 112.f,
                                            static_cast<float>(std::clamp(anchor - 5, 20, 134)) + static_cast<float>(j - 2) / 3.f};
        std::array<float, 17> features{};
        for (std::size_t field = 0; field < 7; ++field) {
            const std::size_t row = m.PROFILE_OFFSETS[field] + read_positions(local.data(), m.PROFILE_GROUPS[field].data(), m.PROFILE_LENGTHS[field]);
            for (std::size_t k = 0; k < 17; ++k) features[k] += m.PROFILE_WEIGHT.get(row * 17 + k);
        }
        for (std::size_t slot = 0; slot < 2; ++slot) {
            result.periods[half * 2 + slot] = pitch[slot];
            const float log_period = std::log(rs::clamp(pitch[slot], 20.f, 160.f));
            result.features[34 + half * 2 + slot] = log_period;
            for (std::size_t k = 0; k < 17; ++k) features[k] += log_period * m.PROFILE_WEIGHT.get((648 + slot) * 17 + k);
        }
        for (std::size_t k = 0; k < 17; ++k) features[k] += m.PROFILE_WEIGHT.get(650 * 17 + k);
        std::copy(features.begin(), features.end(), result.features.begin() + static_cast<std::ptrdiff_t>(half * 17));
        result.features[38 + half] = static_cast<float>(local[0]);
    }
    for (std::size_t i = 0; i < 158; ++i) result.features[40 + i] = 1.f - 2.f * static_cast<float>(bits[i]);
    return result;
}

struct MlpOut {
    std::array<float, 17> output{};
    std::array<float, 32> hidden{};
};
MlpOut mlp(const Mlp& model, std::span<const float> input) {
    std::array<float, 1007> normalized{};
    normalize(input, model.x_mean, model.x_scale, normalized.data(), false);
    std::array<float, 64> first{};
    affine(normalized, model.weight[0], model.bias[0].data, first.data());
    for (std::size_t i = 0; i < model.weight[0].cols; ++i) first[i] = std::tanh(first[i]);
    MlpOut r;
    affine(first, model.weight[1], model.bias[1].data, r.hidden.data());
    for (auto& x : r.hidden) x = std::tanh(x);
    affine(r.hidden, model.weight[2], model.bias[2].data, r.output.data());
    for (std::size_t i = 0; i < 17; ++i) r.output[i] = r.output[i] * model.y_scale.get(i) + model.y_mean.get(i);
    return r;
}

} // namespace

std::size_t native_bit(std::size_t half, std::size_t local) {
    if (local < 39) return half * 39 + local;
    if (local < 59) return 78 + half * 20 + local - 39;
    return 118 + half * 20 + local - 59;
}

void Predictor::reset() {
    count_ = 0;
    cursor_ = 0;
    state_.fill(0.f);
    previous_context_.reset();
}

const FeatureRecord& Predictor::at(int32_t index) const {
    return buffer_[static_cast<std::size_t>(std::clamp(index, 0, static_cast<int32_t>(count_) - 1))];
}

std::array<float, 17> Predictor::baseline(int32_t index) const {
    const auto& m = predictor_model();
    std::array<float, 280> input{};
    for (int32_t offset = -3; offset <= 3; ++offset) {
        const auto pos = static_cast<std::size_t>(offset + 3) * 40;
        const auto& f = at(index + offset).features;
        std::copy_n(f.begin(), 40, input.begin() + static_cast<std::ptrdiff_t>(pos));
    }
    std::array<float, 17> out{};
    affine(input, m.BASELINE_WEIGHT, m.BASELINE_BIAS.data, out.data());
    return out;
}

std::array<float, 70> Predictor::gru(const std::array<float, 81>& features) {
    const auto& m = predictor_model();
    std::array<float, 81> input{};
    std::array<float, 192> projected{};
    std::array<float, 128> gates{};
    normalize(features, m.RECURRENT_X_MEAN, m.RECURRENT_X_SCALE, input.data(), false);
    affine(input, m.RECURRENT_INPUT_WEIGHT, m.RECURRENT_GATE_BIAS.data, projected.data());
    affine_columns(state_, m.RECURRENT_RECURRENT_WEIGHT, nullptr, gates.data(), 0, 128);
    for (std::size_t j = 0; j < 128; ++j) gates[j] = 0.5f * (1.f + std::tanh(0.5f * (projected[j] + gates[j])));
    std::array<float, 64> reset_state{};
    for (std::size_t i = 0; i < 64; ++i) reset_state[i] = gates[64 + i] * state_[i];
    std::array<float, 64> next{};
    affine_columns(reset_state, m.RECURRENT_RECURRENT_WEIGHT, nullptr, next.data(), 128, 64);
    for (std::size_t j = 0; j < 64; ++j) next[j] = gates[j] * state_[j] + (1.f - gates[j]) * std::tanh(projected[128 + j] + next[j]);
    state_ = next;
    std::array<float, 70> output{};
    affine(state_, m.RECURRENT_OUTPUT_WEIGHT, m.RECURRENT_OUTPUT_BIAS.data, output.data());
    for (std::size_t i = 0; i < 70; ++i) output[i] = output[i] * m.RECURRENT_Y_SCALE.get(i) + m.RECURRENT_Y_MEAN.get(i);
    return output;
}

void Predictor::stabilize_pitch(int32_t index, Controls& values) const {
    const float octave = std::log(2.f);
    for (std::size_t block = 0; block < 4; ++block) {
        const float confidence = values[70 + block];
        if (confidence >= 0.8f) continue;
        std::array<float, 5> nearby{};
        for (std::size_t j = 0; j < 5; ++j) {
            const int32_t sub = std::clamp(index * 4 + static_cast<int32_t>(block) + static_cast<int32_t>(j) - 2, 0, static_cast<int32_t>(count_) * 4 - 1);
            nearby[j] = rs::clamp(at(sub / 4).periods[static_cast<std::size_t>(sub) % 4], 20.f, 160.f);
        }
        std::stable_sort(nearby.begin(), nearby.end(), [](float a, float b) { return rs::total_lt(a, b); });
        const float original = std::log(values[74 + block]);
        const float correction = rs::clamp(std::log(nearby[2]) - original, -octave, octave);
        values[74 + block] = std::exp(original + (1.f - confidence / 0.8f) * correction);
    }
}

void Predictor::refine(int32_t index, Controls& values) {
    const auto& m = predictor_model();
    for (std::size_t half = 0; half < 2; ++half) {
        const auto& features = at(index).features;
        const std::size_t state = features[38 + half] != 0.f ? 1 : 0;
        auto category = [&](std::size_t field) {
            std::size_t code = 0;
            for (std::size_t j = field == 0 ? 1 : 0; j < m.PROFILE_LENGTHS[field]; ++j) {
                const std::size_t local = m.PROFILE_GROUPS[field][j];
                const std::size_t bit = native_bit(half, local);
                code = 2 * code + (features[40 + bit] < 0.f ? 1 : 0);
            }
            return code;
        };
        const std::array<std::size_t, 3> rows = {state * 256 + category(0), state * 256 + 128 + category(1), state * 256 + 192 + category(2)};
        std::array<float, 36> input{};
        std::copy_n(values.begin() + static_cast<std::ptrdiff_t>(6 + half * 32), 32, input.begin());
        input[32] = values[4];
        input[33] = std::log(rs::clamp(values[5], 20.f, 160.f));
        input[34] = values[half * 2];
        input[35] = values[half * 2 + 1];
        const float gate = rs::clamp(((input[34] + input[35]) * 0.5f + 60.f) / 10.f, 0.f, 1.f);
        normalize_in_place(input.data(), m.REFINEMENT_SPECTRAL_MEAN, m.REFINEMENT_SPECTRAL_SCALE, true);
        std::array<float, 32> delta{};
        affine(input, m.REFINEMENT_SPECTRAL_WEIGHT, m.REFINEMENT_SPECTRAL_BIAS.data, delta.data());
        for (std::size_t j = 0; j < 16; ++j) {
            for (std::size_t row : rows) delta[j] += m.REFINEMENT_SPECTRAL_TABLE.get(row * 16 + j);
            delta[j] *= gate;
        }
        numeric_.bound_correction(delta.data());
        for (std::size_t j = 0; j < 32; ++j) values[6 + half * 32 + j] += delta[j];
    }
    for (std::size_t block = 0; block < 4; ++block) {
        std::array<float, 37> input{};
        input[0] = std::log(rs::clamp(values[74 + block], 20.f, 160.f));
        input[1] = std::log(rs::clamp(values[5], 20.f, 160.f));
        input[2] = values[70 + block];
        input[3] = values[block];
        input[4] = static_cast<float>(block % 2);
        for (std::size_t j = 0; j < 9; ++j) {
            const int32_t pos = std::clamp(index * 4 + static_cast<int32_t>(block) + static_cast<int32_t>(j) - 4, 0, static_cast<int32_t>(count_) * 4 - 1);
            const float period = at(pos / 4).periods[static_cast<std::size_t>(pos) % 4];
            input[5 + j] = std::log(rs::clamp(period, 20.f, 160.f)) - input[0];
            input[28 + j] = input[2] * input[5 + j];
        }
        const auto& features = at(index).features;
        std::copy_n(features.begin() + static_cast<std::ptrdiff_t>(17 * (block / 2)), 7, input.begin() + 14);
        std::copy_n(features.begin() + static_cast<std::ptrdiff_t>(17 * (1 - block / 2)), 7, input.begin() + 21);
        normalize_in_place(input.data(), m.REFINEMENT_PITCH_MEAN, m.REFINEMENT_PITCH_SCALE, true);
        std::array<float, 1> delta{};
        affine(input, m.REFINEMENT_PITCH_WEIGHT, m.REFINEMENT_PITCH_BIAS.data, delta.data());
        const float gate = rs::clamp((values[block] + 60.f) / 10.f, 0.f, 1.f);
        const float limit = std::log(2.f) * 0.5f;
        values[74 + block] = rs::clamp(std::exp(std::log(values[74 + block]) + gate * rs::clamp(delta[0], -limit, limit)), 20.f, 160.f);
    }
}

Prediction Predictor::emit() {
    const auto& m = predictor_model();
    const auto index = static_cast<int32_t>(cursor_);
    const auto base = baseline(index);
    std::array<float, 1007> context_input{};
    std::copy(base.begin(), base.end(), context_input.begin());
    for (int32_t offset = -2; offset <= 2; ++offset) {
        const std::size_t p = 17 + static_cast<std::size_t>(offset + 2) * 198;
        const auto& f = at(index + offset).features;
        std::copy(f.begin(), f.end(), context_input.begin() + static_cast<std::ptrdiff_t>(p));
    }
    const auto spectrum = mlp(m.SPECTRUM, context_input);
    const auto delta = mlp(m.CONTEXT, context_input).output;
    std::array<float, 17> current{};
    for (std::size_t j = 0; j < 17; ++j) current[j] = base[j] + m.context_blend[j] * delta[j];
    const auto before = previous_context_.value_or(current);
    previous_context_ = current;
    std::array<float, 51> level_input{};
    std::copy(base.begin(), base.end(), level_input.begin());
    std::copy(before.begin(), before.end(), level_input.begin() + 17);
    std::copy(current.begin(), current.end(), level_input.begin() + 34);
    const auto level = mlp(m.LEVELS, level_input);
    std::array<float, 81> features{};
    std::copy(level.hidden.begin(), level.hidden.end(), features.begin() + 17);
    std::copy(spectrum.hidden.begin(), spectrum.hidden.end(), features.begin() + 49);
    for (std::size_t j = 0; j < 17; ++j) features[j] = base[j] + m.level_blend[j] * level.output[j] + m.spectral_blend[j] * spectrum.output[j];
    Controls values{};
    for (std::size_t i = 0; i < 4; ++i) values[i] = features[0] + features[i + 1];
    values[4] = rs::clamp((features[5] - 0.35f) / 0.65f, 0.f, 1.f);
    values[5] = features[6];
    numeric_.lpc_cepstrum(features.data() + 7, values.data() + 6);
    std::copy_n(values.begin() + 6, 32, values.begin() + 38);
    const auto residual = gru(features);
    for (std::size_t i = 0; i < 70; ++i) values[i] += m.recurrent_blend[i] * residual[i];
    values[4] = rs::clamp(values[4], 0.f, 1.f);
    values[5] = std::exp(rs::clamp(values[5], std::log(20.f), std::log(160.f)));
    for (std::size_t i = 0; i < 4; ++i) {
        values[70 + i] = values[4];
        values[74 + i] = std::exp(m.PITCH_BLEND * std::log(rs::clamp(at(index).periods[i], 20.f, 160.f)) + (1.f - m.PITCH_BLEND) * std::log(values[5]));
    }
    for (std::size_t half = 0; half < 2; ++half) {
        std::array<float, 67> input{};
        std::copy_n(values.begin() + static_cast<std::ptrdiff_t>(6 + half * 32), 32, input.begin());
        input[32] = values[4];
        input[33] = std::log(rs::clamp(values[5], 20.f, 160.f));
        const float a = rs::clamp(values[half * 2], -120.f, -3.f) * (std::log(10.f) / 10.f);
        const float b = rs::clamp(values[half * 2 + 1], -120.f, -3.f) * (std::log(10.f) / 10.f);
        input[34] = (rs::max(a, b) + std::log(1.f + std::exp(-std::abs(a - b))) - std::log(2.f)) * (10.f / std::log(10.f));
        for (std::size_t i = 0; i < 32; ++i) input[35 + i] = values[4] * input[i];
        normalize_in_place(input.data(), m.CALIBRATION_MEAN, m.CALIBRATION_SCALE, true);
        std::array<float, 32> d{};
        affine(input, m.CALIBRATION_WEIGHT, m.CALIBRATION_BIAS.data, d.data());
        numeric_.bound_correction(d.data());
        for (std::size_t i = 0; i < 32; ++i) values[6 + half * 32 + i] += d[i];
    }
    stabilize_pitch(index, values);
    refine(index, values);
    for (float x : values)
        if (!std::isfinite(x)) throw Failure{};
    Prediction prediction;
    prediction.controls = values;
    const auto& cur = at(index).features;
    for (std::size_t i = 0; i < 158; ++i) prediction.bits[i] = cur[40 + i] < 0.f ? 1 : 0;
    ++cursor_;
    if (cursor_ > 4) {
        const std::size_t discard = cursor_ - 4;
        std::copy(buffer_.begin() + static_cast<std::ptrdiff_t>(discard), buffer_.begin() + static_cast<std::ptrdiff_t>(count_), buffer_.begin());
        count_ -= discard;
        cursor_ -= discard;
    }
    return prediction;
}

std::optional<Prediction> Predictor::decode(const PayloadBytes& payload) {
    if ((payload[19] & 0xc0) || count_ == buffer_.size()) throw Failure{};
    buffer_[count_] = encode_features(payload);
    ++count_;
    if (pending() > kLookahead) return emit();
    return std::nullopt;
}

} // namespace spear::jr_train::voice
