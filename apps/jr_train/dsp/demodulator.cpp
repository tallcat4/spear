// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 原本: crates/jr-conventional-dsp/src/{dsp,receiver}.rs。式の順序は原本のまま(浮動小数の結果を一致させるため)。
#include "jr_dsp.hpp"

#include "detail.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
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

// std::fmax / std::fmin と同じ値(片方が NaN なら他方)。libm を呼ばないようにインラインで書く。
// ±0 の扱いは C の規定でも未規定だが、ここで使う値はすべて +0 以上(絶対値とその和)なので問題にならない
inline float max_nan(float a, float b) { return (a > b || std::isnan(b)) ? a : b; }
inline float min_nan(float a, float b) { return (a < b || std::isnan(b)) ? a : b; }

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
        // 2 回書く(i と i + ring_len_): 窓を剰余なしの連続領域として読む。窓は出力が揃うまで(最大 kGroup 個、
        // 1 出力あたり入力 20 sample 以下)上書きされない長さにする
        ring_len_ = count + 16 + 128;
        ring_.assign(2 * ring_len_, C32{});
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
        const std::size_t ring_len = ring_len_;
        ring_[write_] = value;
        ring_[write_ + ring_len] = value;
        if (++write_ == ring_len) write_ = 0;
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
            // (write_ + first − input_index_) mod ring_len。first − input_index_ は −ring_len 以上(窓は直近の ring_len 内)
            int64_t wrapped = static_cast<int64_t>(write_) + first - input_index_;
            if (wrapped < 0) wrapped += static_cast<int64_t>(ring_len);
            const auto offset = static_cast<std::size_t>(wrapped);
            pending_[pending_count_++] = Pending{ring_.data() + offset, taps.data()};   // offset + taps.size() <= 2 * ring_len
            if (pending_count_ == kGroup) flush(sink);
            next_output_ += step_;
        }
    }

    // 揃った出力を順に sink へ(process() の呼び出しの終わりにも呼ぶ)。kGroup 個の内積を同時に進めて加算のレイテンシを隠す。
    // 各出力の和は原本と同じくタップの順に 1 本の累算器で取る(結果は 1 つずつ計算したときと同じ)
    template <class Sink>
    void flush(Sink&& sink) {
        const std::size_t length = arms_[0].size();
        std::size_t n = 0;
        if (pending_count_ == kGroup) {
            const Pending a = pending_[0], b = pending_[1], c = pending_[2], d = pending_[3];
            float ar = 0.f, ai = 0.f, br = 0.f, bi = 0.f, cr = 0.f, ci = 0.f, dr = 0.f, di = 0.f;
            for (std::size_t i = 0; i < length; ++i) {
                ar += a.window[i].re * a.taps[i];
                ai += a.window[i].im * a.taps[i];
                br += b.window[i].re * b.taps[i];
                bi += b.window[i].im * b.taps[i];
                cr += c.window[i].re * c.taps[i];
                ci += c.window[i].im * c.taps[i];
                dr += d.window[i].re * d.taps[i];
                di += d.window[i].im * d.taps[i];
            }
            pending_count_ = 0;
            sink(C32{ar, ai});
            sink(C32{br, bi});
            sink(C32{cr, ci});
            sink(C32{dr, di});
            return;
        }
        for (; n < pending_count_; ++n) {
            const Pending a = pending_[n];
            float real = 0.f, imaginary = 0.f;
            for (std::size_t i = 0; i < length; ++i) {
                real += a.window[i].re * a.taps[i];
                imaginary += a.window[i].im * a.taps[i];
            }
            sink(C32{real, imaginary});
        }
        pending_count_ = 0;
    }

private:
    std::size_t decimation_ = 1;
    uint64_t integrators_[6] = {};
    uint64_t delays_[6] = {};
    double cic_scale_ = 1.0;
    std::size_t counter_ = 0;
    std::vector<std::vector<float>> arms_;
    std::vector<C32> ring_;
    std::size_t ring_len_ = 0;
    struct Pending {
        const C32* window;
        const float* taps;
    };
    static constexpr std::size_t kGroup = 4;
    std::array<Pending, kGroup> pending_{};
    std::size_t pending_count_ = 0;
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
        if (count_ >= kRing) throw std::logic_error("jr_dsp: clock recovery buffer overflow");
        const std::size_t slot = (start_ + count_) % kRing;
        ring_[slot] = sample;
        ring_[slot + kRing] = sample;   // 2 回書く: 窓を剰余なしの連続領域として読む
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
        // 3 つの窓を同時に進める(12 本の累算器で加算のレイテンシを隠す)。各和はタップの順に 1 本の累算器で取る
        const float* a = arms_[static_cast<std::size_t>(arm)].data();
        const float* da = derivatives_[static_cast<std::size_t>(arm)].data();
        const C32* base = ring_.data() + (start_ + static_cast<std::size_t>(first)) % kRing;   // + 2 (kTaps − 1) + length <= 2 kRing
        static_assert(kTaps % 3 == 0);
        for (std::size_t k = 0; k < kTaps; k += 3) {
            const C32* x0 = base + 2 * k;
            const C32* x1 = x0 + 2;
            const C32* x2 = x0 + 4;
            float m0r = 0.f, m0i = 0.f, d0r = 0.f, d0i = 0.f, m1r = 0.f, m1i = 0.f, d1r = 0.f, d1i = 0.f, m2r = 0.f, m2i = 0.f, d2r = 0.f, d2i = 0.f;
            for (std::size_t i = 0; i < length; ++i) {
                const float t = a[i], dt = da[i];
                m0r += x0[i].re * t;
                m0i += x0[i].im * t;
                d0r += x0[i].re * dt;
                d0i += x0[i].im * dt;
                m1r += x1[i].re * t;
                m1i += x1[i].im * t;
                d1r += x1[i].re * dt;
                d1i += x1[i].im * dt;
                m2r += x2[i].re * t;
                m2i += x2[i].im * t;
                d2r += x2[i].re * dt;
                d2i += x2[i].im * dt;
            }
            values[k] = C32{m0r, m0i};
            derivatives[k] = C32{d0r, d0i};
            values[k + 1] = C32{m1r, m1i};
            derivatives[k + 1] = C32{d1r, d1i};
            values[k + 2] = C32{m2r, m2i};
            derivatives[k + 2] = C32{d2r, d2i};
        }
        position_ = std::max(position_ + shift + 4, 0);
        while (position_ > 12) {
            --position_;
            --count_;
            start_ = (start_ + 1) % kRing;
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
    static constexpr std::size_t kRing = 256;
    std::array<C32, 2 * kRing> ring_{};
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

using V8 = float __attribute__((vector_size(32)));
using M8 = int32_t __attribute__((vector_size(32)));

// 原本の peak = fmax(peak, x) の畳み込み(peak = 0 から)と同じ値: 0 と NaN でない点の最大。最大は順序に依らない
#if defined(__x86_64__) && defined(__GNUC__) && !defined(__clang__) && !defined(__SANITIZE_THREAD__) && !defined(__SANITIZE_ADDRESS__)
__attribute__((target_clones("avx2", "default")))
#endif
float window_peak(const float* level) {
    V8 acc{};
    for (std::size_t i = 0; i < 64; i += 8) {
        V8 x;
        std::memcpy(&x, level + i, sizeof x);
        const M8 m = x > acc;   // NaN は偽 → 捨てる
        acc = (V8)(((M8)x & m) | ((M8)acc & ~m));
    }
    float peak = 0.f;
    for (std::size_t l = 0; l < 8; ++l) peak = acc[l] > peak ? acc[l] : peak;
    return peak;
}

} // namespace

// ---- 1 チャネル分(原本 SymbolChain + Demodulator)----
struct Demodulator::Impl {
    Resampler resampler;
    detail::SignalView signal;
    ClockRecovery clock;
    detail::Equalizer equalizer;
    Carrier carrier;
    std::array<C32, 64> agc{};
    alignas(32) std::array<float, 64> level{};   // agc の各点の包絡
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
        {   // 各 sample の包絡(max + 0.4 min)は入ったときに 1 回だけ計算する(原本は窓の全点を毎回計算するが値は同じ)
            const float re = std::abs(value.re), im = std::abs(value.im);
            level[head] = max_nan(re, im) + 0.4f * min_nan(re, im);
        }
        head = (head + 1) % agc.size();
        if (fill < agc.size()) {
            if (++fill < agc.size()) return std::nullopt;
        }
        const float peak = window_peak(level.data());
        const C32 oldest = agc[head];
        // 約 1 s で減衰するピーク基準。短い深いフェードで増幅された雑音に等化器を学習させない
        reference_peak = max_nan(peak, reference_peak * 0.99995f);
        const bool reliable = peak > 1e-12f && peak >= reference_peak * 0.08f;
        const float gain = 1.f / max_nan(max_nan(peak, reference_peak * 0.08f), 1e-12f);
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
    m.resampler.flush([&](C32 value) {
        if (auto dibit = m.symbol(value)) out.push_back(*dibit);
    });
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
