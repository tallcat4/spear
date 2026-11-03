// 規格書 7.7.2 の試験信号(spec_vectors.hpp、送信されるシンボルそのもの)を IQ にする試験用の変調器。
// std-t99-receiver の tests/native/test_main.cpp(std-t99-receiver-sharp tests/spec_iq.cpp)から。インパルス → H(f)·P(f) → ±315/±945 Hz。
//
// SPEAR の「合成の変調器は作らない」(docs/DEVELOPMENT.md)との関係: あの規約は、規格に無いパラメータを合わせ込んだ自作の信号を
// golden にしない、という意味(STD-T98 で失敗した)。ここで通すのは規格書が定める試験信号のシンボル列で、整形も規格のフィルタ
// (α = 0.2、偏移 315 Hz/level)なので、受信系を一次資料で確かめる手段として使う。実録音の golden が取れたらそちらを主にする。
#pragma once

#include "spec_vectors.hpp"
#include "std_t98/dsp/rrc.hpp"
#include "spear/core/types.hpp"

#include <cmath>
#include <numbers>
#include <random>
#include <string>
#include <vector>

namespace spear::std_t99::spec {

inline std::vector<cf32> modulate(const std::vector<int8_t>& symbols, double rate, double offset_hz, double noise, uint32_t seed) {
    const double fs = 48000, sps = fs / 2400;
    const auto taps = spear::std_t98::make_tx_taps(fs, 2400, 0.2, static_cast<int>(sps) * 24 + 1);
    std::vector<double> level(static_cast<std::size_t>(static_cast<double>(symbols.size()) * sps + static_cast<double>(taps.size())), 0.0);
    for (std::size_t k = 0; k < symbols.size(); ++k)
        for (std::size_t t = 0; t < taps.size(); ++t) level[static_cast<std::size_t>(static_cast<double>(k) * sps) + t] += symbols[k] * sps * taps[t];
    std::mt19937 rng(seed);
    std::normal_distribution<float> gauss(0.f, static_cast<float>(noise));
    std::vector<cf32> iq(static_cast<std::size_t>(static_cast<double>(level.size()) / fs * rate));
    double phase = 0;
    for (std::size_t i = 0; i < iq.size(); ++i) {
        const double x = static_cast<double>(i) * fs / rate;
        const auto j = static_cast<std::size_t>(x);
        const double mu = x - static_cast<double>(j);
        const double f = (j + 1 < level.size() ? level[j] + mu * (level[j + 1] - level[j]) : 0.0) * 315.0 + offset_hz;
        phase += 2 * std::numbers::pi * f / rate;
        if (phase > std::numbers::pi) phase -= 2 * std::numbers::pi;
        iq[i] = cf32(0.5f * static_cast<float>(std::cos(phase)) + gauss(rng), 0.5f * static_cast<float>(std::sin(phase)) + gauss(rng));
    }
    return iq;
}

inline std::vector<int8_t> transmission(const std::string& test) {
    std::vector<int8_t> s(48, 0);
    for (int p : {3, 3, 3, -3, 3, -3, 3, 3, -3, -3, -3, 3}) s.push_back(static_cast<int8_t>(p));   // プリアンブル 5775FDh
    for (const auto& v : vectors()) if (test == v.test) s.insert(s.end(), v.symbols.begin(), v.symbols.end());
    s.insert(s.end(), 48, 0);
    return s;
}

// 試験 1-1 6-2 2-1 6-1 を 0.5 s の雑音を挟んで並べる(spec-iq の既定と同じ)。offset_hz: 搬送波の位置(LO からの距離 + ずれ)
inline std::vector<cf32> spec_iq(double rate, double offset_hz, const std::vector<std::string>& tests = {"1-1", "6-2", "2-1", "6-1"}) {
    std::mt19937 rng(5);
    std::normal_distribution<float> gauss(0.f, 0.03f);
    std::vector<cf32> out, gap(static_cast<std::size_t>(rate / 2));
    uint32_t seed = 1;
    for (const auto& test : tests) {
        for (auto& x : gap) x = {gauss(rng), gauss(rng)};
        out.insert(out.end(), gap.begin(), gap.end());
        const auto m = modulate(transmission(test), rate, offset_hz, 0.03, seed++);
        out.insert(out.end(), m.begin(), m.end());
    }
    for (auto& x : gap) x = {gauss(rng), gauss(rng)};
    out.insert(out.end(), gap.begin(), gap.end());
    return out;
}

} // namespace spear::std_t99::spec
