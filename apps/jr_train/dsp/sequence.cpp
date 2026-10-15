// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 原本: crates/jr-conventional-dsp/src/{sequence,signal,equalizer}.rs
#include "detail.hpp"

#include <algorithm>
#include <limits>
#include <numbers>

namespace spear::jr_train::dsp::detail {
namespace {

constexpr float kLevel = std::numbers::sqrt2_v<float> / 2.f;   // Rust FRAC_1_SQRT_2(f32 で同じ値)
constexpr std::array<C32, 4> kSymbols = {C32{kLevel, kLevel}, C32{-kLevel, kLevel}, C32{-kLevel, -kLevel}, C32{kLevel, -kLevel}};
constexpr std::array<uint8_t, 4> kGray = {0, 1, 3, 2};

static_assert(kLevel == 0.70710677f);

} // namespace

Sequence::Sequence() {
    std::array<C32, 4> channel{};
    channel[0].re = 1.f;
    channels.fill(channel);
}

Sequence::Result Sequence::process(C32 input, bool reliable) {
    std::array<float, kStates> next_costs;
    next_costs.fill(std::numeric_limits<float>::infinity());
    std::array<uint8_t, kStates> parents{};
    std::array<C32, kStates> predictions{};
    for (std::size_t prior = 0; prior < kStates; ++prior) {
        for (std::size_t current = 0; current < 4; ++current) {
            const std::array<C32, 4> symbols = {kSymbols[current], kSymbols[prior % 4], kSymbols[(prior / 4) % 4], kSymbols[prior / 16]};
            C32 prediction{};
            for (std::size_t k = 0; k < 4; ++k) {
                const C32 h = channels[prior][k], s = symbols[k];
                prediction.re += h.re * s.re - h.im * s.im;
                prediction.im += h.re * s.im + h.im * s.re;
            }
            // 観測が無い間は証拠を足さずに trellis だけ進める(増幅された雑音で伝送路を学習しない)
            float error = 0.f;
            if (reliable) {
                const float dr = input.re - prediction.re, di = input.im - prediction.im;
                error = std::fmin(dr * dr + di * di, 16.f);
            }
            const float cost = costs[prior] + error;
            const std::size_t state = current + 4 * (prior % 16);
            if (cost < next_costs[state]) {
                next_costs[state] = cost;
                parents[state] = static_cast<uint8_t>(prior);
                predictions[state] = prediction;
            }
        }
    }
    // 同値なら最初の最小(原本 total_cmp)
    std::size_t best = 0;
    for (std::size_t state = 1; state < kStates; ++state)
        if (total_lt(next_costs[state], next_costs[best])) best = state;
    const C32 bp = predictions[best];
    const C32 error{input.re - bp.re, input.im - bp.im};
    const float phase = std::atan2(input.im * bp.re - input.re * bp.im, input.re * bp.re + input.im * bp.im);

    // 診断: 現在の最良生き残りの遅延波を観測から引き、主タップで割る。適応の前に行う(今の誤差を学習で消さない)
    {
        const std::size_t prior = parents[best];
        const auto& channel = channels[prior];
        const C32 main = channel[0];
        const float power = main.re * main.re + main.im * main.im;
        equalized_.reset();
        if (reliable && std::isfinite(power) && power > 1e-6f) {
            C32 value = input;
            const std::array<C32, 3> past = {kSymbols[prior % 4], kSymbols[(prior / 4) % 4], kSymbols[prior / 16]};
            for (std::size_t k = 0; k < 3; ++k) {
                const C32 h = channel[k + 1], s = past[k];
                value.re -= h.re * s.re - h.im * s.im;
                value.im -= h.re * s.im + h.im * s.re;
            }
            const C32 out{(value.re * main.re + value.im * main.im) / power, (value.im * main.re - value.re * main.im) / power};
            if (std::isfinite(out.re) && std::isfinite(out.im)) equalized_ = out;
        }
    }

    auto updated = channels;
    for (std::size_t state = 0; state < kStates; ++state) {
        const std::size_t prior = parents[state];
        const std::array<C32, 4> symbols = {kSymbols[state % 4], kSymbols[prior % 4], kSymbols[(prior / 4) % 4], kSymbols[prior / 16]};
        const C32 e{input.re - predictions[state].re, input.im - predictions[state].im};
        updated[state] = channels[prior];
        if (reliable) {
            for (std::size_t k = 0; k < 4; ++k) {
                C32& h = updated[state][k];
                const C32 s = symbols[k];
                h.re += kChannelStep * (e.re * s.re + e.im * s.im);
                h.im += kChannelStep * (e.im * s.re - e.re * s.im);
            }
            float norm = 0.f;
            for (const C32& h : updated[state]) norm += h.re * h.re + h.im * h.im;
            if (norm > 16.f) {
                const float gain = 4.f / std::sqrt(norm);
                for (C32& h : updated[state]) {
                    h.re *= gain;
                    h.im *= gain;
                }
            }
        }
    }
    channels = updated;
    // 共通オフセットを引いて、無限長の入力でも累積コストを有界に保つ
    const float base = next_costs[best];
    for (std::size_t state = 0; state < kStates; ++state) costs[state] = next_costs[state] - base;
    history_[write_] = parents;
    write_ = (write_ + 1) % kDepth;
    filled_ = std::min(filled_ + 1, kDepth);

    Result r;
    if (filled_ == kDepth) {
        std::size_t state = best;
        for (std::size_t back = 0; back < kDepth - 1; ++back) state = history_[(write_ + kDepth - 1 - back) % kDepth][state];
        const std::size_t current = state % 4;
        r.dibit = previous_ ? kGray[(current + 4 - *previous_) % 4] : uint8_t{0};
        previous_ = current;
    }
    r.phase_error = reliable ? phase : 0.f;
    r.error_power = reliable ? error.re * error.re + error.im * error.im : 1.f;
    return r;
}

// ---- SignalView ----

void SignalView::push(std::optional<C32> symbol) {
    // π/4 交番を除いた観測に、差動座標でその増分を戻す(4 つの対角の基準点が π/4-DQPSK の意味を保つ)。
    // 振幅・位相のばらつきは残す(判定も点ごとの正規化もしない)。
    C32 value{};
    if (previous_ && symbol) {
        const C32 p = *previous_, s = *symbol;
        const float real = s.re * p.re + s.im * p.im;
        const float imaginary = s.im * p.re - s.re * p.im;
        const float rotation = kLevel;
        value = C32{(real - imaginary) * rotation, (real + imaginary) * rotation};
    }
    // 観測が無ければ表示を古くし、差動の対を切る(フェードをまたいで無関係なシンボルを結ばない)
    previous_ = symbol;
    ring_[head_] = value;
    head_ = (head_ + 1) % kCapacity;
    count_ = std::min(count_ + 1, kCapacity);
}

std::size_t SignalView::read(C32* out, std::size_t capacity, double& evm) const {
    evm = 0.0;
    const std::size_t count = std::min(count_, capacity);
    if (count == 0) return 0;
    const std::size_t start = (head_ + kCapacity - count) % kCapacity;
    double power = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const C32 z = ring_[(start + i) % kCapacity];
        const double re = z.re, im = z.im;
        power += re * re + im * im;
    }
    power /= static_cast<double>(count);
    if (!std::isfinite(power) || power < 1e-12) return 0;
    const double amplitude = std::sqrt(power);
    double squared_error = 0.0;
    constexpr double ideal = std::numbers::sqrt2 / 2.0;
    static_assert(ideal == 0.7071067811865476);
    for (std::size_t i = 0; i < count; ++i) {
        const C32 z = ring_[(start + i) % kCapacity];
        const double re = static_cast<double>(z.re) / amplitude;
        const double im = static_cast<double>(z.im) / amplitude;
        const double dr = std::abs(re) - ideal, di = std::abs(im) - ideal;
        squared_error += dr * dr + di * di;
        out[i] = C32{static_cast<float>(re), static_cast<float>(im)};
    }
    evm = 100.0 * std::sqrt(squared_error / static_cast<double>(count));
    return count;
}

// ---- Equalizer ----

std::pair<C32, float> Equalizer::process(const std::array<C32, kTaps>& input, const std::array<C32, kTaps>& derivatives, bool reliable) {
    constexpr float kAdaptation = 0.f;   // 原本: CMA 適応はフェードの谷で回復を悪くしたので止めてある
    C32 value{}, derivative{};
    float energy = 0.f;
    for (std::size_t k = 0; k < kTaps; ++k) {
        const C32 w = weights_[k], x = input[k], dx = derivatives[k];
        value.re += w.re * x.re - w.im * x.im;
        value.im += w.re * x.im + w.im * x.re;
        derivative.re += w.re * dx.re - w.im * dx.im;
        derivative.im += w.re * dx.im + w.im * dx.re;
        energy += x.re * x.re + x.im * x.im;
    }
    const float power = value.re * value.re + value.im * value.im;
    if (reliable && energy > 0.01f && std::isfinite(energy) && std::isfinite(power)) {
        const float step = kAdaptation * std::clamp(1.f - power, -2.f, 1.f) / energy;
        for (std::size_t k = 0; k < kTaps; ++k) {
            C32& w = weights_[k];
            const C32 x = input[k];
            w.re += step * (value.re * x.re + value.im * x.im);
            w.im += step * (value.im * x.re - value.re * x.im);
        }
        float norm = 0.f;
        for (const C32& w : weights_) norm += w.re * w.re + w.im * w.im;
        if (norm > 16.f) {
            const float gain = 4.f / std::sqrt(norm);
            for (C32& w : weights_) {
                w.re *= gain;
                w.im *= gain;
            }
        }
    }
    // 選んだ波形とその微分からタイミング誤差を出す(ISI は後段の系列推定が扱う)。フェード中は惰性で進む
    float timing = 0.f;
    if (reliable) {
        const float t = 0.5f * (value.re * derivative.re + value.im * derivative.im);
        timing = t < -1.f ? -1.f : (t > 1.f ? 1.f : t);
    }
    return {value, timing};
}

} // namespace spear::jr_train::dsp::detail
