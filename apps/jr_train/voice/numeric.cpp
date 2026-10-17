// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 原本: src/numeric.rs
#include "numeric.hpp"

#include "common.hpp"

#include <algorithm>
#include <cmath>

namespace spear::jr_train::voice {

void affine(std::span<const float> input, const Tensor& weight, const float* bias, float* output) {
    affine_columns(input, weight, bias, output, 0, weight.cols);
}

void affine_columns(std::span<const float> input, const Tensor& weight, const float* bias, float* output, std::size_t offset, std::size_t columns) {
    std::fill(output, output + columns, 0.f);
    if (weight.sparse()) {
        for (std::size_t j = 0; j < columns; ++j) {
            const std::size_t column = offset + j;
            float sum = 0.f;
            for (std::size_t p = weight.sparse_offsets[column]; p < weight.sparse_offsets[column + 1]; ++p)
                sum += input[weight.sparse_rows[p]] * static_cast<float>(weight.quantized[p]);
            output[j] = sum * weight.quantum;
        }
    } else if (weight.is_quantized()) {
        for (std::size_t j = 0; j < columns; ++j) {
            float sum = 0.f;
            for (std::size_t i = 0; i < weight.rows; ++i) sum += input[i] * static_cast<float>(weight.quantized[i * weight.cols + offset + j]);
            output[j] = sum * weight.quantum;
        }
    } else {
        for (std::size_t i = 0; i < weight.rows; ++i)
            for (std::size_t j = 0; j < columns; ++j) output[j] += input[i] * weight.data[i * weight.cols + offset + j];
    }
    if (bias)
        for (std::size_t j = 0; j < columns; ++j) output[j] += bias[j];
}

void normalize(std::span<const float> input, const Tensor& mean, const Tensor& scale, float* output, bool clip) {
    for (std::size_t i = 0; i < mean.rows; ++i) {
        const float x = (input[i] - mean.get(i)) / scale.get(i);
        output[i] = clip ? rs::clamp(x, -6.f, 6.f) : x;
    }
}

void normalize_in_place(float* input, const Tensor& mean, const Tensor& scale, bool clip) {
    for (std::size_t i = 0; i < mean.rows; ++i) {
        const float x = (input[i] - mean.get(i)) / scale.get(i);
        input[i] = clip ? rs::clamp(x, -6.f, 6.f) : x;
    }
}

float round_even(float value) {
    const float lower = std::floor(value);
    const float fraction = value - lower;
    if (fraction < 0.5f || (fraction == 0.5f && std::fmod(lower, 2.f) == 0.f)) return lower;
    return lower + 1.f;
}

int16_t sample(float value) { return rs::as<int16_t>(rs::clamp(round_even(value * 32768.f), -32768.f, 32767.f)); }

namespace {
struct Root {
    float re, im;
};
const std::array<Root, 256>& rotations() {
    static const std::array<Root, 256> roots = [] {
        std::array<Root, 256> r{};
        for (std::size_t i = 0; i < 256; ++i) {
            const float angle = rs::at_runtime(-2.f * kPi * static_cast<float>(i) / 512.f);
            r[i] = {std::cos(angle), std::sin(angle)};
        }
        return r;
    }();
    return roots;
}
} // namespace

void Numeric::fft(bool inverse) {
    const auto& roots = rotations();
    std::size_t j = 0;
    for (std::size_t i = 1; i < 512; ++i) {
        std::size_t bit = 256;
        while (j & bit) {
            j ^= bit;
            bit >>= 1;
        }
        j ^= bit;
        if (i < j) std::swap(work_[i], work_[j]);
    }
    for (std::size_t size = 2; size <= 512; size <<= 1) {
        for (std::size_t k = 0; k < size / 2; ++k) {
            Root rotation = roots[k * (512 / size)];
            if (inverse) rotation.im = -rotation.im;
            for (std::size_t offset = 0; offset < 512; offset += size) {
                const Complex even = work_[offset + k];
                const Complex value = work_[offset + k + size / 2];
                const Complex odd{value.re * rotation.re - value.im * rotation.im, value.re * rotation.im + value.im * rotation.re};
                work_[offset + k] = {even.re + odd.re, even.im + odd.im};
                work_[offset + k + size / 2] = {even.re - odd.re, even.im - odd.im};
            }
        }
    }
    if (inverse)
        for (auto& v : work_) {
            v.re /= 512.f;
            v.im /= 512.f;
        }
}

void Numeric::symmetric_spectrum(const float* cepstrum) {
    work_.fill(Complex{});
    for (std::size_t i = 0; i < 32; ++i) {
        work_[i + 1].re = cepstrum[i];
        work_[511 - i].re = cepstrum[i];
    }
    fft(false);
}

void Numeric::bound_correction(float* correction) {
    for (std::size_t i = 0; i < 32; ++i) correction[i] = rs::clamp(correction[i], -0.25f, 0.25f);
    symmetric_spectrum(correction);
    float peak = 0.f;
    for (const auto& x : work_) peak = rs::max(peak, std::abs(x.re));
    const float limit = 6.f * std::log(10.f) / 10.f;
    if (peak > limit)
        for (std::size_t i = 0; i < 32; ++i) correction[i] *= limit / peak;
}

void Numeric::lpc_cepstrum(const float* reflection, float* output) {
    std::array<float, 11> denominator{};
    denominator[0] = 1.f;
    for (std::size_t k = 0; k < 10; ++k) {
        const float r = rs::clamp(std::tanh(reflection[k]), -0.96f, 0.96f);
        std::array<float, 11> next{};
        std::copy(denominator.begin(), denominator.begin() + static_cast<std::ptrdiff_t>(k + 1), next.begin());
        for (std::size_t i = 0; i <= k; ++i) next[i + 1] += r * denominator[k - i];
        denominator = next;
    }
    for (std::size_t n = 1; n <= 32; ++n) {
        float value = n <= 10 ? -denominator[n] : 0.f;
        for (std::size_t k = 1; k <= std::min<std::size_t>(n - 1, 10); ++k)
            value -= (1.f - static_cast<float>(k) / static_cast<float>(n)) * denominator[k] * output[n - k - 1];
        output[n - 1] = value;
    }
}

bool Numeric::envelope_impulse(const float* shape, std::array<float, 256>& output) {
    std::array<float, 32> bounded{};
    float absolute_bound = 0.f;
    for (std::size_t i = 0; i < 32; ++i) {
        bounded[i] = rs::clamp(shape[i], -30.f, 30.f);
        absolute_bound += 2.f * std::abs(bounded[i]);
    }
    if (absolute_bound <= 29.f) {
        std::array<float, 32> weighted{};
        for (std::size_t k = 0; k < 32; ++k) weighted[k] = static_cast<float>(k + 1) * bounded[k];
        output[0] = 1.f;
        float norm = 1.f;
        for (std::size_t n = 1; n < 256; ++n) {
            float sum = 0.f;
            for (std::size_t k = 1; k <= std::min<std::size_t>(n, 32); ++k) sum += weighted[k - 1] * output[n - k];
            output[n] = sum / static_cast<float>(n);
            norm += output[n] * output[n];
        }
        norm = std::sqrt(norm);
        if (std::isfinite(norm) && norm > 0.f) {
            for (auto& x : output) x /= norm;
            return true;
        }
    }
    if (absolute_bound <= 29.f) {
        work_.fill(Complex{});
        for (std::size_t i = 0; i < 32; ++i) work_[i + 1].re = bounded[i];
    } else {
        symmetric_spectrum(bounded.data());
        for (auto& v : work_) v = Complex{rs::clamp(v.re, -30.f, 30.f), 0.f};
        fft(true);
        work_[0] = Complex{};
        for (std::size_t i = 1; i < 256; ++i) work_[i].im = 0.f;
        work_[256] = Complex{work_[256].re * 0.5f, 0.f};
        std::fill(work_.begin() + 257, work_.end(), Complex{});
    }
    fft(false);
    for (auto& v : work_) {
        const float radius = std::exp(v.re);
        v = Complex{radius * std::cos(v.im), radius * std::sin(v.im)};
    }
    fft(true);
    float norm = 0.f;
    for (std::size_t i = 0; i < 256; ++i) {
        output[i] = work_[i].re;
        norm += output[i] * output[i];
    }
    norm = std::sqrt(norm);
    if (!std::isfinite(norm) || norm <= 0.f) return false;
    for (auto& x : output) x /= norm;
    return true;
}

} // namespace spear::jr_train::voice
