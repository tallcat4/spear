// dsp::Leveler(音声のレベル合わせ)
#include "spear/dsp/leveler.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <vector>

namespace {

using spear::dsp::Leveler;

double rms_db(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double p = 0;
    for (std::size_t i = from; i < to; ++i) p += static_cast<double>(x[i]) * x[i];
    return 10.0 * std::log10(p / static_cast<double>(to - from));
}
double peak(const std::vector<float>& x) {
    double m = 0;
    for (float v : x) m = std::max(m, static_cast<double>(std::abs(v)));
    return m;
}
// 「声」の代わり: 1 kHz の正弦波を 4 Hz の包絡で揺らしたもの(10 ms のどのブロックもゲートより上。目標はゲートより上の所の rms)
std::vector<float> speechlike(double rate, double seconds, double amp) {
    std::vector<float> x(static_cast<std::size_t>(rate * seconds));
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double t = static_cast<double>(i) / rate;
        const double env = 0.6 + 0.4 * std::sin(2 * std::numbers::pi * 4 * t);
        x[i] = static_cast<float>(amp * env * env * std::sin(2 * std::numbers::pi * 1000 * t));
    }
    return x;
}

TEST(Leveler, QuietAndLoudSpeechEndAtTheSameLevel) {
    for (double rate : {8000.0, 12500.0, 48000.0}) {
        double result[2];
        int k = 0;
        for (double amp : {0.03, 0.9}) {   // −36 dBFS 前後 / ほぼフルスケール
            Leveler lv("t", rate);
            auto x = speechlike(rate, 4.0, amp);
            lv.process(x);
            result[k++] = rms_db(x, x.size() / 2, x.size());
            EXPECT_LE(peak(x), std::pow(10.0, -1.0 / 20.0) + 1e-6) << "ceiling (rate " << rate << ")";
        }
        EXPECT_NEAR(result[0], -18.0, 2.0) << rate;
        EXPECT_NEAR(result[1], -18.0, 2.0) << rate;
    }
}

TEST(Leveler, SilenceAndNoiseBelowTheGateAreNotRaised) {
    Leveler lv("t", 8000);
    auto x = speechlike(8000, 2.0, 0.1);
    lv.process(x);
    std::vector<float> quiet(8000 * 3, 0.f);
    for (std::size_t i = 0; i < quiet.size(); ++i) quiet[i] = static_cast<float>(0.001 * std::sin(0.37 * static_cast<double>(i)));   // −63 dBFS
    auto q1 = quiet;
    lv.process(q1);   // 利得の平滑が落ち着く
    const double g = lv.gain_db();
    auto q2 = quiet;
    lv.process(q2);
    EXPECT_NEAR(lv.gain_db(), g, 0.01) << "gain holds across the pause";
    EXPECT_LT(g, 24.0 - 1.0) << "not pushed to the maximum by the noise";
    EXPECT_LT(rms_db(q2, 0, q2.size()), -40.0);
}

TEST(Leveler, DelayIsTheDeclaredLookaheadAndLengthIsKept) {
    Leveler lv("t", 8000);
    const auto d = static_cast<std::size_t>(lv.info().group_delay_in);
    EXPECT_EQ(d, 24u);   // 3 ms
    std::vector<float> x(100, 0.f);
    x[10] = 0.01f;
    lv.process(x);
    EXPECT_EQ(x.size(), 100u);
    for (std::size_t i = 0; i < x.size(); ++i)
        if (i != 10 + d) EXPECT_EQ(x[i], 0.f) << i;
    EXPECT_NE(x[10 + d], 0.f);
}

} // namespace
