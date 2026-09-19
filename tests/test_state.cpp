// State の単一所有 (§4.1): 宣言は Source が 1 か所で持ち、変更 API を通れば即座に config() に現れ、
// state_version が進み、適用は Retune event で報告される。GUI 側のコピーに依存してはならない。
#include "spear/core/lo_correction.hpp"
#include "spear/core/synthetic_source.hpp"

#include <gtest/gtest.h>

using namespace spear;
using namespace std::chrono_literals;

TEST(State, RetuneUpdatesDeclarationImmediatelyAndEmitsEvent) {
    EventBus ev;
    SyntheticSource src(&ev, SyntheticSignal{});
    RfConfig cfg; cfg.sample_rate = 1e6; cfg.center_freq = 100e6;
    ASSERT_TRUE(src.configure(cfg));
    const uint64_t v0 = src.state_version();
    auto sub = src.output().subscribe("t", DeliveryPolicy::LatestOnly, 1);
    src.start();

    // 連続した要求はどれも失われず、宣言は最後の値になる
    EXPECT_TRUE(src.retune(101e6));
    EXPECT_TRUE(src.retune(102e6));
    EXPECT_TRUE(src.retune(103e6));
    EXPECT_DOUBLE_EQ(src.config().center_freq, 103e6);   // 即時
    EXPECT_GT(src.state_version(), v0);
    ev.flush();
    EXPECT_EQ(ev.count(EventKind::Retune), 3u);
    auto hist = ev.history();
    ASSERT_FALSE(hist.empty());
    EXPECT_EQ(hist.front().kind, EventKind::Retune);
    EXPECT_EQ(hist.front().value, 103000000);
    src.stop();
}

TEST(State, ConfigureBumpsVersionAndIsVisibleToReaders) {
    EventBus ev;
    SyntheticSource src(&ev, SyntheticSignal{});
    RfConfig a; a.sample_rate = 2e6; a.center_freq = 50e6;
    ASSERT_TRUE(src.configure(a));
    const uint64_t v1 = src.state_version();
    RfConfig b = a; b.center_freq = 60e6;
    ASSERT_TRUE(src.configure(b));
    EXPECT_GT(src.state_version(), v1);
    EXPECT_DOUBLE_EQ(src.config().center_freq, 60e6);
}

// 個体の LO 誤差(ppm)は Radio が LO 側で打ち消す(lo_correction.hpp)。換算は純関数なのでここで確かめる。
TEST(State, LoCorrectionMapsTrueAndDeviceFrequencies) {
    // この個体: 351.04375 MHz の LO で信号が +1030 Hz 高く見えた → +2.934 ppm
    const LoCorrection c{2.934};
    const double f = 351.04375e6;
    EXPECT_NEAR(c.error_hz(f), 1030.0, 1.0);
    EXPECT_NEAR(c.to_device(f) - f, 1030.0, 1.0);          // 装置には +1030 Hz 高く要求する
    EXPECT_NEAR(c.to_true(c.to_device(f)), f, 1e-6);        // 往復で戻る
    EXPECT_NEAR(c.to_device(1090e6) - 1090e6, 3198.0, 1.0); // 周波数に比例(1090 MHz なら約 3.2 kHz)
    EXPECT_TRUE(c.active());
    const LoCorrection none{};
    EXPECT_FALSE(none.active());
    EXPECT_DOUBLE_EQ(none.to_device(f), f);                 // 0 ppm は恒等(Synthetic / Recording)
    EXPECT_DOUBLE_EQ(none.to_true(f), f);
}
