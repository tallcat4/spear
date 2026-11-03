// App SDK の検証: 各 App が SyntheticSource 上で start → 出力 → stop できること。
#include "spear/appfw/app.hpp"
#include "spear/core/sigmf.hpp"
#include "spear/core/synthetic_source.hpp"
#include "adsb_app.hpp"
#include "demod_app.hpp"
#include "jr_train_app.hpp"
#include "recorder_app.hpp"
#include "replay_app.hpp"
#include "spear/core/recording.hpp"
#include "slpr_app.hpp"
#include "spectrum_app.hpp"
#include "std_t98_app.hpp"
#include "std_t99_app.hpp"
#include "template_app.hpp"

#include <QCoreApplication>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
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

TEST(Apps, RegistryListsAllIntegratedApps) {
    const auto& apps = appfw::registered_apps();
    ASSERT_GE(apps.size(), 2u);
    EXPECT_EQ(apps[0].info.id, "spectrum");
    EXPECT_EQ(apps[1].info.id, "recorder");
    EXPECT_NE(apps[0].info.page_url.find("SpectrumPage.qml"), std::string::npos);
    // 統合した試験機能も既存 App と同じ GUI のメニューに 1 回ずつ登録されること。
    for (const auto* id : {"spectrum", "recorder", "replay", "demod", "std_t98", "adsb", "slpr", "jr_train", "std_t99"}) {
        EXPECT_EQ(std::count_if(apps.begin(), apps.end(), [id](const auto& e) { return e.info.id == id; }), 1) << id;
    }
}

// 受信端子はシェルの草案で決まり、App はそのまま Core へ宣言する。center / rate を自分で決める App(DEMOD / STD-T98 / ADS-B)も
// 端子を落としてはならない(落とすと選んだ端子と違う端子で受ける)。
TEST(Apps, EveryAppDeclaresTheDraftPortUnchanged) {
    for (const auto& e : appfw::registered_apps()) {
        auto app = e.make();
        for (const auto& i : all_rf_ports()) {
            RfConfig c; c.sample_rate = 2e6; c.center_freq = 100e6; c.port = i.port;
            app->set_rf_config(c);
            EXPECT_EQ(app->declare_rf_config().port, i.port) << e.info.id << " " << i.name;
        }
    }
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

// 方向は App の定義が決める。送信 App の後の草案(Core の RF の写し = TX)を渡されても、受信 App は RX を宣言する
TEST(Apps, DirectionComesFromAppNotDraft) {
    for (const auto& e : appfw::registered_apps()) {
        auto app = e.make();
        RfConfig c; c.direction = e.info.direction == Direction::RX ? Direction::TX : Direction::RX;
        app->set_rf_config(c);
        EXPECT_EQ(app->declare_rf_config().direction, e.info.direction) << e.info.id;
    }
}

namespace {
struct CaptureSink final : Sink {
    std::string name() const override { return "capture"; }
    void write(const Delivery& d) override {
        std::lock_guard lk(mu);
        const auto s = d.block.as<sc16>();
        samples.insert(samples.end(), s.begin(), s.end());
    }
    std::size_t size() { std::lock_guard lk(mu); return samples.size(); }
    std::mutex mu;
    std::vector<sc16> samples;
};
// 小さな SigMF 録音(ci16、値 = 通し番号)を作る
std::string write_test_recording(const std::string& dir, uint32_t n, double rate, double freq) {
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string base = dir + "/rec_test";
    sigmf::Meta m;
    m.dtype = DataType::ComplexInt16;
    m.sample_rate = rate;
    sigmf::Capture c; c.frequency = freq;
    m.captures.push_back(c);
    m.total_samples = n;
    EXPECT_TRUE(sigmf::write(base, m));
    std::ofstream f(sigmf::data_path(base), std::ios::binary);
    for (uint32_t i = 0; i < n; ++i) { const sc16 v(static_cast<int16_t>(i & 0x7fff), 0); f.write(reinterpret_cast<const char*>(&v), sizeof v); }
    return base;
}
} // namespace

// IQ REPLAY TX: 録音の center / rate で送信を宣言し、TX MASTER が SAFE なら送らず、ARMED なら録音を欠けなく 1 burst で送る
TEST(Apps, ReplaySendsRecordingOnlyWhenArmed) {
    Core core(".");
    auto src = std::make_unique<SyntheticSource>(&core.events(), SyntheticSignal{});
    auto sink = std::make_shared<CaptureSink>();
    src->set_tx_sink(sink);
    src->set_realtime(false);
    core.set_source(std::move(src));
    const auto dir = (std::filesystem::temp_directory_path() / "spear_app_test_replay").string();
    const uint32_t n = 100000;
    const std::string base = write_test_recording(dir, n, 1e6, 422.175e6);
    auto app = std::make_shared<apps::ReplayApp>(appfw::AppInfo{"replay", "IQ REPLAY TX", "", Direction::TX, ""});
    QVariantMap settings; settings["record_dir"] = QString::fromStdString(dir);
    app->configure(settings);
    ASSERT_EQ(app->recordings().size(), 1);
    app->setFile(QString::fromStdString(base));
    ASSERT_TRUE(app->fileValid()) << app->fileError().toStdString();
    RfConfig draft; draft.sample_rate = 2e6; draft.center_freq = 1090e6; draft.gain = 40;   // 受信の草案(送信には使わない)
    app->set_rf_config(draft);
    const RfConfig decl = app->declare_rf_config();
    EXPECT_EQ(decl.direction, Direction::TX);
    EXPECT_DOUBLE_EQ(decl.center_freq, 422.175e6);
    EXPECT_DOUBLE_EQ(decl.sample_rate, 1e6);
    EXPECT_DOUBLE_EQ(decl.tx_atten, kTxAttenFullRangeDb);   // 既定は最小出力
    ASSERT_TRUE(core.run_app(app));
    EXPECT_TRUE(app->configCurrent());
    // SAFE: App は送信を始めない(Core も拒否する)
    app->transmit();
    EXPECT_FALSE(app->sending());
    EXPECT_NE(app->lastError().indexOf("SAFE"), -1);
    // ARMED: 全 sample が順に送られ、burst が閉じる
    core.set_tx_armed(true);
    app->transmit();
    for (int i = 0; i < 400 && (app->sending() || core.tx().stats().in_burst); ++i) std::this_thread::sleep_for(5ms);
    EXPECT_FALSE(app->sending());
    EXPECT_FALSE(core.tx().stats().in_burst);
    EXPECT_EQ(core.tx().stats().bursts, 1u);
    ASSERT_EQ(sink->size(), n);
    for (uint32_t i = 0; i < n; ++i) ASSERT_EQ(sink->samples[i].real(), static_cast<int16_t>(i & 0x7fff)) << i;
    // 録音を変える: App は止めずに Core が構成し直す。適用されるまで TRANSMIT できない
    const std::string base2 = write_test_recording(dir + "/b", 50000, 2e6, 351.29375e6);
    app->setFile(QString::fromStdString(base2));
    EXPECT_FALSE(app->configCurrent());
    std::string err;
    ASSERT_TRUE(core.reconfigure(app, app->declare_rf_config(), &err)) << err;
    EXPECT_TRUE(app->running());
    EXPECT_TRUE(app->configCurrent());
    EXPECT_DOUBLE_EQ(core.source().config().center_freq, 351.29375e6);
    EXPECT_DOUBLE_EQ(core.source().config().sample_rate, 2e6);
    app->transmit();
    for (int i = 0; i < 400 && (app->sending() || core.tx().stats().in_burst); ++i) std::this_thread::sleep_for(5ms);
    EXPECT_EQ(core.tx().stats().sent_samples, 50000u);   // 新しい送信の入口(作り直された TxStream)で送れた
    core.stop_app();
    EXPECT_EQ(core.rx().consumer_count(), 0u);
}

// App を止めずに RF を構成し直す(Core::reconfigure): App は running のまま、観測点は新しいレートの stream に付け直され、consumer は増えない
TEST(Apps, ReconfigureChangesRateWithoutStoppingApp) {
    Rig rig;
    auto app = std::make_shared<apps::SpectrumApp>(appfw::AppInfo{"spectrum", "SPECTRUM", "", Direction::RX, ""});
    app->set_rf_config(rig.cfg());
    ASSERT_TRUE(rig.core.run_app(app));
    std::this_thread::sleep_for(300ms);
    int running_changes = 0;
    QObject::connect(app.get(), &appfw::GuiApp::runningChanged, [&] { ++running_changes; });
    RfConfig c = rig.core.source().config();
    c.sample_rate = 4e6;
    app->set_rf_config(c);
    std::string err;
    ASSERT_TRUE(rig.core.reconfigure(app, app->declare_rf_config(), &err)) << err;
    EXPECT_TRUE(app->running());
    EXPECT_EQ(running_changes, 0);
    EXPECT_EQ(rig.core.active_app(), app);
    EXPECT_DOUBLE_EQ(rig.core.source().config().sample_rate, 4e6);
    EXPECT_DOUBLE_EQ(rig.core.rx().meta().sample_rate, 4e6);   // stream も新しいレートで作り直された
    EXPECT_EQ(rig.core.rx().consumer_count(), 1u);
    const auto rows = app->view()->processor()->rows_written();
    std::this_thread::sleep_for(500ms);
    EXPECT_GT(app->view()->processor()->rows_written(), rows);   // 新しい stream で表示が動いている
    rig.core.stop_app();
    EXPECT_EQ(rig.core.rx().consumer_count(), 0u);
}

// 適用できない構成(送信できない端子)は拒否され、元の構成で App が動き続ける
TEST(Apps, ReconfigureRejectedKeepsPreviousConfig) {
    Rig rig;
    auto app = std::make_shared<apps::SpectrumApp>(appfw::AppInfo{"spectrum", "SPECTRUM", "", Direction::RX, ""});
    app->set_rf_config(rig.cfg());
    ASSERT_TRUE(rig.core.run_app(app));
    RfConfig bad = rig.core.source().config();
    bad.direction = Direction::TX; bad.tx_port = RfPort::RxB;
    std::string err;
    EXPECT_FALSE(rig.core.reconfigure(app, bad, &err));
    EXPECT_FALSE(err.empty());
    EXPECT_TRUE(app->running());
    EXPECT_EQ(rig.core.source().config().direction, Direction::RX);
    EXPECT_DOUBLE_EQ(rig.core.source().config().sample_rate, 2e6);
    EXPECT_EQ(rig.core.rx().consumer_count(), 1u);
    rig.core.stop_app();
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
    sig.fm = {250e3, 0.5, 2.5e3, 1e3, true};   // 合成源の中心(LO)から +250 kHz(= App の既定 LO offset)
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
    // 草案の 100 MHz は LO。RX 周波数は LO + 250 kHz の導出値
    EXPECT_DOUBLE_EQ(core.source().config().center_freq, 100e6);
    EXPECT_DOUBLE_EQ(app->rxFreq(), 100e6 + 250e3);
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
    sub.reset();   // テストの購読が持つ BlockRef を App の pool より先に返す(stop で pool が消える)
    core.stop_app();
    EXPECT_EQ(core.rx().consumer_count(), 0u);
    // 停止後、シェルは草案を Core の RF(LO)に戻して次の起動に渡す。起動し直しても RX 周波数はずれない
    app->set_rf_config(core.source().config());
    EXPECT_DOUBLE_EQ(app->declare_rf_config().center_freq, 100e6);
    EXPECT_DOUBLE_EQ(app->rxFreq(), 100e6 + 250e3);
}

// mode を変えて offset の上限(±(rate/2 − bw))を超えるときは、RX 周波数を保ったまま LO を移す
TEST(Apps, DemodModeChangeKeepsChannelInSpan) {
    auto app = std::make_shared<apps::DemodApp>(appfw::AppInfo{"demod", "FM / AM RX", "", Direction::RX, ""});
    RfConfig c; c.center_freq = 100e6; c.sample_rate = 1.92e6;
    app->set_rf_config(c);
    app->setMode("NFM");
    app->setLoOffsetHz(900e3);                  // NFM の上限 960 − 12.5 = 947.5 kHz 以内
    const double rx = app->rxFreq();
    EXPECT_DOUBLE_EQ(app->channelOffsetHz(), 900e3);
    app->setMode("WFM");                        // WFM の上限 960 − 200 = 760 kHz
    EXPECT_DOUBLE_EQ(app->channelOffsetHz(), 760e3);
    EXPECT_DOUBLE_EQ(app->rxFreq(), rx);
}

int main(int argc, char** argv) {
    QCoreApplication qapp(argc, argv);   // QObject timer のため
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

TEST(Apps, StdT98LifecycleOnSynthetic) {
    // 97ch 受信機(v2.0 登録局、apps/std_t98/channel_plan.hpp)を合成源(信号なし)で start → 観測点が動く → stop。実信号の検証は apps/std_t98/tests(実録音 golden)
    Core core(".");
    SyntheticSignal sig; sig.tones = {{351.2e6 - (351.29375e6 - 500e3), 0.05}};   // CH1(351.2 MHz)の位置に無変調トーン(LO は基準 − 500 kHz)
    sig.noise_amplitude = 0.002;
    core.set_source(std::make_unique<SyntheticSource>(&core.events(), sig));
    auto app = std::make_shared<apps::StdT98App>(appfw::AppInfo{"std_t98", "STD-T98 MONITOR", "", Direction::RX, ""});
    QVariantMap settings; settings["audio_device"] = "null";
    app->configure(settings);
    RfConfig c; c.center_freq = 100e6; c.sample_rate = 1e6;   // App が 4 Msps / 基準 − 500 kHz に上書きする
    app->set_rf_config(c);
    ASSERT_TRUE(core.run_app(app));
    EXPECT_DOUBLE_EQ(core.source().config().sample_rate, 4e6);
    EXPECT_DOUBLE_EQ(core.source().config().center_freq, 351.29375e6 - 500e3);
    std::this_thread::sleep_for(800ms);
    ASSERT_NE(app->bandView()->processor(), nullptr);
    EXPECT_GT(app->bandView()->processor()->rows_written(), 3u) << "帯域の観測点(std_t98.band)が流れていること";
    const auto chans = app->channels();
    ASSERT_EQ(chans.size(), 97);
    EXPECT_GT(chans[0].toMap()["power"].toDouble(), chans[10].toMap()["power"].toDouble() + 20) << "ch1 のトーンがチャネル電力に出る";
    EXPECT_EQ(app->losslessDrops(), 0.0);
    core.stop_app();
    EXPECT_FALSE(app->running());
    EXPECT_EQ(core.rx().consumer_count(), 0u) << "stop 後に consumer が残ってはならない";
}

TEST(Apps, AdsbLifecycleOnSynthetic) {
    // 8 Msps フルレート消費の App を合成源(信号なし)で start → 観測点が動く → stop。実信号の検証は apps/adsb/tests(合成 PPM と実録音 golden)
    Core core(".");
    SyntheticSignal sig; sig.tones = {{2e6, 0.05}};   // 1090 MHz の位置(LO は −2 MHz)に無変調トーン
    sig.noise_amplitude = 0.002;
    core.set_source(std::make_unique<SyntheticSource>(&core.events(), sig));
    auto app = std::make_shared<apps::AdsbApp>(appfw::AppInfo{"adsb", "ADS-B", "", Direction::RX, ""});
    QVariantMap settings; settings["adsb.ref_lat"] = 35.68; settings["adsb.ref_lon"] = 139.77;
    app->configure(settings);
    EXPECT_TRUE(app->hasReference());
    RfConfig c; c.center_freq = 100e6; c.sample_rate = 1e6;   // App が 8 Msps / 1090 MHz − 2 MHz に上書きする
    app->set_rf_config(c);
    ASSERT_TRUE(core.run_app(app));
    EXPECT_DOUBLE_EQ(core.source().config().sample_rate, 8e6);
    EXPECT_DOUBLE_EQ(core.source().config().center_freq, 1090e6 - 2e6);
    std::this_thread::sleep_for(800ms);
    ASSERT_NE(app->view()->processor(), nullptr);
    EXPECT_GT(app->view()->processor()->rows_written(), 3u) << "radio.rx の観測点が流れていること";
    const auto st = app->stats();
    EXPECT_EQ(st["frames"].toDouble(), 0.0) << "無変調トーンからフレームが出てはならない";
    EXPECT_EQ(st["drops"].toDouble(), 0.0);
    EXPECT_EQ(app->aircraft().size(), 0);
    core.stop_app();
    EXPECT_FALSE(app->running());
    EXPECT_EQ(core.rx().consumer_count(), 0u) << "stop 後に consumer が残ってはならない";
}

// 特定小電力 20ch: 合成 NFM(偏移 2.5 kHz、変調 1 kHz)を ch15 の位置に置いて start → ch15 だけ開き、ミックス音声に 1 kHz が出る → stop。
// 音声は App 内部の Stream Bus の stream(slpr.audio、12.5 kHz)から取る(AudioSink は ALSA "null" device)。受信機自体の検証は apps/slpr/tests。
TEST(Apps, SlprReceivesOneChannelOnSynthetic) {
    Core core(".");
    SyntheticSignal sig;
    sig.fm = {62.5e3 + 250e3, 0.3, 2.5e3, 1e3, true};   // ch15(帯域中心 + 62.5 kHz)。LO は帯域中心 − 250 kHz
    sig.noise_amplitude = 0.02;
    core.set_source(std::make_unique<SyntheticSource>(&core.events(), sig));
    auto app = std::make_shared<apps::SlprApp>(appfw::AppInfo{"slpr", "SLPR 20CH RX", "", Direction::RX, ""});
    QVariantMap settings; settings["audio_device"] = "null";
    app->configure(settings);
    RfConfig c; c.center_freq = 100e6; c.sample_rate = 1e6;   // App が 4 Msps / 帯域中心 − 250 kHz に上書きする
    app->set_rf_config(c);
    ASSERT_TRUE(core.run_app(app));
    EXPECT_DOUBLE_EQ(core.source().config().sample_rate, 4e6);
    EXPECT_DOUBLE_EQ(core.source().config().center_freq, 422.175e6 - 250e3);
    EXPECT_DOUBLE_EQ(app->channelFreqHz(14), 422.2375e6);
    auto sub = app->audio_stream().subscribe("test", DeliveryPolicy::Lossless, 256);
    std::vector<float> audio;
    const auto t0 = std::chrono::steady_clock::now();
    while (audio.size() < 12500 && std::chrono::steady_clock::now() - t0 < 6s) {
        if (auto d = sub->pop(200ms)) { auto s = d->block.as<float>(); audio.insert(audio.end(), s.begin(), s.end()); }
    }
    ASSERT_GE(audio.size(), 12500u);
    ASSERT_NE(app->bandView()->processor(), nullptr);
    EXPECT_GT(app->bandView()->processor()->rows_written(), 3u) << "帯域の観測点(slpr.band)が流れていること";
    ASSERT_NE(app->channelView()->processor(), nullptr);
    EXPECT_GT(app->channelView()->processor()->rows_written(), 3u) << "12.5 kHz の観測点(slpr.channel、1 ブロック 51 sample)も流れていること(ViewProcessor が nfft ぶん溜める)";
    const auto chans = app->channels();
    ASSERT_EQ(chans.size(), 20);
    for (int i = 0; i < 20; ++i) EXPECT_EQ(chans[i].toMap()["open"].toBool(), i == 14) << "ch" << i + 1 << " power " << chans[i].toMap()["power"].toDouble();
    EXPECT_EQ(app->openChannels(), 1);
    EXPECT_EQ(chans[14].toMap()["name"].toString(), "15");
    // 後半 4096 sample で FFT → 1 kHz が最大
    const int N = 4096;
    std::vector<fftwf_complex> in(N), out(N);
    auto plan = fftwf_plan_dft_1d(N, in.data(), out.data(), FFTW_FORWARD, FFTW_ESTIMATE);
    for (int i = 0; i < N; ++i) { in[i][0] = audio[audio.size() - N + i]; in[i][1] = 0; }
    fftwf_execute(plan);
    int pk = 1; float pm = 0;
    for (int i = 1; i < N / 2; ++i) { const float m = out[i][0] * out[i][0] + out[i][1] * out[i][1]; if (m > pm) { pm = m; pk = i; } }
    fftwf_destroy_plan(plan);
    EXPECT_NEAR(pk * 12500.0 / N, 1000.0, 12500.0 / N * 2);
    // Provenance: squelch の開 event が入力 index 範囲を持つ
    core.events().flush();
    bool carrier_event = false;
    for (const auto& e : core.events().history()) if (e.source == "slpr" && e.detail == "ch15 carrier") { carrier_event = true; EXPECT_EQ(e.value, 15); }
    EXPECT_TRUE(carrier_event);
    EXPECT_EQ(app->losslessDrops(), 0.0);
    sub.reset();   // テストの購読が持つ BlockRef を App の pool より先に返す(stop で pool が消える)
    core.stop_app();
    EXPECT_FALSE(app->running());
    EXPECT_EQ(core.rx().consumer_count(), 0u) << "stop 後に consumer が残ってはならない";
}

TEST(Apps, JrTrainLifecycleAndRecordingOnSynthetic) {
    // 22 波受信機を合成源(無変調トーンと雑音)で start → 観測点が動く → 録音 → stop。
    // 実信号の検証は apps/jr_train/tests(原本の Rust 実装と同じ結果になること)
    Core core(".");
    SyntheticSignal sig; sig.tones = {{352.6e6 - (352.603125e6 - 250e3), 0.05}};   // 352.6 MHz(D4 V2)の位置。LO は帯域中心 − 250 kHz
    sig.noise_amplitude = 0.002;
    core.set_source(std::make_unique<SyntheticSource>(&core.events(), sig));
    auto app = std::make_shared<apps::JrTrainApp>(appfw::AppInfo{"jr_train", "JR TRAIN RADIO", "", Direction::RX, ""});
    const auto dir = std::filesystem::temp_directory_path() / "spear_jr_train_test";
    std::filesystem::remove_all(dir);
    QVariantMap settings; settings["jr_train.preset"] = "east"; settings["record_dir"] = QString::fromStdString(dir.string()); settings["audio_device"] = "null";
    app->configure(settings);
    RfConfig c; c.center_freq = 100e6; c.sample_rate = 1e6;   // App が 1.92 MS/s / 帯域中心 − 250 kHz に上書きする
    app->set_rf_config(c);
    ASSERT_TRUE(core.run_app(app));
    EXPECT_DOUBLE_EQ(core.source().config().sample_rate, 1.92e6);
    EXPECT_DOUBLE_EQ(core.source().config().center_freq, 352.603125e6 - 250e3);
    EXPECT_EQ(app->numChannels(), 22);
    app->toggleRecording();
    EXPECT_TRUE(app->recording()) << app->recordError().toStdString();
    std::this_thread::sleep_for(1000ms);
    app->toggleRecording();
    EXPECT_FALSE(app->recording());
    EXPECT_GT(app->recordSeconds(), 0.3);
    EXPECT_TRUE(std::filesystem::exists(sigmf::data_path(app->recordPath().toStdString())));
    // 復号結果の保存: 開始 → 停止で session.json / result.json ができ、INCOMPLETE が消える(音声は無いので WAV は 0)
    app->toggleSaving();
    EXPECT_TRUE(app->saving());
    std::this_thread::sleep_for(300ms);
    app->toggleSaving();
    EXPECT_FALSE(app->saving());
    const auto save = app->saveStatus();
    EXPECT_EQ(save["state"].toString().toStdString(), "completed") << save["error"].toString().toStdString();
    const std::filesystem::path save_dir = save["directory"].toString().toStdString();
    EXPECT_TRUE(std::filesystem::exists(save_dir / "session.json"));
    EXPECT_TRUE(std::filesystem::exists(save_dir / "result.json"));
    EXPECT_FALSE(std::filesystem::exists(save_dir / "INCOMPLETE"));
    EXPECT_EQ(app->monitoredChannel(), app->selectedChannel()) << "通話が無ければ選択中のチャネルを聴く";
    EXPECT_TRUE(app->activeCalls().isEmpty());
    ASSERT_NE(app->bandView()->processor(), nullptr);
    EXPECT_GT(app->bandView()->processor()->rows_written(), 3u) << "帯域の観測点(jr_train.band)が流れていること";
    const auto chans = app->channels();
    ASSERT_EQ(chans.size(), 22);
    for (const auto& ch : chans) EXPECT_FALSE(ch.toMap()["active"].toBool()) << "無変調トーンでフレーム同期してはならない";
    EXPECT_EQ(app->losslessDrops(), 0.0);
    core.stop_app();
    EXPECT_FALSE(app->running());
    EXPECT_EQ(core.rx().consumer_count(), 0u) << "stop 後に consumer が残ってはならない";
    std::filesystem::remove_all(dir);
}

// STD-T99 DIGICOM: 4 Msps / LO = 選択チャネル − 250 kHz を宣言し、観測点が流れ、選局で LO が動き、stop で consumer が残らない。
// 受信機自体(規格書の試験信号でのフレーム・呼の情報・音声)の検証は apps/std_t99/tests。
TEST(Apps, StdT99RunsAndRetunesOnChannelChange) {
    Core core(".");
    SyntheticSignal sig;
    sig.noise_amplitude = 0.02;
    core.set_source(std::make_unique<SyntheticSource>(&core.events(), sig));
    auto app = std::make_shared<apps::StdT99App>(appfw::AppInfo{"std_t99", "STD-T99 DIGICOM", "", Direction::RX, ""});
    QVariantMap settings; settings["audio_device"] = "null";
    app->configure(settings);
    RfConfig c; c.center_freq = 100e6; c.sample_rate = 1e6;   // App が 4 Msps / チャネル − 250 kHz に上書きする
    app->set_rf_config(c);
    EXPECT_EQ(app->selectedChannel(), 18) << "既定は呼出チャネル";
    ASSERT_TRUE(core.run_app(app));
    EXPECT_DOUBLE_EQ(core.source().config().sample_rate, 4e6);
    EXPECT_DOUBLE_EQ(core.source().config().center_freq, 146.984375e6 - 250e3);
    const auto t0 = std::chrono::steady_clock::now();
    while ((!app->bandView()->processor() || app->bandView()->processor()->rows_written() < 4 ||
            app->channelView()->processor()->rows_written() < 4) && std::chrono::steady_clock::now() - t0 < 6s)
        std::this_thread::sleep_for(50ms);
    EXPECT_GT(app->bandView()->processor()->rows_written(), 3u) << "std_t99.band(400 kHz)が流れていること";
    EXPECT_GT(app->channelView()->processor()->rows_written(), 3u) << "std_t99.channel(100 kHz)が流れていること";
    app->setSelectedChannel(1);   // CH1 は 4 MHz 下: LO を動かす
    EXPECT_DOUBLE_EQ(core.source().config().center_freq, 142.934375e6 - 250e3);
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(app->status()["frames"].toDouble(), 0.0) << "雑音からフレームは出ない";
    EXPECT_EQ(app->losslessDrops(), 0.0);
    core.stop_app();
    EXPECT_FALSE(app->running());
    EXPECT_EQ(core.rx().consumer_count(), 0u) << "stop 後に consumer が残ってはならない";
}
