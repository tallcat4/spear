// App SDK の検証: 各 App が SyntheticSource 上で start → 出力 → stop できること。
#include "spear/appfw/app.hpp"
#include "spear/core/synthetic_source.hpp"
#include "demod_app.hpp"
#include "recorder_app.hpp"
#include "spectrum_app.hpp"
#include "std_t98_app.hpp"
#include "template_app.hpp"

#include <QCoreApplication>
#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <thread>
#include <fftw3.h>

using namespace spear;
using namespace std::chrono_literals;

namespace {
struct Rig {
    Core core{"."};
    Rig() {
        SyntheticSignal sig; sig.tones = {{100e3, 0.3}};
        auto src = std::make_unique<SyntheticSource>(&core.events(), sig);
        core.set_source(std::move(src));
    }
    RfConfig cfg() { RfConfig c; c.sample_rate = 2e6; c.center_freq = 100e6; return c; }
};
} // namespace

TEST(Apps, RegistryListsSpectrumAndRecorder) {
    const auto& apps = appfw::registered_apps();
    ASSERT_GE(apps.size(), 2u);
    EXPECT_EQ(apps[0].info.id, "spectrum");
    EXPECT_EQ(apps[1].info.id, "recorder");
    EXPECT_NE(apps[0].info.page_url.find("SpectrumPage.qml"), std::string::npos);
}

TEST(Apps, SpectrumProducesViewFrames) {
    Rig rig;
    auto app = std::make_shared<apps::SpectrumApp>(appfw::AppInfo{"spectrum", "SPECTRUM", "", Direction::RX, ""});
    app->set_rf_config(rig.cfg());
    std::string err;
    ASSERT_TRUE(rig.core.run_app(app, &err)) << err;
    EXPECT_TRUE(app->running());
    std::this_thread::sleep_for(600ms);
    ASSERT_NE(app->view()->processor(), nullptr);
    EXPECT_GT(app->view()->processor()->rows_written(), 5u);
    appfw::SpectrumFrame f;
    EXPECT_TRUE(app->view()->processor()->latest(f, 0));
    EXPECT_NEAR(f.peak_offset_hz, 100e3, 2e6 / 1024);
    // retune は Core の状態を即時に変え、Retune event を出す(App はコピーを持たない)
    app->tune(101e6);
    EXPECT_DOUBLE_EQ(rig.core.source().config().center_freq, 101e6);
    rig.core.stop_app();
    EXPECT_FALSE(app->running());
    EXPECT_EQ(app->view()->processor(), nullptr);
    EXPECT_EQ(rig.core.rx().consumer_count(), 0u) << "stop 後に consumer が残ってはならない";
}

TEST(Apps, RecorderWritesSigmfWithoutDrops) {
    Rig rig;
    const auto dir = (std::filesystem::temp_directory_path() / "spear_app_test_rec").string();
    std::filesystem::remove_all(dir);
    auto app = std::make_shared<apps::RecorderApp>(appfw::AppInfo{"recorder", "IQ RECORDER", "", Direction::RX, ""});
    QVariantMap settings; settings["record_dir"] = QString::fromStdString(dir);
    app->configure(settings);
    app->set_rf_config(rig.cfg());
    ASSERT_TRUE(rig.core.run_app(app));
    app->record();
    ASSERT_TRUE(app->recording());
    std::this_thread::sleep_for(500ms);
    app->stopRecording();
    EXPECT_GT(app->samplesWritten(), 100000.0);
    EXPECT_EQ(app->droppedBlocks(), 0.0);
    EXPECT_TRUE(std::filesystem::exists(app->path().toStdString() + ".sigmf-data"));
    EXPECT_TRUE(std::filesystem::exists(app->path().toStdString() + ".spear.json"));
    rig.core.stop_app();
    EXPECT_EQ(rig.core.rx().consumer_count(), 0u);
}

TEST(Apps, TemplateLifecycle) {
    Rig rig;
    auto app = std::make_shared<apps::TemplateApp>(appfw::AppInfo{"template", "TEMPLATE", "", Direction::RX, ""});
    app->set_rf_config(rig.cfg());
    ASSERT_TRUE(rig.core.run_app(app));
    std::this_thread::sleep_for(300ms);
    EXPECT_GT(app->blocksSeen(), 0.0);
    rig.core.stop_app();
    EXPECT_EQ(rig.core.rx().consumer_count(), 0u);
}

// FM/AM: 合成 FM トーン(偏移 2.5 kHz, 変調 1 kHz)→ NFM 復調 → 音声 stream に 1 kHz のトーンが出ること。
// 音声は App 内部の Stream Bus の stream から取る(AudioSink は ALSA "null" device)。
TEST(Apps, DemodRecoversFmTone) {
    Core core(".");
    SyntheticSignal sig;
    sig.fm = {250e3, 0.5, 2.5e3, 1e3, true};   // 合成源の中心から +250 kHz(= App の既定 LO offset)
    sig.noise_amplitude = 0.002;
    core.set_source(std::make_unique<SyntheticSource>(&core.events(), sig));
    auto app = std::make_shared<apps::DemodApp>(appfw::AppInfo{"demod", "FM / AM RX", "", Direction::RX, ""});
    QVariantMap settings; settings["audio_device"] = "null";
    app->configure(settings);
    RfConfig c; c.center_freq = 100e6; c.sample_rate = 1e6;   // App が 1.92 Msps に上書きする
    app->set_rf_config(c);
    app->setMode("NFM");
    app->setSquelchDb(-90);
    ASSERT_TRUE(core.run_app(app));
    EXPECT_DOUBLE_EQ(core.source().config().sample_rate, 1.92e6);
    // 宣言 100 MHz は RX 周波数。LO は 250 kHz 下に置かれ、RX 周波数は導出値として一致する
    EXPECT_DOUBLE_EQ(core.source().config().center_freq, 100e6 - 250e3);
    EXPECT_DOUBLE_EQ(app->rxFreq(), 100e6);
    auto sub = app->audio_stream().subscribe("test", DeliveryPolicy::Lossless, 256);
    std::vector<float> audio;
    const auto t0 = std::chrono::steady_clock::now();
    while (audio.size() < 48000 && std::chrono::steady_clock::now() - t0 < 5s) {
        if (auto d = sub->pop(200ms)) { auto s = d->block.as<float>(); audio.insert(audio.end(), s.begin(), s.end()); }
    }
    ASSERT_GE(audio.size(), 48000u);
    EXPECT_TRUE(app->squelchOpen());
    EXPECT_GT(app->signalDb(), -20.0);
    // 後半 32768 sample で FFT → 1 kHz が最大
    const int N = 32768;
    std::vector<fftwf_complex> in(N), out(N);
    auto plan = fftwf_plan_dft_1d(N, in.data(), out.data(), FFTW_FORWARD, FFTW_ESTIMATE);
    for (int i = 0; i < N; ++i) { in[i][0] = audio[audio.size() - N + i]; in[i][1] = 0; }
    fftwf_execute(plan);
    int pk = 1; float pm = 0;
    for (int i = 1; i < N / 2; ++i) { const float m = out[i][0] * out[i][0] + out[i][1] * out[i][1]; if (m > pm) { pm = m; pk = i; } }
    fftwf_destroy_plan(plan);
    const double peak_hz = pk * 48000.0 / N;
    EXPECT_NEAR(peak_hz, 1000.0, 48000.0 / N * 2);
    // 音声振幅: 偏移 2.5 kHz = fm_deviation なので約 1.0 → RMS ≈ 0.7(de-emphasis なし)
    double rms = 0; for (int i = 0; i < N; ++i) rms += audio[audio.size() - N + i] * audio[audio.size() - N + i];
    rms = std::sqrt(rms / N);
    EXPECT_GT(rms, 0.3);
    // Provenance: 搬送波検出 event が入力 index 範囲を持つ
    core.events().flush();
    bool carrier_event = false;
    for (const auto& e : core.events().history()) if (e.source == "demod" && e.detail == "carrier detected") { carrier_event = true; EXPECT_GE(e.range.end, 0u); }
    EXPECT_TRUE(carrier_event);
    EXPECT_EQ(app->losslessDrops(), 0.0);
    core.stop_app();
    EXPECT_EQ(core.rx().consumer_count(), 0u);
}

int main(int argc, char** argv) {
    QCoreApplication qapp(argc, argv);   // QObject timer のため
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

TEST(Apps, StdT98LifecycleOnSynthetic) {
    // 30ch 受信機を合成源(信号なし)で start → 観測点が動く → stop。実信号の検証は apps/std_t98/tests(実録音 golden)
    Core core(".");
    SyntheticSignal sig; sig.tones = {{-93750.0 + 250e3, 0.05}};   // ch1 の位置(帯域中心 − 15 ch、LO は −250 kHz)に無変調トーン
    sig.noise_amplitude = 0.002;
    core.set_source(std::make_unique<SyntheticSource>(&core.events(), sig));
    auto app = std::make_shared<apps::StdT98App>(appfw::AppInfo{"std_t98", "STD-T98 MONITOR", "", Direction::RX, ""});
    QVariantMap settings; settings["audio_device"] = "null"; settings["std_t98.freq_err_hz"] = 0.0;
    app->configure(settings);
    RfConfig c; c.center_freq = 100e6; c.sample_rate = 1e6;   // App が 4 Msps / 帯域中心 − 250 kHz に上書きする
    app->set_rf_config(c);
    ASSERT_TRUE(core.run_app(app));
    EXPECT_DOUBLE_EQ(core.source().config().sample_rate, 4e6);
    EXPECT_DOUBLE_EQ(core.source().config().center_freq, 351.29375e6 - 250e3);
    std::this_thread::sleep_for(800ms);
    ASSERT_NE(app->bandView()->processor(), nullptr);
    EXPECT_GT(app->bandView()->processor()->rows_written(), 3u) << "帯域の観測点(std_t98.band)が流れていること";
    const auto chans = app->channels();
    ASSERT_EQ(chans.size(), 30);
    EXPECT_GT(chans[0].toMap()["power"].toDouble(), chans[10].toMap()["power"].toDouble() + 20) << "ch1 のトーンがチャネル電力に出る";
    EXPECT_EQ(app->losslessDrops(), 0.0);
    core.stop_app();
    EXPECT_FALSE(app->running());
    EXPECT_EQ(core.rx().consumer_count(), 0u) << "stop 後に consumer が残ってはならない";
}
