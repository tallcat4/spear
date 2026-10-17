// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 原本: src/planner.rs、src/plan_features.rs、src/plan_target.rs、src/plan_acoustics.rs
#include "plan.hpp"

#include "common.hpp"
#include "models.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <limits>

namespace spear::jr_train::voice {
namespace {

// ================= plan_features.rs =================
struct Half {
    std::size_t mode = 0, stage1 = 0, low = 0, high = 0;
    std::array<std::size_t, 2> gain{}, word{};
};

std::size_t read(const uint8_t* w, std::size_t begin, std::size_t count) {
    std::size_t a = 0;
    for (std::size_t i = begin; i < begin + count; ++i) a = 2 * a + w[i];
    return a;
}
std::size_t pick(const uint8_t* w, std::initializer_list<std::size_t> positions) {
    std::size_t a = 0;
    for (std::size_t i : positions) a = 2 * a + w[i];
    return a;
}

Half half_params(const uint8_t* w, std::size_t h) {
    const std::size_t q = 84 + 39 * h, a = 20 * h, b = 40 + 20 * h;
    const std::size_t low = read(w, q + 23, 4);
    Half p;
    p.mode = w[q];
    p.stage1 = read(w, q + 1, 7);
    p.low = pick(w, {q + 8, q + 9, q + 10, q + 11, a + 3, a + 1});
    p.high = pick(w, {b + 3, b + 1, a + 2, a, b + 2, b});
    p.gain = {16 * read(w, q + 20, 3) + low, 16 * read(w, q + 32, 3) + (h == 1 ? low : read(w, q + 35, 4))};
    for (std::size_t s = 0; s < 2; ++s) {
        const std::size_t k = 8 * s;
        const std::size_t j = s == 1 ? 30 : 18;
        p.word[s] = pick(w, {q + j, q + j + 1, b + 7 + k, b + 5 + k, a + 6 + k, a + 4 + k, b + 6 + k, b + 4 + k, a + 11 + k, a + 9 + k, b + 11 + k, b + 9 + k, a + 10 + k,
                             a + 8 + k, b + 10 + k, b + 8 + k});
    }
    return p;
}

std::pair<std::array<int16_t, 80>, std::size_t> innovation(std::size_t index) {
    const auto& t = planner_model();
    index = std::min<std::size_t>(index, static_cast<std::size_t>(t.INDEX_LIMIT[0]) - 1);
    std::size_t cls = 5;
    while (cls > 0 && index < t.CLASS_BASE[cls]) --cls;
    std::array<int16_t, 80> raw{};
    std::size_t rest = index - t.CLASS_BASE[cls];
    auto add = [](int16_t x, int32_t v) { return static_cast<int16_t>(std::clamp(static_cast<int32_t>(x) + v, -32768, 32767)); };
    if (cls == 5) {
        const int16_t* pairs[2] = {t.ALT_A.data() + 5 * (rest / 128), t.ALT_B.data() + 5 * (rest % 128)};
        for (const int16_t* pair : pairs)
            for (std::size_t e = 0; e < 5; ++e) {
                const int16_t entry = pair[e];
                const std::size_t pos = static_cast<std::size_t>(std::abs(static_cast<int>(entry))) - 1;
                raw.at(pos) = add(raw.at(pos), entry > 0 ? 8192 : -8192);
            }
    } else {
        const auto count = static_cast<std::size_t>(t.PULSES[cls]);
        std::array<std::size_t, 3> signs{}, position{};
        for (std::size_t s = 0; s < count; ++s) {
            signs[s] = rest & 1;
            rest /= 2;
        }
        constexpr int64_t lo = std::numeric_limits<int32_t>::min(), hi = std::numeric_limits<int32_t>::max();
        int64_t value = static_cast<int64_t>(rest) * 65536;
        for (std::size_t k = count; k-- > 0;) {
            const int64_t quotient = std::clamp<int64_t>((value / 65536) * static_cast<int64_t>(t.RADIX_INVERSE[3 * cls + k]) * 2, lo, hi) / 262144;
            value = std::clamp<int64_t>(value - quotient * static_cast<int64_t>(t.RADIX[3 * cls + k]) * 65536, lo, hi);
            position[k] = static_cast<std::size_t>(value / 65536);
            value = quotient * 65536;
        }
        const auto first = static_cast<std::size_t>(t.SHAPE_SELECT[3 * cls]);
        for (std::size_t k = 0; k < count; ++k) {
            const auto pos = static_cast<std::size_t>(t.POSITIONS.at(40 * (3 * cls + k) + position[k]));
            for (std::size_t n = pos; n < std::min<std::size_t>(80, pos + 20); ++n) {
                const int32_t v = t.SHAPES[20 * (first + k) + n - pos];
                raw[n] = add(raw[n], signs[k] != 0 ? v : -v);
            }
        }
    }
    return {raw, cls};
}

void spectral(const Half& p, std::array<std::array<float, 10>, 3>& history, std::array<float, 10>& residual, std::array<float, 10>& lsf) {
    const auto& t = planner_model();
    for (std::size_t k = 0; k < 10; ++k) {
        residual[k] = t.SPECTRAL[p.stage1 * 10 + k] + t.SPECTRAL[k < 5 ? 1280 + p.low * 5 + k : 1600 + p.high * 5 + k - 5];
        const float* w = t.MA.data() + p.mode * 40;
        lsf[k] = ((residual[k] * w[k] + history[0][k] * w[10 + k]) + history[1][k] * w[20 + k]) + history[2][k] * w[30 + k];
    }
    std::stable_sort(lsf.begin(), lsf.end(), [](float a, float b) { return rs::total_lt(a, b); });
    lsf[0] = rs::max(lsf[0], 41.f);
    for (std::size_t k = 1; k < 10; ++k) lsf[k] = rs::max(lsf[k], lsf[k - 1] + 321.f);
    lsf[9] = rs::min(lsf[9], 25682.f);
    history[2] = history[1];
    history[1] = history[0];
    history[0] = residual;
}

// ================= plan_target.rs =================
struct Matrix {
    std::size_t inputs;
    const std::vector<int8_t>& weights;
    const std::vector<float>& scales;
    const std::vector<float>& bias;
    void affine(const float* input, float* out, std::size_t n) const {
        for (std::size_t j = 0; j < n; ++j) {
            float sum = 0.f;
            for (std::size_t i = 0; i < inputs; ++i) sum += input[i] * static_cast<float>(weights[j * inputs + i]);
            out[j] = sum * scales[j] + bias[j];
        }
    }
};

void isotonic(std::array<float, 140>& values) {
    std::array<float, 140> pooled{};
    std::array<std::size_t, 140> counts{};
    std::size_t blocks = 0;
    for (float value : values) {
        pooled[blocks] = value;
        counts[blocks] = 1;
        ++blocks;
        while (blocks >= 2 && pooled[blocks - 2] > pooled[blocks - 1]) {
            const std::size_t a = blocks - 2, b = blocks - 1;
            const std::size_t total = counts[a] + counts[b];
            pooled[a] = (pooled[a] * static_cast<float>(counts[a]) + pooled[b] * static_cast<float>(counts[b])) / static_cast<float>(total);
            counts[a] = total;
            --blocks;
        }
    }
    std::size_t at = 0;
    for (std::size_t b = 0; b < blocks; ++b) {
        std::fill_n(values.begin() + static_cast<std::ptrdiff_t>(at), counts[b], rs::clamp(pooled[b], 0.f, 1.f));
        at += counts[b];
    }
}

// ================= plan_acoustics.rs =================
float level(const int16_t* pcm, std::size_t n) {
    int64_t sum = 0;
    for (std::size_t i = 0; i < n; ++i) sum += static_cast<int64_t>(pcm[i]) * static_cast<int64_t>(pcm[i]);
    return 10.f * std::log10(rs::max(static_cast<float>(sum) / 1073741824.f / static_cast<float>(n), 1e-12f));
}

// Σ a[j] b[j](j < n)を int64 で。整数の和なので順序を変えても値は同じ(積は int32 に収まる: |a b| <= 2^30)
using I16x8 = int16_t __attribute__((vector_size(16)));
using I32x8 = int32_t __attribute__((vector_size(32)));
using I32x4 = int32_t __attribute__((vector_size(16)));
using I64x4 = int64_t __attribute__((vector_size(32)));
#if defined(__x86_64__) && defined(__GNUC__) && !defined(__clang__) && !defined(__SANITIZE_THREAD__) && !defined(__SANITIZE_ADDRESS__)
__attribute__((target_clones("avx2", "default")))
#endif
int64_t dot_i16(const int16_t* a, const int16_t* b, std::size_t n) {
    I64x4 acc{};
    std::size_t j = 0;
    for (; j + 8 <= n; j += 8) {
        I16x8 x, y;
        std::memcpy(&x, a + j, sizeof x);
        std::memcpy(&y, b + j, sizeof y);
        const I32x8 p = __builtin_convertvector(x, I32x8) * __builtin_convertvector(y, I32x8);
        I32x4 lo, hi;
        std::memcpy(&lo, &p, sizeof lo);
        std::memcpy(&hi, reinterpret_cast<const char*>(&p) + sizeof lo, sizeof hi);
        acc += __builtin_convertvector(lo, I64x4) + __builtin_convertvector(hi, I64x4);
    }
    int64_t sum = acc[0] + acc[1] + acc[2] + acc[3];
    for (; j < n; ++j) sum += static_cast<int64_t>(a[j]) * static_cast<int64_t>(b[j]);
    return sum;
}

std::array<float, 141> correlation(const int16_t* pcm, std::size_t n) {
    std::array<int64_t, 641> sums{}, squares{};
    for (std::size_t i = 0; i < n; ++i) {
        sums[i + 1] = sums[i] + pcm[i];
        squares[i + 1] = squares[i] + static_cast<int64_t>(pcm[i]) * static_cast<int64_t>(pcm[i]);
    }
    const auto size = static_cast<int64_t>(n);
    const int64_t total = sums[n];
    const int64_t total_square = total * total;
    std::array<float, 141> out{};
    for (std::size_t i = 0; i < 141; ++i) {
        const std::size_t lag = i + 20;
        const int64_t raw = dot_i16(pcm, pcm + lag, n - lag);
        const int64_t left_sum = sums[n - lag], right_sum = total - sums[lag];
        const int64_t extra = (size - static_cast<int64_t>(lag)) * total_square;
        const int64_t cov = size * size * raw - size * total * (left_sum + right_sum) + extra;
        const int64_t left = size * size * squares[n - lag] - 2 * size * total * left_sum + extra;
        const int64_t right = size * size * (squares[n] - squares[lag]) - 2 * size * total * right_sum + extra;
        out[i] = rs::clamp(static_cast<float>(cov) / std::sqrt(rs::max(static_cast<float>(left) * static_cast<float>(right), 1e-30f)), -1.f, 1.f);
    }
    return out;
}

std::array<float, 20> bands(const int16_t* pcm) {
    const auto& t = planner_model();
    std::array<float, 512> real{}, imag{};
    for (std::size_t i = 0; i < 160; ++i) real[i] = static_cast<float>(pcm[i]) / 32768.f * t.F32_HANN[i];
    std::size_t j = 0;
    for (std::size_t i = 1; i < 512; ++i) {
        std::size_t bit = 256;
        while (j & bit) {
            j ^= bit;
            bit >>= 1;
        }
        j ^= bit;
        if (i < j) {
            std::swap(real[i], real[j]);
            std::swap(imag[i], imag[j]);
        }
    }
    for (std::size_t len = 2; len <= 512; len *= 2) {
        const std::size_t half = len / 2, step = 2048 / len;
        for (std::size_t begin = 0; begin < 512; begin += len)
            for (std::size_t k = 0; k < half; ++k) {
                const std::size_t a = begin + k, b = a + half;
                const float wr = t.F32_TWIDDLE_REAL[k * step], wi = t.F32_TWIDDLE_IMAG[k * step];
                const float br = real[b] * wr - imag[b] * wi;
                const float bi = real[b] * wi + imag[b] * wr;
                real[b] = real[a] - br;
                imag[b] = imag[a] - bi;
                real[a] += br;
                imag[a] += bi;
            }
    }
    std::array<float, 257> energy{};
    for (std::size_t i = 0; i < 257; ++i) energy[i] = (real[i] * real[i] + imag[i] * imag[i]) / t.F32_HANN_ENERGY;
    std::array<float, 20> result{};
    for (std::size_t band = 0; band < 20; ++band) {
        float power = 0.f;
        const std::size_t start = t.F32_BAND_OFFSET[band], end = t.F32_BAND_OFFSET[band + 1];
        for (std::size_t k = start; k < end; ++k) power += energy[t.F32_BAND_START[band] + k - start] * t.F32_BAND_WEIGHTS[k];
        result[band] = 10.f * std::log10(rs::max(power, 1e-12f));
    }
    return result;
}

float cost(const int16_t* pcm, std::size_t n, const Target& target, std::size_t kind) {
    const auto corr = correlation(pcm, n);
    float strength = -1.f;
    for (float c : corr) strength = rs::max(strength, c);
    std::size_t lag = 0;
    if (level(pcm, n) >= -40.f && strength >= 0.5f) {
        const float minimum = rs::max(0.35f, 0.9f * strength);
        for (std::size_t i = 0; i < 141; ++i)
            if (corr[i] >= minimum && (i == 0 || corr[i] >= corr[i - 1]) && (i == 140 || corr[i] >= corr[i + 1])) {
                lag = i + 20;
                break;
            }
    }
    const float* mean = target.mean.data() + (kind == 0 ? 0 : (kind == 1 ? 46 : 48));
    const float error = strength - mean[kind == 0 ? 44 : 0];
    float result = error * error / (0.17f * 0.17f);
    result += lag != 0 ? target.risk[kind][lag - 20] : target.missing[kind];
    if (lag == 0) result += target.missing[kind];
    if (kind == 0) {
        float levels = 0.f, band_error = 0.f, shape_error = 0.f;
        for (std::size_t i = 0; i < 4; ++i) {
            const float x = level(pcm + 80 * i, 80) - mean[i];
            levels += x * x;
        }
        for (std::size_t half = 0; half < 2; ++half) {
            const auto b = bands(pcm + 160 * half);
            float center = 0.f;
            for (std::size_t j = 0; j < 20; ++j) center += b[j] - mean[4 + half * 20 + j];
            center /= 20.f;
            for (std::size_t j = 0; j < 20; ++j) {
                const float x = b[j] - mean[4 + half * 20 + j];
                band_error += x * x;
                const float y = x - center;
                shape_error += y * y;
            }
        }
        result += 0.25f * (levels / 4.f) / 25.f + 0.25f * (band_error / 40.f) / 36.f + 0.5f * (shape_error / 40.f) / 16.f;
        std::size_t clipped = 0;
        for (std::size_t i = 0; i < n; ++i) clipped += std::abs(static_cast<int32_t>(pcm[i])) >= 32767 ? 1 : 0;
        result += 2.f * static_cast<float>(clipped);
    }
    return result;
}

// ================= planner.rs =================
void fade_slice(int16_t* pcm, std::size_t n, bool fade_in) {
    const float denominator = static_cast<float>(std::max<std::size_t>(n > 0 ? n - 1 : 0, 1));
    for (std::size_t i = 0; i < n; ++i) {
        const float gain = 0.5f - 0.5f * std::cos(kPi * static_cast<float>(i) / denominator);
        pcm[i] = rs::as<int16_t>(round_even(static_cast<float>(pcm[i]) * (fade_in ? gain : 1.f - gain)));
    }
}

PcmFrame mix_planned(const PcmFrame& raw, const PlanInput& input) {
    PcmFrame pcm{};
    for (std::size_t i = 0; i < 320; ++i) pcm[i] = rs::as<int16_t>(round_even(input.offset[i] + static_cast<float>(raw[i]) * input.weight[i]));
    if (input.fade_in) fade_slice(pcm.data(), 80, true);
    if (input.fade_out) fade_slice(pcm.data() + 240, 80, false);
    return pcm;
}

} // namespace

std::array<float, 220> PlanFeatures::process(const PayloadBytes& payload, const Controls& controls, bool reset) {
    const auto& t = planner_model();
    if (payload[19] & 0xc0) throw Failure{};
    for (float v : controls)
        if (!std::isfinite(v)) throw Failure{};
    std::array<uint8_t, 158> bits{};
    for (std::size_t i = 0; i < 158; ++i) bits[i] = (payload[i / 8] >> (i % 8)) & 1;
    std::array<uint8_t, 158> wire{};
    std::copy(bits.begin() + 78, bits.end(), wire.begin());
    for (std::size_t i : {1, 5, 7}) wire[i] ^= 1;
    std::copy_n(bits.begin(), 74, wire.begin() + 84);
    std::array<Half, 2> normal = {half_params(wire.data(), 0), half_params(wire.data(), 1)};
    normal[1].gain[1] = (normal[1].gain[1] / 16) * 16 + read(bits.data(), 74, 4);
    wire[1] ^= 1;
    const std::array<Half, 2> alternate = {half_params(wire.data(), 0), half_params(wire.data(), 1)};
    if (reset) {
        std::array<float, 10> mean{};
        std::copy_n(t.LSF_MEAN.begin(), 10, mean.begin());
        for (auto& h : spectral_history_) h = {mean, mean, mean};
        gain_history_.fill(t.INITIAL_GAIN);
    }
    std::array<float, 28> current{};
    std::array<float, 220> out{};
    for (std::size_t i = 0; i < 4; ++i) {
        current[i] = rs::clamp(controls[i], -120.f, -3.f);
        current[4 + i] = rs::clamp(controls[70 + i], 0.f, 1.f);
        current[8 + i] = std::log(rs::clamp(controls[74 + i], 20.f, 160.f));
    }
    for (std::size_t i = 0; i < 8; ++i) {
        current[12 + i] = rs::clamp(controls[6 + i], -4.f, 4.f);
        current[20 + i] = rs::clamp(controls[38 + i], -4.f, 4.f);
    }
    for (std::size_t i = 0; i < 28; ++i) {
        out[i] = current[i];
        out[28 + i] = reset ? 0.f : current[i] - previous_[i];
        average_[i] = reset ? current[i] : 0.75f * average_[i] + 0.25f * current[i];
        out[56 + i] = average_[i];
        previous_[i] = current[i];
    }
    for (std::size_t h = 0; h < 2; ++h) {
        const Half& p = normal[h];
        std::array<float, 10> residual{}, lsf{}, other_residual{}, other{};
        spectral(p, spectral_history_[0], residual, lsf);
        spectral(alternate[h], spectral_history_[1], other_residual, other);
        float* o = out.data() + 84 + 68 * h;
        for (std::size_t k = 0; k < 10; ++k) {
            o[k] = lsf[k] / 8192.f;
            o[10 + k] = other[k] / 8192.f;
            o[20 + k] = residual[k] / 8192.f;
        }
        for (std::size_t s = 0; s < 2; ++s) {
            const float correction = t.GAIN_LOG[p.gain[s]];
            o[30 + s] = correction;
            std::copy(gain_history_.begin(), gain_history_.end(), o + 32 + 4 * s);
            for (std::size_t tap = 3; tap >= 1; --tap) gain_history_[tap] = gain_history_[tap - 1];
            gain_history_[0] = correction;
            o[40 + s] = t.GAIN_ADAPTIVE[p.gain[s]] / 16384.f;
            const auto [raw, cls] = innovation(p.word[s]);
            int64_t e = 0;
            for (int16_t v : raw) e += static_cast<int64_t>(v) * static_cast<int64_t>(v);
            const float energy = rs::max(static_cast<float>(e) / 80.f, 1.f);
            const std::size_t start = 42 + 13 * s;
            o[start] = std::log(energy);
            const std::size_t lags[4] = {1, 2, 4, 8};
            for (std::size_t j = 0; j < 4; ++j) {
                const std::size_t lag = lags[j];
                int64_t dot = 0;
                for (std::size_t i = 0; i < 80 - lag; ++i) dot += static_cast<int64_t>(raw[i]) * static_cast<int64_t>(raw[i + lag]);
                o[start + 1 + j] = (static_cast<float>(dot) / static_cast<float>(80 - lag)) / energy;
            }
            o[start + 5 + cls] = 1.f;
        }
    }
    return out;
}

Target ModelState::process(const std::array<float, 220>& features, const Controls& controls, bool reset) {
    const auto& m = planner_model();
    std::array<float, 220> normalized{};
    for (std::size_t i = 0; i < 220; ++i) {
        if (!std::isfinite(features[i]) || m.V__SCALE[i] <= 0.f) throw Failure{};
        normalized[i] = rs::clamp((features[i] - m.V__MEAN[i]) / m.V__SCALE[i], -6.f, 6.f);
    }
    for (std::size_t i = 0; i < 4; ++i) {
        if (!std::isfinite(controls[74 + i])) throw Failure{};
        if (reset) {
            previous_[i] = controls[74 + i];
            older_[i] = controls[74 + i];
        }
    }
    struct Window {
        Matrix mean_model, scores_model, cdf_model;
        float voice_rate;
    };
    const Window windows[3] = {
        {{220, m.Q__CURRENT_WEIGHT, m.S__CURRENT_WEIGHT, m.V__CURRENT_BIAS},
         {220, m.Q__CURRENT_CDF_WEIGHT, m.S__CURRENT_CDF_WEIGHT, m.V__CURRENT_CDF_BIAS},
         {16, m.Q__CURRENT_CDF_BASIS, m.S__CURRENT_CDF_BASIS, m.V__CURRENT_CDF_MEAN},
         m.V__CURRENT_VOICE_RATE[0]},
        {{220, m.Q__PREVIOUS_WEIGHT, m.S__PREVIOUS_WEIGHT, m.V__PREVIOUS_BIAS},
         {220, m.Q__PREVIOUS_CDF_WEIGHT, m.S__PREVIOUS_CDF_WEIGHT, m.V__PREVIOUS_CDF_BIAS},
         {16, m.Q__PREVIOUS_CDF_BASIS, m.S__PREVIOUS_CDF_BASIS, m.V__PREVIOUS_CDF_MEAN},
         m.V__PREVIOUS_VOICE_RATE[0]},
        {{220, m.Q__TRAILING_WEIGHT, m.S__TRAILING_WEIGHT, m.V__TRAILING_BIAS},
         {220, m.Q__TRAILING_CDF_WEIGHT, m.S__TRAILING_CDF_WEIGHT, m.V__TRAILING_CDF_BIAS},
         {16, m.Q__TRAILING_CDF_BASIS, m.S__TRAILING_CDF_BASIS, m.V__TRAILING_CDF_MEAN},
         m.V__TRAILING_VOICE_RATE[0]},
    };
    Target out;
    constexpr float kSigma = 0.057762265f;
    constexpr float kPitchScale = 7.213475f;
    for (std::size_t kind = 0; kind < 3; ++kind) {
        const Window& w = windows[kind];
        const std::size_t offset = kind == 0 ? 0 : (kind == 1 ? 46 : 48);
        const std::size_t count = kind == 0 ? 46 : 2;
        float* mean = out.mean.data() + offset;
        w.mean_model.affine(normalized.data(), mean, count);
        if (kind == 0) {
            for (std::size_t i = 0; i < 4; ++i) mean[i] = rs::clamp(mean[i], -120.f, 0.f);
            for (std::size_t i = 4; i < 44; ++i) mean[i] = rs::clamp(mean[i], -120.f, 20.f);
        }
        for (std::size_t i = count - 2; i < count; ++i) mean[i] = rs::clamp(mean[i], 0.f, 1.f);
        std::array<float, 16> scores{};
        std::array<float, 140> cdf{};
        w.scores_model.affine(normalized.data(), scores.data(), 16);
        w.cdf_model.affine(scores.data(), cdf.data(), 140);
        std::array<float, 8> periods{};
        const std::size_t size = kind == 0 ? 4 : 8;
        if (kind == 0) {
            std::copy_n(controls.begin() + 74, 4, periods.begin());
        } else if (kind == 1) {
            std::copy_n(older_.begin() + 2, 2, periods.begin());
            std::copy(previous_.begin(), previous_.end(), periods.begin() + 2);
            std::copy_n(controls.begin() + 74, 2, periods.begin() + 6);
        } else {
            std::copy(previous_.begin(), previous_.end(), periods.begin());
            std::copy_n(controls.begin() + 74, 4, periods.begin() + 4);
        }
        for (std::size_t i = 0; i < size; ++i) periods[i] = std::log(rs::clamp(periods[i], 20.f, 160.f));
        for (std::size_t i = 0; i < 140; ++i) {
            float prior = 0.f;
            for (std::size_t k = 0; k < size; ++k) {
                const float z = rs::clamp((m.EDGES[i] - periods[k]) / kSigma, -50.f, 50.f);
                prior += 1.f / (1.f + std::exp(-z));
            }
            cdf[i] += prior / static_cast<float>(size);
        }
        isotonic(cdf);
        float previous = 0.f, cumulative = 0.f;
        std::array<float, 141> sums{};
        for (std::size_t i = 0; i < 141; ++i) {
            const float value = i == 140 ? 1.f : cdf[i];
            cumulative += (value - previous) * m.LOG_LAGS[i];
            previous = value;
            sums[i] = cumulative;
        }
        const float voice = mean[count - 1] / w.voice_rate;
        out.missing[kind] = 5.f * voice;
        for (std::size_t i = 0; i < 141; ++i) {
            const float c = i == 140 ? 1.f : cdf[i];
            const float risk = m.LOG_LAGS[i] * (2.f * c - 1.f) + cumulative - 2.f * sums[i];
            out.risk[kind][i] = rs::max(risk, 0.f) * kPitchScale * voice;
        }
    }
    std::array<float, 16> hidden{};
    std::array<float, 8> hidden2{};
    std::array<float, 1> residual{};
    Matrix{220, m.Q__NET_W1, m.S__NET_W1, m.V__NET_B1}.affine(normalized.data(), hidden.data(), 16);
    for (auto& v : hidden) v = std::tanh(v);
    Matrix{16, m.Q__NET_W2, m.S__NET_W2, m.V__NET_B2}.affine(hidden.data(), hidden2.data(), 8);
    for (auto& v : hidden2) v = std::tanh(v);
    Matrix{8, m.Q__NET_W3, m.S__NET_W3, m.V__NET_B3}.affine(hidden2.data(), residual.data(), 1);
    const float parent = rs::clamp(out.mean[44], 1e-4f, 1.f - 1e-4f);
    const float logit = rs::clamp(std::log(parent / (1.f - parent)) + residual[0], -50.f, 50.f);
    out.mean[44] = 1.f / (1.f + std::exp(-logit));
    older_ = previous_;
    std::copy_n(controls.begin() + 74, 4, previous_.begin());
    return out;
}

float frame_cost(const PcmFrame& pcm, const Target& target, const PcmFrame* past, const PcmFrame* before) {
    float value = cost(pcm.data(), 320, target, 0);
    std::array<int16_t, 640> window{};
    if (before && past) {
        std::copy(before->begin() + 160, before->end(), window.begin());
        std::copy(past->begin(), past->end(), window.begin() + 160);
        std::copy_n(pcm.begin(), 160, window.begin() + 480);
        value += 0.5f * cost(window.data(), 640, target, 1);
    }
    if (past) {
        std::copy(past->begin(), past->end(), window.begin());
        std::copy(pcm.begin(), pcm.end(), window.begin() + 320);
        value += 0.5f * cost(window.data(), 640, target, 2);
    }
    return value;
}

std::optional<PlanOutput> Planner::push(const PlanInput& input) {
    const bool reset = !pending_ && !previous_;
    if (pending_ && (pending_->input.key == std::numeric_limits<uint64_t>::max() || pending_->input.key + 1 != input.key)) throw Failure{};
    const auto features = features_.process(input.payload, input.controls, reset);
    const Target target = model_.process(features, input.controls, reset);
    auto result = render(&input, &target);
    pending_ = Pending{input, target, reset};
    return result;
}

std::optional<PlanOutput> Planner::render(const PlanInput* ahead_input, const Target* ahead_target) {
    if (!pending_) return std::nullopt;
    const Pending pending = std::move(*pending_);
    pending_.reset();
    if (pending.reset) {
        state_.reset();
        previous_.reset();
        older_.reset();
    }
    std::array<Vocoder, 3> trials = {state_, state_, state_};
    std::array<PcmFrame, 3> current{};
    std::array<float, 3> scores{};
    std::size_t base = 0;
    for (std::size_t a = 0; a < 3; ++a) {
        const auto raw = trials[a].process_explicit(pending.input.controls, 4 * a, &pending.input.source, nullptr, true);
        if (a != 0 && base != trials[a].original_choice) throw Failure{};
        base = trials[a].original_choice;
        current[a] = mix_planned(raw, pending.input);
    }
    for (std::size_t a = 0; a < 3; ++a) {
        scores[a] = frame_cost(current[a], pending.target, previous_ ? &*previous_ : nullptr, older_ ? &*older_ : nullptr);
        if (ahead_input) {
            float minimum = 0.f;
            for (std::size_t b = 0; b < 3; ++b) {
                Vocoder future = trials[a];
                const auto raw = future.process_explicit(ahead_input->controls, 4 * b, &ahead_input->source, nullptr, true);
                const auto pcm = mix_planned(raw, *ahead_input);
                const float c = frame_cost(pcm, *ahead_target, &current[a], previous_ ? &*previous_ : nullptr);
                if (!std::isfinite(c)) throw Failure{};
                if (b == 0 || c < minimum) minimum = c;
            }
            scores[a] += minimum;
        }
        if (!std::isfinite(scores[a])) throw Failure{};
    }
    std::size_t chosen = 0;
    for (std::size_t a = 1; a < 3; ++a)
        if (scores[a] < scores[chosen]) chosen = a;
    constexpr float kMargin = static_cast<float>(0.001 / (0.17 * 0.17));
    if (scores[chosen] >= scores[base / 4] - kMargin || pending.reset) chosen = base / 4;
    state_ = trials[chosen];
    older_ = previous_;
    previous_ = current[chosen];
    return PlanOutput{pending.input.key, current[chosen], state_.source_trace};
}

} // namespace spear::jr_train::voice
