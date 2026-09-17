#include "spear/core/synthetic_source.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace spear {

SyntheticSource::SyntheticSource(EventBus* events, SyntheticSignal sig, std::size_t block_samples)
    : ThreadedSource("synthetic", events, block_samples), sig_(std::move(sig)), rng_(sig_.seed) {}

void SyntheticSource::set_signal(SyntheticSignal sig) {
    sig_ = std::move(sig);
    rng_.seed(sig_.seed);
}

void SyntheticSource::on_start() {
    phase_.assign(sig_.tones.size(), 0.0);
    fm_phase_ = fm_mod_phase_ = am_phase_ = am_mod_phase_ = 0;
    produced_ = 0;
    rng_.seed(sig_.seed);
}

uint32_t SyntheticSource::fill(BlockBuilder& b) {
    if (sig_.max_samples && produced_ >= sig_.max_samples) return 0;
    auto out = b.data<sc16>();
    uint32_t n = static_cast<uint32_t>(out.size());
    if (sig_.max_samples) n = static_cast<uint32_t>(std::min<uint64_t>(n, sig_.max_samples - produced_));

    const double rate = config().sample_rate;
    std::normal_distribution<double> noise(0.0, sig_.noise_amplitude);
    constexpr double two_pi = 2.0 * std::numbers::pi;
    for (uint32_t i = 0; i < n; ++i) {
        double re = 0, im = 0;
        for (std::size_t k = 0; k < sig_.tones.size(); ++k) {
            re += sig_.tones[k].amplitude * std::cos(phase_[k]);
            im += sig_.tones[k].amplitude * std::sin(phase_[k]);
            phase_[k] += two_pi * sig_.tones[k].offset_hz / rate;
            if (phase_[k] > two_pi) phase_[k] -= two_pi;
            else if (phase_[k] < -two_pi) phase_[k] += two_pi;
        }
        if (sig_.fm.enabled) {
            // FM: 瞬時周波数 = offset + deviation * sin(mod)
            const double inst = sig_.fm.offset_hz + sig_.fm.deviation_hz * std::sin(fm_mod_phase_);
            fm_phase_ += two_pi * inst / rate;
            if (fm_phase_ > two_pi) fm_phase_ -= two_pi;
            fm_mod_phase_ += two_pi * sig_.fm.tone_hz / rate;
            if (fm_mod_phase_ > two_pi) fm_mod_phase_ -= two_pi;
            re += sig_.fm.amplitude * std::cos(fm_phase_);
            im += sig_.fm.amplitude * std::sin(fm_phase_);
        }
        if (sig_.am.enabled) {
            const double env = 1.0 + sig_.am.depth * std::sin(am_mod_phase_);
            am_phase_ += two_pi * sig_.am.offset_hz / rate;
            if (am_phase_ > two_pi) am_phase_ -= two_pi;
            am_mod_phase_ += two_pi * sig_.am.tone_hz / rate;
            if (am_mod_phase_ > two_pi) am_mod_phase_ -= two_pi;
            re += sig_.am.amplitude * env * std::cos(am_phase_);
            im += sig_.am.amplitude * env * std::sin(am_phase_);
        }
        if (sig_.noise_amplitude > 0) { re += noise(rng_); im += noise(rng_); }
        // ADC 相当の量子化 (12-bit ADC を 16-bit に左詰めした B210 と同じ full scale)
        auto q = [](double v) {
            v = std::clamp(v, -1.0, 1.0);
            return static_cast<int16_t>(std::lrint(v * 32767.0));
        };
        out[i] = sc16(q(re), q(im));
    }
    produced_ += n;
    return n;
}

} // namespace spear
