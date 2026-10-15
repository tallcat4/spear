// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 受信ビットを条件にした学習済みの利得・スペクトル補正。原本: src/correction.rs。
#pragma once

#include "predictor.hpp"

namespace spear::jr_train::voice {

class Correction {
public:
    void reset() { *this = Correction{}; }
    Controls process(const std::array<uint8_t, 158>& bits, const Controls& input, Numeric& numeric);

private:
    std::array<std::size_t, 4> gain_history_{};
    std::array<std::array<std::size_t, 3>, 4> spectral_history_{};
    std::array<std::size_t, 3> main_history_{};
    float previous_level_ = 0.f;
    bool has_gain_ = false, has_spectrum_ = false;
};

} // namespace spear::jr_train::voice
