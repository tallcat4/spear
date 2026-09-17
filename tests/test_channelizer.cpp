// PFB チャネライザの検証: 単一トーンが対応チャネルに分離される(§8.3 の参照ベクトル)。
#include "spear/dsp/channelizer.hpp"
#include "spear/dsp/fir.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>

using namespace spear;
using namespace spear::dsp;

namespace {
// channel c の総エネルギー
double energy(const std::vector<cf32>& v) {
    double e = 0;
    for (auto x : v) e += std::norm(x);
    return e;
}
}

TEST(PfbChannelizer, SeparatesTones) {
    const std::size_t M = 48;
    const double fs = 48000.0;
    auto proto = design_lowpass(fs, fs / M / 2.0, M * 8 - 1);
    PfbChannelizer ch(M, fs, proto);
    EXPECT_EQ(ch.channels(), M);
    EXPECT_DOUBLE_EQ(ch.out_rate(), 1000.0);

    // それぞれのチャネル中心にトーンを置き、そのチャネルが最大になることを確認
    for (int k : {0, 1, 5, 24, 40, 47}) {
        ch.reset();
        std::vector<cf32> in(M * 400);
        const double f = k * (fs / static_cast<double>(M));
        for (std::size_t n = 0; n < in.size(); ++n) {
            const double ph = 2.0 * std::numbers::pi * f * static_cast<double>(n) / fs;
            in[n] = cf32(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
        }
        std::vector<std::vector<cf32>> outs;
        ch.process(in, outs);
        // 後半(過渡を除く)でエネルギー比較
        std::size_t peak = 0; double pe = 0, total = 0;
        for (std::size_t c = 0; c < M; ++c) {
            std::vector<cf32> tail(outs[c].end() - 100, outs[c].end());
            const double e = energy(tail);
            total += e;
            if (e > pe) { pe = e; peak = c; }
        }
        EXPECT_EQ(static_cast<int>(peak), k) << "tone at bin " << k << " landed in channel " << peak;
        EXPECT_GT(pe / total, 0.9) << "channel " << k << " should hold >90% of energy";
    }
}
