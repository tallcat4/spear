#include "spear/core/synthetic_source.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <fftw3.h>
#include <vector>

using namespace spear;
using namespace std::chrono_literals;

TEST(SyntheticSource, ToneAppearsAtExpectedBin) {
    EventBus ev;
    SyntheticSignal sig;
    sig.tones = {{250e3, 0.5}};
    sig.max_samples = 8192 * 4;
    SyntheticSource src(&ev, sig, 8192);
    RfConfig cfg; cfg.sample_rate = 2e6;
    ASSERT_TRUE(src.configure(cfg));
    src.set_realtime(false);
    auto sub = src.output().subscribe("test", DeliveryPolicy::Lossless, 16);
    src.start();

    std::vector<Delivery> got;
    while (!sub->eos()) if (auto d = sub->pop(200ms)) got.push_back(*d);
    src.stop();
    ASSERT_EQ(got.size(), 5u); // 4 data + EOS
    EXPECT_TRUE(got.back().flags.has(Flag::EndOfStream));
    EXPECT_TRUE(got.front().flags.has(Flag::StartOfBurst));

    // sample index 連続性
    ContinuityChecker c;
    for (auto& d : got) {
        if (d.block.header().sample_count) { EXPECT_FALSE(c.check(d.block.header())); }
    }

    // FFT: 250 kHz @ 2 Msps, N=8192 → bin 1024
    const int N = 8192;
    std::vector<fftwf_complex> in(N), out(N);
    auto plan = fftwf_plan_dft_1d(N, in.data(), out.data(), FFTW_FORWARD, FFTW_ESTIMATE);
    auto s = got[1].block.as<sc16>();
    for (int i = 0; i < N; ++i) { in[i][0] = s[i].real() / 32768.f; in[i][1] = s[i].imag() / 32768.f; }
    fftwf_execute(plan);
    int peak = 0; float pm = 0;
    for (int i = 0; i < N; ++i) { float m = out[i][0] * out[i][0] + out[i][1] * out[i][1]; if (m > pm) { pm = m; peak = i; } }
    fftwf_destroy_plan(plan);
    EXPECT_EQ(peak, 1024);
    EXPECT_NEAR(std::sqrt(pm) / N, 0.5f, 0.01f);
}

TEST(SyntheticSource, ProvidesRadioRxStreamMetadata) {
    EventBus ev;
    SyntheticSource src(&ev, SyntheticSignal{});
    RfConfig cfg; cfg.sample_rate = 4e6; cfg.center_freq = 433.92e6;
    ASSERT_TRUE(src.configure(cfg));
    EXPECT_EQ(src.output().meta().id, std::string(kRadioRxStreamId));
    EXPECT_EQ(src.output().meta().dtype, DataType::ComplexInt16);
    EXPECT_DOUBLE_EQ(src.output().meta().sample_rate, 4e6);
    EXPECT_DOUBLE_EQ(src.output().meta().center_freq, 433.92e6);
}
