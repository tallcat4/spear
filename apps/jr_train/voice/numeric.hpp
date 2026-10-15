// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// binary32 の計算核。原本: src/numeric.rs。和の順序と乗算・加算ごとの丸めを原本のまま保つ。
#pragma once

#include "models.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>

namespace spear::jr_train::voice {

inline constexpr float kPi = std::numbers::pi_v<float>;

void affine(std::span<const float> input, const Tensor& weight, const float* bias, float* output);
void affine_columns(std::span<const float> input, const Tensor& weight, const float* bias /*nullable*/, float* output, std::size_t offset, std::size_t columns);
void normalize(std::span<const float> input, const Tensor& mean, const Tensor& scale, float* output, bool clip);
void normalize_in_place(float* input, const Tensor& mean, const Tensor& scale, bool clip);
float round_even(float value);
int16_t sample(float value);

class Numeric {
public:
    void bound_correction(float* correction /*32*/);
    void lpc_cepstrum(const float* reflection /*10*/, float* output /*32*/);
    bool envelope_impulse(const float* shape /*32*/, std::array<float, 256>& output);

private:
    struct Complex {
        float re = 0.f, im = 0.f;
    };
    void fft(bool inverse);
    void symmetric_spectrum(const float* cepstrum);
    std::array<Complex, 512> work_{};
};

} // namespace spear::jr_train::voice
