// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 原本: crates/jr-conventional-dsp/src/{dsp,receiver}.rs。式の順序は原本のまま(浮動小数の結果を一致させるため)。
#include "jr_dsp.hpp"

#include "detail.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace spear::jr_train::dsp {
namespace {

using detail::C32;
using detail::kTaps;
constexpr double kPi = std::numbers::pi;

// 窓関数法(Hamming)の低域。原本 low_pass
std::vector<float> low_pass(std::size_t count, double cutoff, double rate) {
    std::vector<float> taps(count);
    double sum = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double x = static_cast<double>(i) - static_cast<double>(count - 1) / 2.0;
        const double omega = 2.0 * kPi * cutoff / rate;
        const double sinc = x == 0.0 ? omega : std::sin(omega * x) / x;
        const double window = 0.54 - 0.46 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(count - 1));
        taps[i] = static_cast<float>(sinc * window);
        sum += static_cast<double>(taps[i]);
    }
    for (float& tap : taps) tap /= static_cast<float>(sum);
    return taps;
}

// α=0.2 の RRC。原本 root_raised_cosine(偶数長は +1)
std::vector<float> root_raised_cosine(std::size_t count, double sps, double gain) {
    constexpr double A = 0.2;
    if ((count & 1) == 0) ++count;
    std::vector<float> taps(count);
    double sum = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double t = (static_cast<double>(i) - static_cast<double>(count - 1) / 2.0) / sps;
        double value;
        if (t == 0.0) {
            value = 1.0 - A + 4.0 * A / kPi;
        } else if (std::abs(std::abs(4.0 * A * t) - 1.0) < 1e-7) {
            value = A / std::sqrt(2.0) * ((1.0 + 2.0 / kPi) * std::sin(kPi / (4.0 * A)) + (1.0 - 2.0 / kPi) * std::cos(kPi / (4.0 * A)));
        } else {
            value = (std::sin(kPi * t * (1.0 - A)) + 4.0 * A * t * std::cos(kPi * t * (1.0 + A))) / (kPi * t * (1.0 - 16.0 * A * A * t * t));
        }
        taps[i] = static_cast<float>(value);
        sum += static_cast<double>(taps[i]);
    }
    for (float& tap : taps) tap *= static_cast<float>(gain / sum);
    return taps;
}

// polyphase の各相(時間順に並べ替え)。原本 arms
std::vector<std::vector<float>> arms(const std::vector<float>& taps, std::size_t phases) {
    const std::size_t count = (taps.size() + phases - 1) / phases;
    std::vector<std::vector<float>> result(phases, std::vector<float>(count, 0.f));
    for (std::size_t phase = 0; phase < phases; ++phase)
        for (std::size_t j = 0; j < count; ++j)
            if (phase + j * phases < taps.size()) result[phase][count - 1 - j] = taps[phase + j * phases];
    return result;
}

int64_t rem_euclid(int64_t a, int64_t b) {
    const int64_t r = a % b;
    return r < 0 ? r + b : r;
}

// ---- 入力 rate → 19.2 kS/s(原本 Resampler)----
// rate が 256 kHz の 2 倍以上なら 3 段 CIC(整数、wrapping)で先に間引く。
class Resampler {
public:
    explicit Resampler(double input_rate) {
        decimation_ = std::max<std::size_t>(static_cast<std::size_t>(std::round(input_rate / 256000.0)), 1);
        const double d = static_cast<double>(decimation_);
        const double rate = input_rate / d;
        const std::size_t count = std::max<std::size_t>(static_cast<std::size_t>(std::round(0.0015 * rate)), 8);
        const std::size_t half = count / 2;
        auto prototype = low_pass(32 * count + 1, std::min(4200.0, 0.45 * std::min(rate, 19200.0)), 32.0 * rate);
        prototype.pop_back();
        for (float& tap : prototype) tap *= 32.f;
        cic_scale_ = 1.0 / (32768.0 * d * d * d);
        arms_ = arms(prototype, 32);
        ring_.assign(count + 16, C32{});
        half_ = static_cast<int64_t>(half);
        step_ = rate / 19200.0;
        next_output_ = static_cast<double>(half);
    }

    template <class Sink>
    void process(C32 value, Sink&& sink) {
        if (decimation_ > 1) {
            auto real = static_cast<uint64_t>(static_cast<int64_t>(std::round(static_cast<double>(value.re) * 32768.0)));
            auto imaginary = static_cast<uint64_t>(static_cast<int64_t>(std::round(static_cast<double>(value.im) * 32768.0)));
            for (int i = 0; i < 3; ++i) {
                integrators_[i] += real;
                integrators_[i + 3] += imaginary;
                real = integrators_[i];
                imaginary = integrators_[i + 3];
            }
            if (++counter_ < decimation_) return;
            counter_ = 0;
            for (int i = 0; i < 3; ++i) {
                const uint64_t previous_real = real, previous_imaginary = imaginary;
                real -= delays_[i];
                imaginary -= delays_[i + 3];
                delays_[i] = previous_real;
                delays_[i + 3] = previous_imaginary;
            }
            value = C32{static_cast<float>(static_cast<double>(static_cast<int64_t>(real)) * cic_scale_),
                        static_cast<float>(static_cast<double>(static_cast<int64_t>(imaginary)) * cic_scale_)};
        }
        const std::size_t ring_len = ring_.size();
        ring_[write_] = value;
        write_ = (write_ + 1) % ring_len;
        ++input_index_;
        for (;;) {
            auto position = static_cast<int64_t>(std::floor(next_output_));
            auto phase = static_cast<std::size_t>(std::round((next_output_ - static_cast<double>(position)) * 32.0));
            if (phase == 32) {
                phase = 0;
                ++position;
            }
            const int64_t first = position - half_;
            const auto& taps = arms_[phase];
            if (first + static_cast<int64_t>(taps.size()) > input_index_) return;
            auto offset = static_cast<std::size_t>(rem_euclid(static_cast<int64_t>(write_) + first - input_index_, static_cast<int64_t>(ring_len)));
            float real = 0.f, imaginary = 0.f;
            for (float tap : taps) {
                real += ring_[offset].re * tap;
                imaginary += ring_[offset].im * tap;
                offset = (offset + 1) % ring_len;
            }
            sink(C32{real, imaginary});
            next_output_ += step_;
        }
    }

private:
    std::size_t decimation_ = 1;
    uint64_t integrators_[6] = {};
    uint64_t delays_[6] = {};
    double cic_scale_ = 1.0;
    std::size_t counter_ = 0;
    std::vector<std::vector<float>> arms_;
    std::vector<C32> ring_;
    int64_t half_ = 0;
    double step_ = 1.0;
    int64_t input_index_ = 0;
    double next_output_ = 0.0;
    std::size_t write_ = 0;
};

// ---- 整合フィルタ + タイミング(原本 ClockRecovery)----
class ClockRecovery {
public:
    ClockRecovery() {
        const auto taps = root_raised_cosine(1024, 128.0, 32.0);
        std::vector<float> derivatives(taps.size(), 0.f);
        float sum = 0.f;
        for (std::size_t i = 0; i + 2 < taps.size(); ++i) {
            derivatives[i + 1] = taps[i + 2] - taps[i];
            sum += std::abs(derivatives[i + 1]);
        }
        for (float& value : derivatives) value *= 32.f / sum;
        arms_ = arms(taps, 32);
        derivatives_ = arms(derivatives, 32);
    }

    bool process(C32 sample, std::array<C32, kTaps>& values, std::array<C32, kTaps>& derivatives) {
        if (count_ >= ring_.size()) throw std::logic_error("jr_dsp: clock recovery buffer overflow");
        ring_[(start_ + count_) % ring_.size()] = sample;
        ++count_;
        const std::size_t length = arms_[0].size();
        if (!warm_) {
            if (count_ < 6 + length + 4 + 2 * (kTaps - 1)) return false;
            position_ = 6;
            warm_ = true;
        }
        const float saved = phase_;
        auto arm = static_cast<int32_t>(std::floor(phase_));
        int32_t shift = 0;
        while (arm >= 32) {
            phase_ -= 32.f;
            arm -= 32;
            ++shift;
        }
        while (arm < 0) {
            phase_ += 32.f;
            arm += 32;
            --shift;
        }
        const int32_t first = position_ + shift;
        if (first < 0 || static_cast<std::size_t>(first) + length + 2 * (kTaps - 1) > count_) {
            phase_ = saved;
            return false;
        }
        // 半シンボル間隔の窓で共通の小数位相を評価する。複素のまま保つ(後段の適応系列推定に振幅が要る)
        const auto& a = arms_[static_cast<std::size_t>(arm)];
        const auto& da = derivatives_[static_cast<std::size_t>(arm)];
        for (std::size_t k = 0; k < kTaps; ++k) {
            C32 matched{}, derivative{};
            for (std::size_t i = 0; i < length; ++i) {
                const C32 value = ring_[(start_ + static_cast<std::size_t>(first) + 2 * k + i) % ring_.size()];
                matched.re += value.re * a[i];
                matched.im += value.im * a[i];
                derivative.re += value.re * da[i];
                derivative.im += value.im * da[i];
            }
            values[k] = matched;
            derivatives[k] = derivative;
        }
        position_ = std::max(position_ + shift + 4, 0);
        while (position_ > 12) {
            --position_;
            --count_;
            start_ = (start_ + 1) % ring_.size();
        }
        return true;
    }

    void update(float error) {
        constexpr float BW = 0.02f;
        constexpr float DENOMINATOR = 1.f + 128.f * BW + BW * BW;
        constexpr float ALPHA = 256.f * BW / DENOMINATOR;
        constexpr float BETA = 4.f * BW * BW / DENOMINATOR;
        phase_ += rate_;
        for (int i = 0; i < 4; ++i) {
            rate_ += BETA * error;
            phase_ += rate_ + ALPHA * error;
        }
        rate_ = rate_ < -0.1f ? -0.1f : (rate_ > 0.1f ? 0.1f : rate_);
    }

private:
    std::vector<std::vector<float>> arms_, derivatives_;
    std::array<C32, 256> ring_{};
    std::size_t start_ = 0, count_ = 0;
    int32_t position_ = 0;
    bool warm_ = false;
    float phase_ = 16.f;
    float rate_ = 0.f;
};

// ---- 搬送波(原本 Carrier)----
// π/4 交番を回転で除き、系列推定の innovation(最良生き残りの予測との位相差)で 2 次ループを回す。
struct Carrier {
    double phase = 0.0, frequency = 0.0;
    int32_t alternation = 0;
    bool locked = false;
    double error = 0.0;
    detail::Sequence sequence;

    std::optional<uint8_t> process(C32 sample, bool reliable) {
        const double rotation = phase + static_cast<double>(alternation) * kPi / 4.0;
        const double sine = std::sin(rotation), cosine = std::cos(rotation);
        const C32 corrected{static_cast<float>(static_cast<double>(sample.re) * cosine + static_cast<double>(sample.im) * sine),
                            static_cast<float>(static_cast<double>(sample.im) * cosine - static_cast<double>(sample.re) * sine)};
        const auto r = sequence.process(corrected, reliable);
        const auto phase_error = static_cast<double>(r.phase_error < -0.5f ? -0.5f : (r.phase_error > 0.5f ? 0.5f : r.phase_error));
        frequency = std::clamp(frequency + 0.003 * phase_error, -0.65, 0.65);
        phase += frequency + 0.2 * phase_error;
        phase -= std::nearbyint(phase / (2.0 * kPi)) * (2.0 * kPi);   // round_ties_even(既定の丸めモード)
        error = 0.98 * error + 0.02 * static_cast<double>(std::sqrt(r.error_power));
        locked = reliable && error < 0.4;
        alternation = (alternation + 1) % 8;
        return r.dibit;
    }
};

} // namespace

// ---- 1 チャネル分(原本 SymbolChain + Demodulator)----
struct Demodulator::Impl {
    Resampler resampler;
    detail::SignalView signal;
    ClockRecovery clock;
    detail::Equalizer equalizer;
    Carrier carrier;
    std::array<C32, 64> agc{};
    std::size_t head = 0, fill = 0;
    float reference_peak = 0.f;
    int32_t profile = 0;
    int64_t symbols = 0, locked_symbols = 0;
    double osc_re = 1.0, osc_im = 0.0, step_re = 1.0, step_im = 0.0;
    std::size_t renormalize = 0;
    std::array<C32, kTaps> matched{}, derivatives{};

    Impl(double rate, double offset, int32_t prof) : resampler(rate), profile(prof) {
        const double angle = -2.0 * kPi * offset / rate;
        step_re = std::cos(angle);
        step_im = std::sin(angle);
    }

    std::optional<uint8_t> symbol(C32 value) {
        agc[head] = value;
        head = (head + 1) % agc.size();
        if (fill < agc.size()) {
            if (++fill < agc.size()) return std::nullopt;
        }
        float peak = 0.f;
        for (const C32& s : agc) {
            const float re = std::abs(s.re), im = std::abs(s.im);
            peak = std::fmax(peak, std::fmax(re, im) + 0.4f * std::fmin(re, im));
        }
        const C32 oldest = agc[head];
        // 約 1 s で減衰するピーク基準。短い深いフェードで増幅された雑音に等化器を学習させない
        reference_peak = std::fmax(peak, reference_peak * 0.99995f);
        const bool reliable = peak > 1e-12f && peak >= reference_peak * 0.08f;
        const float gain = 1.f / std::fmax(std::fmax(peak, reference_peak * 0.08f), 1e-12f);
        if (!clock.process(C32{oldest.re * gain, oldest.im * gain}, matched, derivatives)) return std::nullopt;
        const auto [sym, timing_error] = equalizer.process(matched, derivatives, reliable);
        clock.update(timing_error);
        const auto dibit = carrier.process(sym, reliable);
        // 観測は全補正の後(判定値ではなく受信のばらつきを残す)
        signal.push(carrier.sequence.equalized());
        if (!dibit) return std::nullopt;
        ++symbols;
        if (carrier.locked) ++locked_symbols;
        if (profile == 1) {
            constexpr uint8_t kRemap[4] = {0, 3, 1, 2};
            return kRemap[*dibit];
        }
        return dibit;
    }
};

std::string validate(double rate, double offset_hz) {
    if (!std::isfinite(rate) || rate < 9600.0 || rate > 20'000'000.0) return "IQ sample rate must be 9,600–20,000,000 Hz";
    if (!std::isfinite(offset_hz) || std::abs(offset_hz) + 4200.0 >= rate / 2.0) return "Channel lies outside the IQ sample bandwidth";
    return {};
}

Demodulator::Demodulator(double rate, double offset_hz, int profile) {
    if (auto e = validate(rate, offset_hz); !e.empty()) throw std::invalid_argument(e);
    if (profile != 0 && profile != 1) throw std::invalid_argument("profile must be 0 or 1");
    impl_ = std::make_unique<Impl>(rate, offset_hz, profile);
}
Demodulator::~Demodulator() = default;
Demodulator::Demodulator(Demodulator&&) noexcept = default;
Demodulator& Demodulator::operator=(Demodulator&&) noexcept = default;

std::size_t Demodulator::process(std::span<const std::complex<float>> iq, std::vector<uint8_t>& out) {
    for (const auto& s : iq)
        if (!std::isfinite(s.real()) || !std::isfinite(s.imag())) throw std::invalid_argument("IQ contains non-finite samples");
    Impl& m = *impl_;
    const std::size_t before = out.size();
    for (const auto& s : iq) {
        const double sr = s.real(), si = s.imag();
        const double real = sr * m.osc_re - si * m.osc_im;
        const double imaginary = sr * m.osc_im + si * m.osc_re;
        const double next = m.osc_re * m.step_re - m.osc_im * m.step_im;
        m.osc_im = m.osc_re * m.step_im + m.osc_im * m.step_re;
        m.osc_re = next;
        if (++m.renormalize == 4096) {
            const double magnitude = std::sqrt(m.osc_re * m.osc_re + m.osc_im * m.osc_im);
            m.osc_re /= magnitude;
            m.osc_im /= magnitude;
            m.renormalize = 0;
        }
        const C32 sample{static_cast<float>(real), static_cast<float>(m.profile == 1 ? -imaginary : imaginary)};
        m.resampler.process(sample, [&](C32 value) {
            if (auto dibit = m.symbol(value)) out.push_back(*dibit);
        });
    }
    return out.size() - before;
}

Snapshot Demodulator::snapshot() const {
    const Impl& m = *impl_;
    Snapshot s;
    s.symbols = m.symbols;
    s.locked_symbols = m.locked_symbols;
    s.carrier_error = m.carrier.error;
    s.frequency_offset_hz = m.carrier.frequency * 4800.0 / (2.0 * kPi);
    s.carrier_locked = m.carrier.locked ? 1 : 0;
    return s;
}

double Demodulator::read_signal(std::vector<std::complex<float>>& points, std::size_t capacity) const {
    std::array<C32, kSignalCapacity> buf;
    double evm = 0.0;
    const std::size_t n = impl_->signal.read(buf.data(), std::min(capacity, kSignalCapacity), evm);
    points.resize(n);
    for (std::size_t i = 0; i < n; ++i) points[i] = {buf[i].re, buf[i].im};
    return evm;
}

} // namespace spear::jr_train::dsp
