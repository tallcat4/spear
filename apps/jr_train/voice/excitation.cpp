// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 原本: src/excitation.rs。Iterator::sum の初期値は -0.0(Rust 1.8x 以降の浮動小数の Sum)、fold は明示の初期値。
#include "excitation.hpp"

#include "common.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace spear::jr_train::voice {
namespace {

constexpr double kPi64 = std::numbers::pi;

struct SpectrumConstants {
    std::array<double, 160> window{};
    double window_power = 0;
    std::array<std::array<double, 257>, 20> bank{};
    std::array<std::array<double, 2>, 256> rotation{};
};

const SpectrumConstants& spectrum() {
    static const SpectrumConstants c = [] {
        SpectrumConstants s;
        for (std::size_t i = 0; i < 160; ++i) s.window[i] = 0.5 - 0.5 * std::cos(rs::at_runtime(2.0 * kPi64 * static_cast<double>(i) / 159.0));
        std::array<double, 22> edges{};
        for (std::size_t i = 0; i < 22; ++i) edges[i] = static_cast<double>(i) * (4000.0 / 21.0);
        for (std::size_t b = 0; b < 20; ++b) {
            for (std::size_t k = 0; k < 257; ++k) {
                const double f = static_cast<double>(k) * (8000.0 / 512.0);
                s.bank[b][k] = rs::max(rs::min((f - edges[b]) / (edges[b + 1] - edges[b]), (edges[b + 2] - f) / (edges[b + 2] - edges[b + 1])), 0.0);
            }
            double sum = -0.0;
            for (double x : s.bank[b]) sum += x;
            for (double& x : s.bank[b]) x /= sum;
        }
        for (std::size_t i = 0; i < 256; ++i) {
            const double t = rs::at_runtime(-2.0 * kPi64 * static_cast<double>(i) / 512.0);
            s.rotation[i] = {std::cos(t), std::sin(t)};
        }
        double wp = -0.0;
        for (double x : s.window) wp += x * x;
        s.window_power = wp;
        return s;
    }();
    return c;
}

std::size_t reverse9(std::size_t i) {
    std::size_t r = 0;
    for (int b = 0; b < 9; ++b) r |= ((i >> b) & 1) << (8 - b);
    return r;
}

double rms_of(const double* x, std::size_t n, double floor) {
    double s = -0.0;
    for (std::size_t i = 0; i < n; ++i) s += x[i] * x[i];
    return rs::max(std::sqrt(s / 80.0), floor);
}

} // namespace

void fft(Bins& values) {
    const auto& c = spectrum();
    for (std::size_t i = 0; i < 512; ++i) {
        const std::size_t j = reverse9(i);
        if (i < j) std::swap(values[i], values[j]);
    }
    for (int sh = 1; sh <= 9; ++sh) {
        const std::size_t size = std::size_t{1} << sh;
        for (std::size_t begin = 0; begin < 512; begin += size) {
            for (std::size_t k = 0; k < size / 2; ++k) {
                const auto w = c.rotation[k * 512 / size];
                const auto b = values[begin + k + size / 2];
                const std::array<double, 2> v = {b[0] * w[0] - b[1] * w[1], b[0] * w[1] + b[1] * w[0]};
                const auto a = values[begin + k];
                values[begin + k] = {a[0] + v[0], a[1] + v[1]};
                values[begin + k + size / 2] = {a[0] - v[0], a[1] - v[1]};
            }
        }
    }
}

std::array<std::array<double, 22>, 2> describe(const std::array<double, 320>& audio) {
    const auto& c = spectrum();
    std::array<std::array<double, 22>, 2> out{};
    for (std::size_t h = 0; h < 2; ++h) {
        Bins bins{};
        for (std::size_t i = 0; i < 160; ++i) bins[i][0] = audio[h * 160 + i] * c.window[i];
        fft(bins);
        std::array<double, 257> power{};
        for (std::size_t i = 0; i < 257; ++i) power[i] = (bins[i][0] * bins[i][0] + bins[i][1] * bins[i][1]) / c.window_power;
        for (std::size_t b = 0; b < 20; ++b) {
            double sum = -0.0;
            for (std::size_t i = 0; i < 257; ++i) sum += power[i] * c.bank[b][i];
            out[h][b] = 10.0 * std::log10(rs::max(sum, 1e-12));
        }
        double mean = -0.0;
        for (std::size_t b = 0; b < 20; ++b) mean += out[h][b];
        mean /= 20.0;
        for (std::size_t b = 0; b < 20; ++b) out[h][b] -= mean;
        for (std::size_t b = 0; b < 2; ++b) {
            const std::size_t offset = h * 160 + b * 80;
            double energy = -0.0;
            for (std::size_t i = 0; i < 80; ++i) energy += audio[offset + i] * audio[offset + i];
            energy /= 80.0;
            out[h][20 + b] = 10.0 * std::log10(rs::max(energy, 1e-12));
        }
    }
    return out;
}

std::array<std::array<float, 22>, 2> pcm_descriptor(const PcmFrame& pcm) {
    std::array<double, 320> x{};
    for (std::size_t i = 0; i < 320; ++i) x[i] = static_cast<double>(pcm[i]) / 32768.0;
    const auto d = describe(x);
    std::array<std::array<float, 22>, 2> out{};
    for (std::size_t h = 0; h < 2; ++h)
        for (std::size_t i = 0; i < 22; ++i) out[h][i] = static_cast<float>(d[h][i]);
    return out;
}

double periodicity(const PcmFrame& pcm) {
    double mean = -0.0;
    for (int16_t v : pcm) mean += static_cast<double>(v);
    mean /= 320.0;
    std::array<double, 320> x{};
    for (std::size_t i = 0; i < 320; ++i) x[i] = static_cast<double>(pcm[i]) - mean;
    double peak = -1.0;
    for (std::size_t lag = 20; lag <= 160; ++lag) {
        double dot = 0.0, a = 0.0, b = 0.0;
        for (std::size_t i = 0; i < 320 - lag; ++i) {
            dot += x[i] * x[i + lag];
            a += x[i] * x[i];
            b += x[i + lag] * x[i + lag];
        }
        peak = rs::max(peak, rs::clamp(dot / std::sqrt(rs::max(a * b, 1e-30)), -1.0, 1.0));
    }
    return peak;
}

Components Sources::components(const std::array<int16_t, 320>& raw, const Controls& controls) {
    std::array<double, 320> normalized{};
    for (std::size_t i = 0; i < 320; ++i) normalized[i] = static_cast<double>(raw[i]);
    for (std::size_t b = 0; b < 4; ++b) {
        double* x = normalized.data() + b * 80;
        const double rms = rms_of(x, 80, 1e-12);
        for (std::size_t i = 0; i < 80; ++i) x[i] /= rms;
    }
    std::array<double, 11> r{};
    for (std::size_t lag = 0; lag <= 10; ++lag) {
        double s = -0.0;
        for (std::size_t i = 0; i < 320 - lag; ++i) s += normalized[i] * normalized[i + lag];
        r[lag] = s / 320.0;
    }
    r[0] = rs::max(1.01 * r[0], 1e-8);
    std::array<std::array<double, 11>, 10> matrix{};
    for (std::size_t i = 0; i < 10; ++i) {
        for (std::size_t j = 0; j < 10; ++j) matrix[i][j] = r[i > j ? i - j : j - i];
        matrix[i][10] = -r[i + 1];
    }
    for (std::size_t k = 0; k < 10; ++k) {
        // Iterator::max_by: 同値なら最後の要素
        std::size_t pivot = k;
        for (std::size_t a = k + 1; a < 10; ++a)
            if (!rs::total_lt(std::abs(matrix[a][k]), std::abs(matrix[pivot][k]))) pivot = a;
        std::swap(matrix[k], matrix[pivot]);
        for (std::size_t i = k + 1; i < 10; ++i) {
            const double ratio = matrix[i][k] / matrix[k][k];
            for (std::size_t j = k; j <= 10; ++j) matrix[i][j] -= ratio * matrix[k][j];
        }
    }
    std::array<double, 10> coef{};
    for (std::size_t i = 10; i-- > 0;) {
        double s = -0.0;
        for (std::size_t j = i + 1; j < 10; ++j) s += matrix[i][j] * coef[j];
        coef[i] = (matrix[i][10] - s) / matrix[i][i];
    }
    for (std::size_t k = 0; k < 10; ++k) coef[k] *= rs::powi_const(0.98, static_cast<int>(k) + 1);
    std::array<double, 330> extended{};
    std::copy(raw_past_.begin(), raw_past_.end(), extended.begin());
    std::copy(normalized.begin(), normalized.end(), extended.begin() + 10);
    std::copy(normalized.begin() + 310, normalized.end(), raw_past_.begin());
    Components out;
    auto& white64 = out.white64;
    for (std::size_t i = 0; i < 320; ++i) {
        white64[i] = normalized[i];
        for (std::size_t k = 0; k < 10; ++k) white64[i] += coef[k] * extended[i + 9 - k];
    }
    for (std::size_t b = 0; b < 4; ++b) {
        double* x = white64.data() + b * 80;
        const double rms = rms_of(x, 80, 1e-12);
        for (std::size_t i = 0; i < 80; ++i) x[i] /= rms;
    }
    for (std::size_t i = 0; i < 320; ++i) out.native[i] = static_cast<float>(white64[i]);
    std::array<double, 800> history{};
    for (std::size_t i = 0; i < 480; ++i) history[i] = static_cast<double>(white_past_[i]);
    for (std::size_t i = 0; i < 320; ++i) history[480 + i] = static_cast<double>(out.native[i]);
    std::array<double, 320> periodic64{};
    for (std::size_t i = 0; i < 320; ++i) periodic64[i] = static_cast<double>(out.native[i]);
    for (std::size_t i = 0; i < 320; ++i) {
        const double period = rs::clamp(static_cast<double>(controls[74 + i / 80]), 20.0, 160.0);
        for (int repeat = 1; repeat <= 3; ++repeat) {
            const double position = static_cast<double>(i) - static_cast<double>(repeat) * period;
            double delayed;
            if (position < -static_cast<double>(valid_)) {
                delayed = static_cast<double>(out.native[i]);
            } else {
                const double left = std::floor(position);
                const double fraction = position - left;
                const auto index = rs::as<std::size_t>(480.0 + left);
                delayed = (1.0 - fraction) * history[index] + fraction * history[index + 1];
            }
            periodic64[i] += rs::powi_const(0.7, repeat) * delayed;
        }
    }
    for (std::size_t b = 0; b < 4; ++b) {
        double* x = periodic64.data() + b * 80;
        const double rms = rms_of(x, 80, 1e-4);
        for (std::size_t i = 0; i < 80; ++i) x[i] /= rms;
    }
    for (std::size_t i = 0; i < 480; ++i) white_past_[i] = static_cast<float>(history[320 + i]);
    valid_ = std::min<std::size_t>(valid_ + 320, 480);
    for (std::size_t i = 0; i < 320; ++i) out.periodic[i] = static_cast<float>(periodic64[i]);
    for (std::size_t i = 0; i < 320; ++i) out.noise[i] = noise_.uniform();
    return out;
}

std::array<float, 320> Sources::mix(const Controls& c, const std::array<std::array<float, 40>, 2>& raw, const Components& parts) {
    std::array<float, 320> out{};
    for (std::size_t b = 0; b < 4; ++b) {
        const double mapped = 1.0 - rs::powi(1.0 - rs::clamp(static_cast<double>(c[70 + b]), 0.0, 1.0), 4);
        std::array<double, 3> prior = {0.9, (1.0 - 0.9) * mapped, (1.0 - 0.9) * (1.0 - mapped)};
        for (auto& p : prior) p = rs::max(p, 1e-5);
        double sum = -0.0;
        for (double p : prior) sum += p;
        std::array<double, 3> logits{};
        for (std::size_t j = 0; j < 3; ++j)
            logits[j] = static_cast<double>(raw[b / 2][34 + 3 * (b % 2) + j]) + static_cast<double>(static_cast<float>(std::log(prior[j] / sum)));
        double maximum = -std::numeric_limits<double>::infinity();
        for (double v : logits) maximum = rs::max(maximum, v);
        for (auto& v : logits) v = std::exp(v - maximum);
        double total = -0.0;
        for (double v : logits) total += v;
        std::array<double, 3> mass{};
        for (std::size_t j = 0; j < 3; ++j) {
            const double v = logits[j] / total;
            mass[j] = mass_ ? 0.5 * (*mass_)[j] + 0.5 * v : v;
        }
        mass_ = mass;
        std::array<double, 3> weights{};
        for (std::size_t j = 0; j < 3; ++j) weights[j] = std::sqrt(rs::max(mass[j], 1e-12));
        std::array<double, 80> mixed{};
        for (std::size_t i = 0; i < 80; ++i) {
            const std::size_t n = b * 80 + i;
            mixed[i] = (static_cast<double>(parts.native[n]) * weights[0] + static_cast<double>(parts.periodic[n]) * weights[1]) + static_cast<double>(parts.noise[n]) * weights[2];
        }
        const double rms = rms_of(mixed.data(), 80, 1e-4);
        for (std::size_t i = 0; i < 80; ++i) out[b * 80 + i] = static_cast<float>(mixed[i] / rms);
    }
    return out;
}

std::array<std::array<float, 12>, 2> geometry(const Components& parts) {
    const std::array<const std::array<float, 320>*, 3> signals = {&parts.native, &parts.periodic, &parts.noise};
    std::array<std::array<float, 12>, 2> out{};
    for (std::size_t b = 0; b < 4; ++b) {
        std::array<double, 3> power{};
        for (std::size_t j = 0; j < 3; ++j) {
            double s = -0.0;
            for (std::size_t i = 0; i < 80; ++i) {
                const double v = static_cast<double>((*signals[j])[b * 80 + i]);
                s += v * v;
            }
            power[j] = s / 80.0;
        }
        const std::array<std::pair<std::size_t, std::size_t>, 3> pairs = {{{0, 1}, {0, 2}, {1, 2}}};
        for (std::size_t k = 0; k < 3; ++k) {
            const auto [a, cc] = pairs[k];
            double s = -0.0;
            for (std::size_t i = 0; i < 80; ++i) s += static_cast<double>((*signals[a])[b * 80 + i]) * static_cast<double>((*signals[cc])[b * 80 + i]);
            const double dot = s / 80.0;
            out[b / 2][6 * (b % 2) + k] = static_cast<float>(dot / rs::max(std::sqrt(power[a] * power[cc]), 1e-12));
        }
        for (std::size_t j = 0; j < 3; ++j) out[b / 2][6 * (b % 2) + 3 + j] = static_cast<float>(std::log(rs::max(power[j], 1e-8)));
    }
    return out;
}

} // namespace spear::jr_train::voice
