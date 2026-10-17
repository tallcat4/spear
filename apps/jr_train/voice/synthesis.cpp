// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 原本: src/synthesis.rs、src/synthesis_parameters.rs、src/source_features.rs
#include "synthesis.hpp"

#include "common.hpp"
#include "models.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace spear::jr_train::voice {
namespace {

// ================= synthesis_parameters.rs =================
void bound(std::array<float, 32>& cep, double response_db) {
    Bins symmetric{};
    for (std::size_t i = 0; i < 32; ++i) {
        symmetric[i + 1][0] = static_cast<double>(cep[i]);
        symmetric[511 - i][0] = static_cast<double>(cep[i]);
    }
    fft(symmetric);
    double peak = 0.0;
    for (std::size_t i = 0; i < 257; ++i) peak = rs::max(peak, std::abs(symmetric[i][0]));
    const double scale = rs::min(response_db * std::numbers::ln10 / 10.0 / rs::max(peak, 1e-20), 1.0);
    for (auto& x : cep) x = static_cast<float>(static_cast<double>(x) * scale);
}

Controls correct(const Controls& controls, const std::array<std::array<float, 34>, 2>& raw, float strength) {
    Controls out = controls;
    float maximum = -std::numeric_limits<float>::infinity();
    for (std::size_t i = 0; i < 4; ++i) maximum = rs::max(maximum, controls[i]);
    const float headroom = rs::clamp((-3.f - maximum) / 12.f, 0.f, 1.f);
    for (std::size_t h = 0; h < 2; ++h) {
        const std::array<float, 2> quiet = {rs::clamp((controls[2 * h] + 75.f) / 15.f, 0.f, 1.f), rs::clamp((controls[2 * h + 1] + 75.f) / 15.f, 0.f, 1.f)};
        for (std::size_t b = 0; b < 2; ++b) {
            const float delta = rs::clamp(raw[h][b] * strength, -12.f, 12.f) * quiet[b];
            out[2 * h + b] += rs::min(delta, 0.f) + rs::max(delta, 0.f) * headroom;
        }
        const float mean = (quiet[0] + quiet[1]) / 2.f;
        std::array<float, 32> cep{};
        for (std::size_t i = 0; i < 32; ++i) cep[i] = rs::clamp(raw[h][i + 2] * strength, -0.75f, 0.75f) * mean;
        bound(cep, 12.0);
        for (std::size_t i = 0; i < 32; ++i) out[6 + 32 * h + i] += cep[i];
    }
    return out;
}

std::pair<Controls, std::array<double, 4>> temporal(const Controls& controls, const std::array<float, 4>& delta, const std::array<std::array<float, 34>, 2>& raw) {
    Controls out = controls;
    std::array<double, 4> total{};
    for (std::size_t h = 0; h < 2; ++h) {
        std::array<float, 32> cep{};
        for (std::size_t i = 0; i < 32; ++i) cep[i] = 1.5f * std::tanh(raw[h][i + 2]) * 0.5f;
        bound(cep, 24.0);
        for (std::size_t i = 0; i < 32; ++i) out[6 + 32 * h + i] += cep[i];
        for (std::size_t b = 0; b < 2; ++b) {
            const std::size_t k = h * 2 + b;
            const auto d = static_cast<double>(delta[k] + 12.f * std::tanh(raw[h][b]) * 0.5f);
            const double old = rs::clamp(static_cast<double>(controls[k]), -120.0, -3.0);
            total[k] = rs::clamp(old + rs::clamp(d, -12.0, 12.0), -120.0, -3.0) - old;
        }
    }
    return {out, total};
}

std::array<float, 320> gain_curve(const std::array<double, 4>& current, std::optional<double> previous, std::optional<double> following) {
    std::array<float, 320> out{};
    for (std::size_t i = 0; i < 320; ++i) {
        const std::size_t b = i / 80, sample = i % 80;
        double a, z, position;
        if (sample < 40) {
            a = b == 0 ? previous.value_or(current[0]) : current[b - 1];
            z = current[b];
            position = (static_cast<double>(sample) + 40.5) / 80.0;
        } else {
            a = current[b];
            z = b == 3 ? following.value_or(current[3]) : current[b + 1];
            position = (static_cast<double>(sample) - 39.5) / 80.0;
        }
        const double u = 0.5 - 0.5 * std::cos(std::numbers::pi * position);
        out[i] = static_cast<float>(std::pow(10.0, (a + (z - a) * u) / 20.0));
    }
    return out;
}

std::array<std::array<float, 24>, 2> gain_features(const std::array<std::array<float, 12>, 4>& trace) {
    std::array<std::array<float, 24>, 2> out{};
    for (std::size_t b = 0; b < 4; ++b) {
        const std::size_t at = (b % 2) * 12;
        const auto& x = trace[b];
        auto& o = out[b / 2];
        std::copy(x.begin() + 1, x.begin() + 7, o.begin() + static_cast<std::ptrdiff_t>(at));
        o[at + 6] = static_cast<float>(10.0 * std::log10(rs::max(static_cast<double>(x[7]), 1.0)));
        o[at + 7] = static_cast<float>(20.0 * std::log10(rs::max(static_cast<double>(x[8]), 0.5)));
        o[at + 8] = x[9] / 32768.f;
        o[at + 9] = x[10];
        o[at + 10] = x[11];
        o[at + 11] = x[8] == 0.f ? 1.f : 0.f;
    }
    return out;
}

std::array<std::array<float, 621>, 2> source_features(const std::array<std::array<float, 570>, 2>& base, const Controls& controls, const PcmFrame& pcm,
                                                     const std::array<int16_t, 320>& raw, const std::array<double, 320>& source) {
    std::array<double, 320> pcm64{};
    for (std::size_t i = 0; i < 320; ++i) pcm64[i] = static_cast<double>(pcm[i]) / 32768.0;
    const auto bands = describe(pcm64);
    const auto source_bands = describe(source);
    std::array<std::array<float, 621>, 2> out{};
    for (std::size_t h = 0; h < 2; ++h) {
        std::copy(base[h].begin(), base[h].end(), out[h].begin());
        for (std::size_t i = 0; i < 20; ++i) {
            out[h][570 + i] = static_cast<float>(bands[h][i]);
            out[h][592 + i] = static_cast<float>(source_bands[h][i]);
        }
        for (std::size_t b = 0; b < 2; ++b) {
            out[h][590 + b] = static_cast<float>(bands[h][20 + b] - static_cast<double>(controls[2 * h + b]));
            const std::size_t at = h * 160 + b * 80;
            double energy = -0.0;
            for (std::size_t i = 0; i < 80; ++i) {
                const double v = static_cast<double>(raw[at + i]);
                energy += v * v;
            }
            energy /= 80.0;
            out[h][619 + b] = static_cast<float>(10.0 * std::log10(rs::max(energy, 1.0)));
        }
        const double* x = source.data() + h * 160;
        const std::size_t lags[7] = {1, 2, 4, 8, 16, 32, 64};
        for (std::size_t k = 0; k < 7; ++k) {
            const std::size_t lag = lags[k];
            double dot = 0.0, a = 0.0, b = 0.0;
            for (std::size_t i = 0; i < 160 - lag; ++i) {
                dot += x[i] * x[i + lag];
                a += x[i] * x[i];
                b += x[i + lag] * x[i + lag];
            }
            out[h][612 + k] = static_cast<float>(dot / std::sqrt(rs::max(a * b, 1e-20)));
        }
    }
    return out;
}

// ================= synthesis.rs =================
void fade(PcmFrame& pcm, const SynthInput& input) {
    const struct {
        std::size_t begin;
        bool enabled, inverse;
    } parts[2] = {{0, input.fade_in, false}, {240, input.fade_out, true}};
    for (const auto& p : parts) {
        if (!p.enabled) continue;
        for (std::size_t n = 0; n < 80; ++n) {
            const float gain = 0.5f - 0.5f * std::cos(kPi * static_cast<float>(n) / 79.f);
            pcm[p.begin + n] = rs::as<int16_t>(round_even(static_cast<float>(pcm[p.begin + n]) * (p.inverse ? 1.f - gain : gain)));
        }
    }
}

PlanInput planned(const SynthInput& input, const std::array<float, 320>& source) {
    PlanInput p;
    p.key = input.key;
    p.payload = input.payload;
    p.controls = input.controls;
    p.source = source;
    p.weight.fill(1.f);
    p.offset.fill(0.f);
    p.fade_in = input.fade_in;
    p.fade_out = input.fade_out;
    return p;
}

PcmFrame synth(Vocoder& vocoder, const SynthInput& input, const std::array<float, 320>& source, const std::array<float, 320>* curve, bool restore) {
    auto pcm = vocoder.process_explicit(input.controls, 4, &source, curve, restore);
    fade(pcm, input);
    return pcm;
}

template <std::size_t N>
std::array<float, N> head(const std::vector<float>& v) {
    if (v.size() != N) throw Failure{};
    std::array<float, N> out{};
    std::copy(v.begin(), v.end(), out.begin());
    return out;
}

} // namespace

// ================= source_features.rs =================
std::array<std::array<float, 570>, 2> SourceFeatures::process(const PayloadBytes& payload, const Controls& c) {
    const auto physical = physical_.process(payload, c, age_ == 0);
    std::array<uint8_t, 158> bits{};
    for (std::size_t i = 0; i < 158; ++i) bits[i] = (payload[i / 8] >> (i % 8)) & 1;
    std::array<std::array<float, 570>, 2> out{};
    for (std::size_t h = 0; h < 2; ++h) {
        std::array<float, 190> x{};
        std::copy_n(c.begin() + static_cast<std::ptrdiff_t>(6 + 32 * h), 32, x.begin());
        std::copy_n(c.begin() + static_cast<std::ptrdiff_t>(2 * h), 2, x.begin() + 32);
        std::copy_n(c.begin() + static_cast<std::ptrdiff_t>(70 + 2 * h), 2, x.begin() + 34);
        x[36] = std::log(rs::clamp(c[74 + 2 * h], 20.f, 160.f));
        x[37] = std::log(rs::clamp(c[75 + 2 * h], 20.f, 160.f));
        x[38] = c[4];
        x[39] = std::log(rs::clamp(c[5], 20.f, 160.f));
        x[40] = static_cast<float>(h);
        x[41] = static_cast<float>(std::min<std::size_t>(age_, 20)) / 20.f;
        x[42] = age_ == 0 ? 1.f : 0.f;
        std::array<uint8_t, 79> group{};
        std::copy_n(bits.begin() + static_cast<std::ptrdiff_t>(h * 39), 39, group.begin());
        std::copy_n(bits.begin() + static_cast<std::ptrdiff_t>(78 + 20 * h), 20, group.begin() + 39);
        std::copy_n(bits.begin() + static_cast<std::ptrdiff_t>(118 + 20 * h), 20, group.begin() + 59);
        for (std::size_t j = 0; j < 79; ++j) x[43 + j] = 1.f - 2.f * static_cast<float>(group[j]);
        std::copy_n(physical.begin() + static_cast<std::ptrdiff_t>(84 + 68 * h), 68, x.begin() + 122);
        if (age_ == 0) previous_.fill(x);
        std::copy(previous_[0].begin(), previous_[0].end(), out[h].begin());
        std::copy(previous_[1].begin(), previous_[1].end(), out[h].begin() + 190);
        std::copy(x.begin(), x.end(), out[h].begin() + 380);
        previous_[0] = previous_[1];
        previous_[1] = x;
        if (age_ != std::numeric_limits<std::size_t>::max()) ++age_;
    }
    return out;
}

// ================= synthesis.rs =================
std::optional<SynthOutput> Pipeline::read() {
    if (output_.empty()) return std::nullopt;
    SynthOutput out = output_.front();
    output_.pop_front();
    ++returned_;
    return out;
}

std::optional<SynthOutput> Pipeline::push(const SynthInput& input) {
    if (finished_ || !output_.empty() || (last_ && (*last_ == std::numeric_limits<uint64_t>::max() || *last_ + 1 != input.key))) throw Failure{};
    const auto& models = synthesis_models();
    const auto observed = native_.decode(input.payload);
    const auto base = features_.process(input.payload, input.controls);
    const auto components = sources_.components(observed.excitation, input.controls);
    const auto pcm = synth(base_synthesis_, input, components.native, nullptr, true);
    const auto x = source_features(base, input.controls, pcm, observed.excitation, components.white64);
    const auto extra = geometry(components);
    std::array<std::array<float, 633>, 2> features{};
    for (std::size_t h = 0; h < 2; ++h) {
        std::copy(x[h].begin(), x[h].end(), features[h].begin());
        std::copy(extra[h].begin(), extra[h].end(), features[h].begin() + 621);
    }
    std::array<std::array<float, 40>, 2> raw{};
    for (std::size_t h = 0; h < 2; ++h) raw[h] = head<40>(models.source.forward(features[h]));
    const auto source = sources_.mix(input.controls, raw, components);
    std::array<std::array<float, 34>, 2> spectral{};
    for (std::size_t h = 0; h < 2; ++h) std::copy_n(raw[h].begin(), 34, spectral[h].begin());
    const Controls changed = correct(input.controls, spectral, 0.5f);
    SynthInput candidate_input = input;
    candidate_input.controls = changed;
    const auto candidate = synth(candidate_synthesis_, candidate_input, source, nullptr, true);
    SourceFrame frame;
    frame.input = input;
    frame.changed = changed;
    frame.source = source;
    frame.strength = periodicity(candidate);
    frame.features = features;
    frame.native_descriptor = pcm_descriptor(observed.pcm);
    frame.gain_features = gain_features(observed.gain);
    frame.age = received_;
    source_queue_.push_back(std::move(frame));
    ++received_;
    last_ = input.key;
    if (auto out = original_.push(planned(input, std::array<float, 320>{}))) choose(*out);
    if (source_queue_.size() > 1 || selected_queue_.size() > 1 || temporal_.size() > 3 || output_.size() > 1) throw Failure{};
    return read();
}

void Pipeline::choose(const PlanOutput& original) {
    if (source_queue_.empty()) throw Failure{};
    SourceFrame frame = std::move(source_queue_.front());
    source_queue_.pop_front();
    if (original.index != frame.input.key) throw Failure{};
    const double strength = periodicity(original.pcm);
    const bool fallback = frame.strength < 0.5 && strength >= 0.65 && strength - frame.strength >= 0.15;
    SynthInput input = frame.input;
    std::array<float, 320> source{};
    if (!fallback) {
        input.controls = frame.changed;
        source = frame.source;
    }
    SelectedFrame sel;
    sel.input = input;
    sel.features = frame.features;
    sel.native_descriptor = frame.native_descriptor;
    sel.gain_features = frame.gain_features;
    sel.age = frame.age;
    selected_queue_.push_back(std::move(sel));
    if (auto out = selected_.push(planned(input, source))) resynthesize(*out);
}

void Pipeline::resynthesize(const PlanOutput& selected) {
    const auto& models = synthesis_models();
    if (selected_queue_.empty()) throw Failure{};
    SelectedFrame frame = std::move(selected_queue_.front());
    selected_queue_.pop_front();
    if (frame.input.key != selected.index) throw Failure{};
    const auto descriptor = pcm_descriptor(selected.pcm);
    std::array<std::array<float, 679>, 2> x{};
    for (std::size_t h = 0; h < 2; ++h) {
        std::copy(frame.features[h].begin(), frame.features[h].end(), x[h].begin());
        std::copy(descriptor[h].begin(), descriptor[h].end(), x[h].begin() + 633);
        std::copy(frame.native_descriptor[h].begin(), frame.native_descriptor[h].end(), x[h].begin() + 655);
        x[h][677] = std::log1p(static_cast<float>(frame.age));
        x[h][678] = frame.age == 0 ? 1.f : 0.f;
    }
    std::array<std::array<float, 34>, 2> raw{};
    for (std::size_t h = 0; h < 2; ++h) raw[h] = head<34>(models.spectrum.forward(x[h]));
    Controls controls = correct(frame.input.controls, raw, 1.f);
    std::copy_n(frame.input.controls.begin(), 4, controls.begin());
    SynthInput trial_input = frame.input;
    trial_input.controls = controls;
    const auto trial = synth(guard_trial_, trial_input, selected.source, nullptr, true);
    if (periodicity(trial) < 0.5 && periodicity(selected.pcm) >= 0.5) controls = frame.input.controls;
    SynthInput input = frame.input;
    input.controls = controls;
    const auto baseline = synth(gain_baseline_, input, selected.source, nullptr, false);
    std::array<std::array<float, 707>, 2> features{};
    std::array<float, 4> delta{};
    for (std::size_t h = 0; h < 2; ++h) {
        std::copy(x[h].begin(), x[h].end(), features[h].begin());
        for (std::size_t b = 0; b < 2; ++b) {
            const std::size_t at = (h * 2 + b) * 80;
            double energy = -0.0;
            for (std::size_t i = 0; i < 80; ++i) {
                const double v = static_cast<double>(baseline[at + i]) / 32768.0;
                energy += v * v;
            }
            energy /= 80.0;
            features[h][679 + b] = static_cast<float>(10.0 * std::log10(rs::max(energy, 1e-12)) - static_cast<double>(controls[h * 2 + b]));
            features[h][681 + b] = controls[h * 2 + b];
        }
        std::copy(frame.gain_features[h].begin(), frame.gain_features[h].end(), features[h].begin() + 683);
        const auto gain = models.gain.predict(features[h].data() + 570);
        for (std::size_t b = 0; b < 2; ++b) delta[h * 2 + b] = gain[b] * 0.5f;
    }
    TemporalFrame t;
    t.input = input;
    t.source = selected.source;
    t.delta = delta;
    for (std::size_t h = 0; h < 2; ++h) t.embedding[h] = models.temporal.temporal_embedding(features[h]);
    temporal_.push_back(std::move(t));
    ++temporal_count_;
    advance_temporal(false);
}

void Pipeline::advance_temporal(bool end) {
    const auto& models = synthesis_models();
    while (temporal_next_ < temporal_count_ && (end || temporal_next_ + 1 < temporal_count_)) {
        const std::size_t at = temporal_next_;
        const TemporalFrame frame = temporal_[at - temporal_begin_];
        std::array<std::array<float, 34>, 2> raw{};
        for (std::size_t h = 0; h < 2; ++h) {
            std::array<std::array<float, 128>, 7> context{};
            for (std::size_t j = 0; j < 7; ++j) {
                const auto half = static_cast<std::size_t>(std::clamp<int64_t>(2 * static_cast<int64_t>(at) + static_cast<int64_t>(h) + static_cast<int64_t>(j) - 4, 0,
                                                                                2 * static_cast<int64_t>(temporal_count_) - 1));
                context[j] = temporal_[half / 2 - temporal_begin_].embedding[half % 2];
            }
            raw[h] = models.temporal.temporal_finish(context);
        }
        const auto [controls, gain] = temporal(frame.input.controls, frame.delta, raw);
        GainFrame current;
        current.input = frame.input;
        current.input.controls = controls;
        current.source = frame.source;
        current.gain = gain;
        if (pending_gain_) {
            const GainFrame previous = *pending_gain_;
            pending_gain_.reset();
            emit(previous, current.gain[0]);
        }
        pending_gain_ = current;
        ++temporal_next_;
        while (temporal_begin_ < (temporal_next_ >= 2 ? temporal_next_ - 2 : 0)) {
            temporal_.pop_front();
            ++temporal_begin_;
        }
    }
}

void Pipeline::emit(const GainFrame& frame, std::optional<double> following) {
    const auto curve = gain_curve(frame.gain, previous_gain_, following);
    const auto pcm = synth(final_synthesis_, frame.input, frame.source, &curve, false);
    previous_gain_ = frame.gain[3];
    output_.push_back(SynthOutput{frame.input.key, pcm});
}

std::optional<SynthOutput> Pipeline::finish() {
    if (!finished_) {
        if (auto frame = original_.finish()) choose(*frame);
        if (auto frame = selected_.finish()) resynthesize(*frame);
        advance_temporal(true);
        if (pending_gain_) {
            const GainFrame frame = *pending_gain_;
            pending_gain_.reset();
            emit(frame, std::nullopt);
        }
        temporal_.clear();
        finished_ = true;
        if (!source_queue_.empty() || !selected_queue_.empty() || output_.size() > 4) throw Failure{};
    }
    return read();
}

} // namespace spear::jr_train::voice
