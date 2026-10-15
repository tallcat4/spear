// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 音源の白色化、コヒーレントな履歴、合成用のスペクトル観測。原本: src/excitation.rs。
#pragma once

#include "predictor.hpp"
#include "vocoder.hpp"

#include <array>
#include <optional>

namespace spear::jr_train::voice {

using Bins = std::array<std::array<double, 2>, 512>;
void fft(Bins& values);
std::array<std::array<double, 22>, 2> describe(const std::array<double, 320>& audio);
std::array<std::array<float, 22>, 2> pcm_descriptor(const PcmFrame& pcm);
double periodicity(const PcmFrame& pcm);

struct Components {
    std::array<double, 320> white64{};
    std::array<float, 320> native{}, periodic{}, noise{};
};

class Sources {
public:
    Components components(const std::array<int16_t, 320>& raw, const Controls& controls);
    std::array<float, 320> mix(const Controls& c, const std::array<std::array<float, 40>, 2>& raw, const Components& parts);

private:
    std::array<double, 10> raw_past_{};
    std::array<float, 480> white_past_{};
    std::size_t valid_ = 0;
    VoiceNoise noise_;
    std::optional<std::array<double, 3>> mass_;
};

std::array<std::array<float, 12>, 2> geometry(const Components& parts);

} // namespace spear::jr_train::voice
