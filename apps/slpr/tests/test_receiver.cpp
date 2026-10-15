// 特定小電力 20ch 受信機のテスト。
//   1) チャネル表: 各社対応表(ICOM 01–20)の周波数と一致すること
//   2) 同時受信: 2 チャネルに同時に NFM(音声トーン)を置き、それぞれの音声にそのトーンだけが出て、他の 18 ch は閉じたまま無音であること
//   3) 隣接チャネル: 強い局の隣(12.5 kHz)と、群の境(ch11 / ch12、25 kHz)が漏れで開かないこと
//   4) 雑音だけ: どのチャネルも開かない(床相対 squelch)
//   5) provenance: 送信開始の radio.rx index が squelch の開いた index に出ること
//   6) LO オフセット: 帯域中心 − 250 kHz の LO(App の運転形)でも同じ結果になること
// 合成 NFM は規格の定義そのもの(偏移 ±2.5 kHz の周波数変調)なので、STD-T98 のような「送信整形の推測」を含まない。
#include "receiver.hpp"

#include <fftw3.h>
#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <random>

using namespace spear;
using namespace spear::slpr;

namespace {
struct Fm { int ch; double tone_hz; double amplitude; double start_s = 0; double freq_err_hz = 0; };

// 帯域中心(または lo_offset だけ下の LO)を DC とする cf32 を 4 Msps で合成する
std::vector<cf32> synth(double seconds, const std::vector<Fm>& fms, double noise, double lo_offset_hz = 0, uint32_t seed = 1) {
    const double fs = 4e6;
    const auto n = static_cast<std::size_t>(seconds * fs);
    std::vector<cf32> out(n);
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0.f, static_cast<float>(noise));
    for (auto& x : out) x = cf32(g(rng), g(rng));
    for (const auto& f : fms) {
        const double off = channel_offset_hz(f.ch - 1) + lo_offset_hz + f.freq_err_hz;
        double ph = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const double t = static_cast<double>(i) / fs;
            if (t < f.start_s) continue;
            const double inst = off + 2500.0 * std::sin(2 * std::numbers::pi * f.tone_hz * t);   // 偏移 ±2.5 kHz
            ph += 2 * std::numbers::pi * inst / fs;
            if (ph > std::numbers::pi) ph -= 2 * std::numbers::pi;
            out[i] += cf32(static_cast<float>(f.amplitude * std::cos(ph)), static_cast<float>(f.amplitude * std::sin(ph)));
        }
    }
    return out;
}

struct Rig {
    Receiver rx;
    std::vector<std::vector<float>> audio;
    std::vector<std::pair<int, std::pair<bool, uint64_t>>> squelch;
    explicit Rig(ReceiverConfig c = {}) : rx(c), audio(kNumChannels) {
        Observer o;
        o.audio = [this](int ch, std::span<const float> a, uint64_t) { audio[static_cast<std::size_t>(ch)].insert(audio[static_cast<std::size_t>(ch)].end(), a.begin(), a.end()); };
        o.squelch = [this](int ch, bool open, uint64_t idx) { squelch.push_back({ch, {open, idx}}); };
        rx.set_observer(std::move(o));
    }
    void feed(const std::vector<cf32>& iq, std::size_t block = 16384, uint64_t first = 0) {
        for (std::size_t i = 0; i < iq.size(); i += block)
            rx.process(std::span<const cf32>(iq.data() + i, std::min(block, iq.size() - i)), first + i);
    }
};

double peak_hz(const std::vector<float>& a, double rate, int N = 4096) {
    std::vector<fftwf_complex> in(static_cast<std::size_t>(N)), out(static_cast<std::size_t>(N));
    auto plan = fftwf_plan_dft_1d(N, in.data(), out.data(), FFTW_FORWARD, FFTW_ESTIMATE);
    for (int i = 0; i < N; ++i) { in[static_cast<std::size_t>(i)][0] = a[a.size() - static_cast<std::size_t>(N) + static_cast<std::size_t>(i)]; in[static_cast<std::size_t>(i)][1] = 0; }
    fftwf_execute(plan);
    int pk = 1; float pm = 0;
    for (int i = 1; i < N / 2; ++i) { const float m = out[static_cast<std::size_t>(i)][0] * out[static_cast<std::size_t>(i)][0] + out[static_cast<std::size_t>(i)][1] * out[static_cast<std::size_t>(i)][1]; if (m > pm) { pm = m; pk = i; } }
    fftwf_destroy_plan(plan);
    return pk * rate / N;
}
double rms_tail(const std::vector<float>& a, std::size_t n) {
    double s = 0;
    for (std::size_t i = a.size() - n; i < a.size(); ++i) s += static_cast<double>(a[i]) * a[i];
    return std::sqrt(s / static_cast<double>(n));
}
} // namespace

TEST(SlprChannels, TableMatchesManufacturerChart) {
    ASSERT_EQ(kChannels.size(), 20u);
    // 単信 11ch: 422.0500 から 12.5 kHz 刻み
    for (int i = 0; i < 11; ++i) {
        EXPECT_EQ(kChannels[static_cast<std::size_t>(i)].number, i + 1);
        EXPECT_DOUBLE_EQ(kChannels[static_cast<std::size_t>(i)].freq_hz, 422.05e6 + i * 12500.0);
        EXPECT_STREQ(kChannels[static_cast<std::size_t>(i)].group, kGroup11);
    }
    // 単信 9ch: 422.2000 から 12.5 kHz 刻み(ch11 と ch12 の間は 25 kHz)
    for (int i = 11; i < 20; ++i) {
        EXPECT_EQ(kChannels[static_cast<std::size_t>(i)].number, i + 1);
        EXPECT_DOUBLE_EQ(kChannels[static_cast<std::size_t>(i)].freq_hz, 422.2e6 + (i - 11) * 12500.0);
        EXPECT_STREQ(kChannels[static_cast<std::size_t>(i)].group, kGroup9);
    }
    EXPECT_DOUBLE_EQ(kChannels[10].freq_hz, 422.175e6);
    EXPECT_DOUBLE_EQ(kChannels[11].freq_hz, 422.2e6);
    EXPECT_DOUBLE_EQ(kChannels[19].freq_hz, 422.3e6);
    // grid は帯域中心(ch11)からのオフセット / 12.5 kHz と一致し、PFB の bin に写る
    Receiver rx({});
    for (int i = 0; i < kNumChannels; ++i) {
        EXPECT_DOUBLE_EQ(kBandCenterHz + channel_offset_hz(i), kChannels[static_cast<std::size_t>(i)].freq_hz) << "ch" << i + 1;
        const int g = kChannels[static_cast<std::size_t>(i)].grid;
        EXPECT_EQ(rx.channel_to_bin(i), g >= 0 ? g : 32 + g);
    }
    EXPECT_STREQ(kChannels[0].name, "01");
    EXPECT_STREQ(kChannels[19].name, "20");
}

TEST(SlprReceiver, TwoChannelsAtOnce) {
    Rig r;
    // ch03(−100 kHz)に 1 kHz、ch15(+62.5 kHz)に 700 Hz の音声トーン。−20 dBFS、雑音 0.02(bin あたり −59 dB)
    r.feed(synth(1.0, {{3, 1000, 0.1}, {15, 700, 0.1}}, 0.02));
    for (int c = 0; c < kNumChannels; ++c) {
        const auto& m = r.rx.metrics(c);
        const bool want = (c == 2 || c == 14);
        EXPECT_EQ(m.open, want) << "ch" << c + 1 << " power " << m.power_db << " thr " << m.threshold_db;
        EXPECT_EQ(r.audio[static_cast<std::size_t>(c)].size(), r.audio[0].size()) << "全チャネルの音声は同じ歩み(ミックスの前提)";
        if (!want) { EXPECT_DOUBLE_EQ(rms_tail(r.audio[static_cast<std::size_t>(c)], 4096), 0.0) << "閉じたチャネルは無音 ch" << c + 1; }
    }
    EXPECT_NEAR(peak_hz(r.audio[2], 12500), 1000.0, 12500.0 / 4096 * 2);
    EXPECT_NEAR(peak_hz(r.audio[14], 12500), 700.0, 12500.0 / 4096 * 2);
    // 偏移 2.5 kHz = fm_dev なので音声は ±1.0 の正弦波 → RMS ≈ 0.71
    EXPECT_NEAR(rms_tail(r.audio[2], 4096), 0.707, 0.08);
    EXPECT_NEAR(rms_tail(r.audio[14], 4096), 0.707, 0.08);
    EXPECT_GT(r.rx.metrics(2).power_db, r.rx.metrics(2).floor_db + 30);
    EXPECT_EQ(r.rx.metrics(2).opens, 1u);
    EXPECT_NEAR(r.rx.metrics(2).open_s, 1.0 - 0.02, 0.03);   // attack 20 ms の後から
}

TEST(SlprReceiver, AdjacentChannelsDoNotLeak) {
    Rig r;
    // ch05 と ch11 に強い局(−14 dBFS、床より 42 dB 上)。隣の ch04 / ch06(±12.5 kHz)と群の境の ch12(+25 kHz)は開かない。
    // 長さはブロック(16384)の整数倍にして、最後の電力が丸ごと 1 ブロック(bin あたり 51 sample)から出るようにする
    r.feed(synth(0.512, {{5, 1000, 0.2}, {11, 1000, 0.2}}, 0.02));
    EXPECT_TRUE(r.rx.metrics(4).open);
    EXPECT_TRUE(r.rx.metrics(10).open);
    for (int c : {3, 5, 9, 11}) {
        EXPECT_FALSE(r.rx.metrics(c).open) << "ch" << c + 1 << " power " << r.rx.metrics(c).power_db << " thr " << r.rx.metrics(c).threshold_db;
        EXPECT_EQ(r.rx.metrics(c).opens, 0u) << "ch" << c + 1 << ": キーアップの過渡(1 ブロック)で開いてはならない(attack)";
    }
    // 漏れは床に埋もれる(プロトタイプの阻止域 −53 dB は信号 −14 dB に対して −67 dB、床 −56 dB より下)
    EXPECT_LT(r.rx.metrics(5).power_db, r.rx.metrics(5).floor_db + 3);
    EXPECT_LT(r.rx.metrics(11).power_db, r.rx.metrics(11).floor_db + 3);
}

TEST(SlprReceiver, NoiseOnlyStaysClosed) {
    Rig r;
    r.feed(synth(0.5, {}, 0.02));
    for (int c = 0; c < kNumChannels; ++c) {
        EXPECT_FALSE(r.rx.metrics(c).open) << "ch" << c + 1;
        EXPECT_EQ(r.rx.metrics(c).opens, 0u);
    }
    EXPECT_TRUE(r.squelch.empty());
    // 床 = 中央値なので、閾値は床 + margin
    EXPECT_NEAR(r.rx.metrics(0).threshold_db, r.rx.metrics(0).floor_db + 8, 1e-9);
}

TEST(SlprReceiver, SquelchProvenancePointsAtTransmissionStart) {
    Rig r;
    const uint64_t first = 5'000'000;   // radio.rx の index は 0 から始まらない
    r.feed(synth(0.8, {{8, 1000, 0.1, 0.3}}, 0.02), 8192, first);
    ASSERT_FALSE(r.squelch.empty());
    EXPECT_EQ(r.squelch[0].first, 7);
    EXPECT_TRUE(r.squelch[0].second.first);
    // 開いた index は attack に関係なく「超え始めた」ブロックの先頭: ブロック単位(8192)の粒度で、送信開始 0.3 s = 1.2M sample の直前〜群遅延ぶん後
    const double open_idx = static_cast<double>(r.squelch[0].second.second - first);
    EXPECT_NEAR(open_idx, 0.3 * 4e6, 8192 + 4000);
    // 閉じる遷移は無い(送信は最後まで続く)
    EXPECT_EQ(r.squelch.size(), 1u);
}

TEST(SlprReceiver, CloseAfterHold) {
    Rig r;
    // 0–0.3 s 送信、以後は雑音だけ → hold(0.15 s)の後に閉じる
    auto iq = synth(0.3, {{8, 1000, 0.1}}, 0.02);
    auto tail = synth(0.5, {}, 0.02, 0, 7);
    iq.insert(iq.end(), tail.begin(), tail.end());
    r.feed(iq, 8192);
    ASSERT_EQ(r.squelch.size(), 2u);
    EXPECT_TRUE(r.squelch[0].second.first);
    EXPECT_FALSE(r.squelch[1].second.first);
    const double close_idx = static_cast<double>(r.squelch[1].second.second);
    EXPECT_NEAR(close_idx, (0.3 + 0.15) * 4e6, 2 * 8192 + 4000);
    EXPECT_FALSE(r.rx.metrics(7).open);
    EXPECT_NEAR(r.rx.metrics(7).open_s, 0.45 - 0.02, 0.02);
    // 隣は開いていない(キーアップ / キーダウンの過渡は 1 ブロックなので attack で捨てられる)
    EXPECT_EQ(r.rx.metrics(6).opens, 0u);
    EXPECT_EQ(r.rx.metrics(8).opens, 0u);
}

TEST(SlprReceiver, LoOffsetBelowBandCenter) {
    // App の運転形: LO = 帯域中心 − 250 kHz。受信機は freq_err_hz = +250 kHz で帯域中心を DC へ戻す
    ReceiverConfig c; c.freq_err_hz = 250e3;
    Rig r(c);
    r.feed(synth(0.6, {{20, 1000, 0.1}, {1, 500, 0.1}}, 0.02, 250e3));
    for (int ch = 0; ch < kNumChannels; ++ch) EXPECT_EQ(r.rx.metrics(ch).open, ch == 0 || ch == 19) << "ch" << ch + 1;
    EXPECT_NEAR(peak_hz(r.audio[19], 12500), 1000.0, 12500.0 / 4096 * 2);
    EXPECT_NEAR(peak_hz(r.audio[0], 12500), 500.0, 12500.0 / 4096 * 2);
}

TEST(SlprReceiver, TransmitterOffsetDoesNotLeaveDcInAudio) {
    // 送信機が +800 Hz ずれていても(特定小電力の許容偏差 ±4 ppm ≈ ±1.7 kHz)、検波後の直流は DC 阻止で消える
    Rig r;
    r.feed(synth(0.6, {{9, 1000, 0.1, 0, 800}}, 0.02));
    EXPECT_TRUE(r.rx.metrics(8).open);
    const auto& a = r.audio[8];
    double mean = 0;
    for (std::size_t i = a.size() - 4096; i < a.size(); ++i) mean += a[i];
    mean /= 4096;
    EXPECT_LT(std::abs(mean), 0.02);   // 直流のまま出ると 800 / 2500 = 0.32
    EXPECT_NEAR(peak_hz(a, 12500), 1000.0, 12500.0 / 4096 * 2);
}
