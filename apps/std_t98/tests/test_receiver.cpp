// STD-T98 受信機のテスト。
//   1) 同期語検出: ../std-t98-tools/tests/test_sync_word_correlator.py と同じシンボル単位の検査
//   2) symbol sync: 公称 26.04 sps の階段波からシンボルが回復できること
//   3) golden: 実機録音の抜粋(~/spear/golden/std_t98/ch3_pich.sigmf-*)を丸ごと通し、CSM/CRC が一致すること
// 合成 RF 変調器は持たない — std-t98-tools 自体が RX 専用で実信号で検証されており、規格外の送信モデルを
// 発明するより実録音を golden にするほうが正しい(§10.3)。
#include "receiver.hpp"
#include "dsp/sync_correlator.hpp"
#include "dsp/symbol_sync.hpp"
#include "spear/core/sigmf.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <numeric>
#include <random>

using namespace spear;
using namespace spear::std_t98;

namespace {
double sync_energy() { return std::inner_product(kSyncWord.begin(), kSyncWord.end(), kSyncWord.begin(), 0.0); }

int feed(SyncCorrelator& c, std::span<const float> s, uint64_t idx = 0) {
    const auto before = c.stats().detections;
    c.process(s, idx, [](const SyncCandidate&) { return false; }, [](uint64_t, bool) {});
    return static_cast<int>(c.stats().detections - before);
}

// 状態機械の検査用: フレーム(同期語 + 182 シンボル)を並べたシンボル列と、候補ごとの CRC を決める関数で回す
struct Run {
    std::vector<uint64_t> candidates, emitted, dropped;
    std::vector<bool> candidate_sync_ok;
};
void append_frame(std::vector<float>& s, bool sync = true) {
    if (sync) s.insert(s.end(), kSyncWord.begin(), kSyncWord.end());
    else s.insert(s.end(), 10, 0.f);                       // 崩れた同期語(SSE = 74 > 緩い閾値 0.6 × 74)
    for (int i = 0; i < 182; ++i) s.push_back(i % 2 ? 1.f : -1.f);
}
Run run_sync(SyncCorrelator& c, const std::vector<float>& s, const std::function<bool(uint64_t)>& crc = [](uint64_t) { return false; }) {
    Run r;
    // 小分けに入れる(ブロック境界をまたいでも同じになること)
    for (std::size_t off = 0; off < s.size(); off += 37) {
        const std::size_t n = std::min<std::size_t>(37, s.size() - off);
        c.process(std::span<const float>(s.data() + off, n), off,
                  [&](const SyncCandidate& k) { r.candidates.push_back(k.first_symbol_index); r.candidate_sync_ok.push_back(k.sync_ok); return crc(k.first_symbol_index); },
                  [&](uint64_t first, bool emit) { (emit ? r.emitted : r.dropped).push_back(first); });
    }
    return r;
}
}

TEST(StdT98Sync, ThresholdIsRatioTimesEnergy) {
    SyncCorrelator c(0.2);
    EXPECT_NEAR(c.stats().threshold, sync_energy() * 0.2, 1e-9);
    c.set_threshold_ratio(0.5);
    EXPECT_NEAR(c.stats().threshold, sync_energy() * 0.5, 1e-9);
}

TEST(StdT98Sync, RatioChangeAltersWhatCountsAsSync) {
    // 全シンボルが 1.0 ずれた同期語: SSE = 10 = 74 × 0.135 → 0.05 では棄却、0.2 では検出(Python テストと同じ)
    std::vector<float> noisy(kSyncWord.begin(), kSyncWord.end());
    for (float& v : noisy) v += 1.f;
    noisy.insert(noisy.end(), 182, 0.f);
    SyncCorrelator c(0.05);
    EXPECT_EQ(feed(c, noisy), 0);
    SyncCorrelator c2(0.2);
    EXPECT_EQ(feed(c2, noisy), 1);
}

TEST(StdT98Sync, CollectsExactly192SymbolsAfterDetection) {
    SyncCorrelator c(0.2);
    std::vector<float> stream(50, 0.f);
    stream.insert(stream.end(), kSyncWord.begin(), kSyncWord.end());
    for (int i = 0; i < 182; ++i) stream.push_back(static_cast<float>((i % 4) * 2 - 3));   // -3,-1,1,3,...
    stream.insert(stream.end(), 20, 0.f);
    int frames = 0;
    c.process(stream, 1000, [&](const SyncCandidate& k) {
        ++frames;
        EXPECT_EQ(k.symbols.size(), 192u);
        EXPECT_EQ(k.first_symbol_index, 1050u);                      // provenance: 同期語先頭のシンボル番号
        for (std::size_t i = 0; i < 10; ++i) EXPECT_FLOAT_EQ(k.symbols[i], kSyncWord[i]);
        EXPECT_FLOAT_EQ(k.symbols[10], -3.f);
        EXPECT_FLOAT_EQ(k.symbols[191], static_cast<float>((181 % 4) * 2 - 3));
        EXPECT_TRUE(k.sync_ok);
        return false;
    }, [](uint64_t, bool) { ADD_FAILURE() << "resolved before the next frame position was seen"; });
    EXPECT_EQ(frames, 1);
    EXPECT_EQ(c.stats().detections, 1u);
    EXPECT_EQ(c.state(), SyncCorrelator::State::Tentative);   // CRC 不良の 1 つだけでは出さない
}

TEST(StdT98Sync, CrcOkLocksAndEmitsImmediately) {
    SyncCorrelator c;
    std::vector<float> s(5, 0.f);
    append_frame(s);
    const auto r = run_sync(c, s, [](uint64_t) { return true; });
    EXPECT_EQ(r.emitted, (std::vector<uint64_t>{5}));
    EXPECT_TRUE(c.locked());
}

TEST(StdT98Sync, SecondSyncAtPredictedPositionConfirmsTentative) {
    SyncCorrelator c;
    std::vector<float> s(5, 0.f);
    append_frame(s); append_frame(s);
    s.insert(s.end(), 20, 0.f);
    const auto r = run_sync(c, s);
    EXPECT_EQ(r.emitted, (std::vector<uint64_t>{5, 197}));
    EXPECT_TRUE(r.dropped.empty());
    EXPECT_TRUE(c.locked());
}

TEST(StdT98Sync, CrcAloneDoesNotConfirmTentative) {
    // 雑音の偽同期の次の予測位置で 6 bit の SACCH CRC が偶然通っても捕捉しない(同期語が要る)
    std::vector<float> s;
    append_frame(s); append_frame(s, false);
    s.insert(s.end(), 20, 0.f);
    SyncCorrelator c;
    const auto r = run_sync(c, s, [](uint64_t first) { return first == 192; });
    EXPECT_TRUE(r.emitted.empty());
    EXPECT_EQ(r.dropped, (std::vector<uint64_t>{0, 192}));
    EXPECT_FALSE(c.locked());
}

TEST(StdT98Sync, FalseSyncIsDroppedAndTheRealOneInsideItIsFound) {
    // 偽同期(5)の 192 シンボルの中に本物の呼の先頭(60)がある: 偽同期を捨て、5 の次から探し直して 60, 252 を出す
    std::vector<float> s(5, 0.f);
    s.insert(s.end(), kSyncWord.begin(), kSyncWord.end());
    s.insert(s.end(), 45, 0.f);
    append_frame(s); append_frame(s);
    s.insert(s.end(), 20, 0.f);
    SyncCorrelator c;
    const auto r = run_sync(c, s);
    EXPECT_EQ(r.dropped, (std::vector<uint64_t>{5, 197}));
    EXPECT_EQ(r.emitted, (std::vector<uint64_t>{60, 252}));
    EXPECT_EQ(c.stats().dropped, 2u);
}

TEST(StdT98Sync, FlywheelKeepsFrameWithCorruptedSyncWord) {
    // 同期中に同期語が崩れたフレーム(CRC も不良)は保留し、次の良いフレームで出す(フライホイール)
    std::vector<float> s;
    append_frame(s); append_frame(s); append_frame(s, false); append_frame(s);
    s.insert(s.end(), 20, 0.f);
    SyncCorrelator c;
    const auto r = run_sync(c, s);
    EXPECT_EQ(r.emitted, (std::vector<uint64_t>{0, 192, 384, 576}));
    EXPECT_EQ(c.stats().flywheel, 1u);
    EXPECT_TRUE(c.locked());
}

TEST(StdT98Sync, CrcOkAtPredictedPositionCountsAsGoodWithoutSyncWord) {
    std::vector<float> s;
    append_frame(s); append_frame(s); append_frame(s, false);
    s.insert(s.end(), 20, 0.f);
    SyncCorrelator c;
    const auto r = run_sync(c, s, [](uint64_t first) { return first == 384; });
    EXPECT_EQ(r.emitted, (std::vector<uint64_t>{0, 192, 384}));   // 3 つ目は保留せずすぐ出る
    EXPECT_EQ(c.stats().flywheel, 1u);
}

TEST(StdT98Sync, LosesLockAfterMaxMissesAndDropsHeldFrames) {
    // 2 フレームの呼 → 3 フレーム分の崩れ(送信の終わり)→ 喪失。崩れの途中(+100)から始まる次の呼を見つける
    std::vector<float> s;
    append_frame(s); append_frame(s);
    const uint64_t miss0 = s.size();
    s.insert(s.end(), 100, 0.f);
    const uint64_t next_call = s.size();
    append_frame(s); append_frame(s);
    s.insert(s.end(), 3 * 192 + 20, 0.f);
    SyncCorrelator c;
    const auto r = run_sync(c, s);
    EXPECT_EQ(r.emitted, (std::vector<uint64_t>{0, 192, next_call, next_call + 192}));
    // 1 回目の喪失で 3 つ捨てる(384 = miss0, 576, 768)。次の呼の後ろの崩れでもう一度喪失する
    ASSERT_GE(r.dropped.size(), 3u);
    EXPECT_EQ(r.dropped[0], miss0);
    EXPECT_FALSE(c.locked());
}

TEST(StdT98Sync, FollowsOneSymbolSlip) {
    std::vector<float> s;
    append_frame(s); append_frame(s);
    s.push_back(0.f);                // symbol sync が 1 シンボル余分に出した
    append_frame(s); append_frame(s);
    s.insert(s.end(), 20, 0.f);
    SyncCorrelator c;
    const auto r = run_sync(c, s);
    EXPECT_EQ(r.emitted, (std::vector<uint64_t>{0, 192, 385, 577}));
    EXPECT_EQ(c.stats().flywheel, 0u);
    EXPECT_TRUE(c.locked());
}

TEST(StdT98SymbolSync, RecoversSymbolsFromFractionalSpsStaircase) {
    // 62.5 kHz / 2400 baud = 26.0417 sps。各シンボルを保持した階段波を +0.05% (= +0.013 sample/symbol、max_dev 0.02 の内側)の
    // 速度誤差で作り、平均周期がそこへ収束しシンボルが回復できること。
    const double sps_true = 26.0417 * 1.0005;
    std::mt19937 rng(3);
    std::uniform_int_distribution<int> lv(0, 3);
    std::vector<float> levels;
    for (int i = 0; i < 600; ++i) levels.push_back(static_cast<float>(lv(rng) * 2 - 3));
    std::vector<float> wave;
    for (double t = 0; t < levels.size() * sps_true; t += 1.0) {
        const double k = t / sps_true, frac = k - std::floor(k);
        const std::size_t i = static_cast<std::size_t>(k);
        // 遷移を 6 サンプルの直線でなまらせる(Gardner が動く最低限)
        float v = levels[i];
        if (frac < 6.0 / sps_true && i > 0) v = static_cast<float>(levels[i - 1] + (levels[i] - levels[i - 1]) * (frac * sps_true / 6.0));
        wave.push_back(v);
    }
    SymbolSync ss(26.0417, 0.06, 1.1, 0.1, 0.02);
    std::vector<float> out;
    std::vector<double> pos;
    for (std::size_t p = 0; p < wave.size(); p += 1000) ss.process(std::span<const float>(wave.data() + p, std::min<std::size_t>(1000, wave.size() - p)), out, &pos);
    ASSERT_GT(out.size(), 500u);
    EXPECT_NEAR(ss.period(), sps_true, 0.01);
    // 後半 300 シンボルはすべて ±1/±3 に 0.25 以内で乗る
    int bad = 0;
    for (std::size_t i = out.size() - 300; i < out.size(); ++i) {
        const float v = out[i], q = v > 2 ? 3.f : v > 0 ? 1.f : v > -2 ? -1.f : -3.f;
        if (std::fabs(v - q) > 0.25f) ++bad;
    }
    EXPECT_LE(bad, 3);
    for (std::size_t i = 1; i < pos.size(); ++i) EXPECT_GT(pos[i], pos[i - 1]);
}

TEST(StdT98SymbolSync, AveragePeriodStaysWithinMaxDeviation) {
    // 速度誤差が max_dev (0.02 sample/symbol) を超える +0.3% でも、平均周期は nominal ± 0.02 に留まり(GR と同じ)、
    // 位相は比例項で追従してシンボルは回復できる。以前は max_dev を比率と誤解して ±0.5 sample を許し、誤った周期に居着いた。
    const double nominal = 26.0417, sps_true = nominal * 1.003;
    std::mt19937 rng(5);
    std::uniform_int_distribution<int> lv(0, 3);
    std::vector<float> levels;
    for (int i = 0; i < 800; ++i) levels.push_back(static_cast<float>(lv(rng) * 2 - 3));
    std::vector<float> wave;
    for (double t = 0; t < levels.size() * sps_true; t += 1.0) {
        const double k = t / sps_true, frac = k - std::floor(k);
        const std::size_t i = static_cast<std::size_t>(k);
        float v = levels[i];
        if (frac < 6.0 / sps_true && i > 0) v = static_cast<float>(levels[i - 1] + (levels[i] - levels[i - 1]) * (frac * sps_true / 6.0));
        wave.push_back(v);
    }
    SymbolSync ss(nominal, 0.06, 1.1, 0.1, 0.02);
    std::vector<float> out;
    for (std::size_t p = 0; p < wave.size(); p += 1000) ss.process(std::span<const float>(wave.data() + p, std::min<std::size_t>(1000, wave.size() - p)), out);
    EXPECT_LE(ss.period(), nominal + 0.02 + 1e-9);
    EXPECT_GE(ss.period(), nominal - 0.02 - 1e-9);
    int bad = 0;
    for (std::size_t i = out.size() - 300; i < out.size(); ++i) {
        const float v = out[i], q = v > 2 ? 3.f : v > 0 ? 1.f : v > -2 ? -1.f : -3.f;
        if (std::fabs(v - q) > 0.25f) ++bad;
    }
    EXPECT_LE(bad, 3);
}

// ---- チャネルの活動: 電力ではなく同期で決まる ----
// 会場の広帯域雑音 + AGC で床が全チャネル一斉にブロックごとに数 dB 動く状況(2026-09-20 の実地録音)を、白色雑音の振幅を
// 20 ms ごとに ±10 dB 段階的に変えて再現し、さらに 1 チャネルに強い無変調の搬送波を置く。
// 電力の閾値なら搬送波のチャネルは開き、床の揺れで他のチャネルも開閉したが、活動は同期だけで決まるので、どのチャネルも活動しない
namespace {
struct PumpedNoise {
    std::mt19937 rng{11};
    std::normal_distribution<float> n{0.f, 0.05f};
    std::uniform_int_distribution<int> step{0, 3};
    double gain_db = 0;
    int in_block = 0;
    // 1 ブロック(4 ms @ 1.2 Msps = 4800 sample)。20 ms ごとにゲインを {-10, 0, +10, +20} dB からランダムに選び直す
    void fill(std::vector<cf32>& buf, double tone_hz, float tone_amp, double& tone_phase, double fs) {
        if (in_block++ % 5 == 0) gain_db = -10.0 + 10.0 * step(rng);
        const float g = static_cast<float>(std::pow(10.0, gain_db / 20.0));
        for (auto& x : buf) {
            cf32 v(n(rng), n(rng));
            if (tone_amp > 0) { v += tone_amp * cf32(static_cast<float>(std::cos(tone_phase)), static_cast<float>(std::sin(tone_phase))); tone_phase += 2 * M_PI * tone_hz / fs; }
            x = v * g;
        }
    }
};
}

TEST(StdT98Activity, PumpedNoiseAndCarrierWithoutSyncAreNeverActive) {
    ReceiverConfig cfg;   // 1.2 Msps / 48ch(テストが軽い)
    Receiver rx(cfg);
    const int ch = 10;
    const std::size_t block = 4800;   // 4 ms
    std::vector<cf32> buf(block);
    PumpedNoise src;
    double ph = 0;
    uint64_t idx = 0;
    // 床 + 約 20 dB の搬送波(1 チャネル 6.25 kHz あたりの雑音電力 ≈ σ² × 2 × 6250 / 1.2e6)
    const float sigma2 = 0.05f * 0.05f * 2;
    const float tone_amp = std::sqrt(sigma2 * 6250.f / 1.2e6f * std::pow(10.f, 20.f / 10.f));
    std::vector<int> active_blocks(static_cast<std::size_t>(cfg.num_channels), 0);
    std::size_t frames = 0;
    Observer obs;
    obs.frame = [&](const Frame&) { ++frames; };
    std::size_t eye = 0;
    obs.eye = [&](int, std::span<const float>) { ++eye; };
    rx.set_observer(obs);
    for (int b = 0; b < 750; ++b) {   // 3 s
        src.fill(buf, rx.channel_offset_hz(ch), tone_amp, ph, cfg.in_rate);
        rx.process(buf, idx);
        idx += block;
        for (int c = 0; c < cfg.num_channels; ++c) if (rx.metrics(c).active) ++active_blocks[static_cast<std::size_t>(c)];
    }
    for (int c = 0; c < cfg.num_channels; ++c) {
        EXPECT_EQ(active_blocks[static_cast<std::size_t>(c)], 0) << "ch " << c;
        EXPECT_FALSE(rx.metrics(c).locked) << "ch " << c;
    }
    EXPECT_EQ(frames, 0u);
    EXPECT_EQ(eye, 0u);
    EXPECT_GT(rx.metrics(ch).power_db, rx.metrics(ch + 3).power_db + 10);   // 電力では搬送波のチャネルは目立っている
}

// ---- golden: 実機録音 ----
// 録音(<base>.sigmf-*)と期待値(<base>.golden.json)はリポジトリ外(個体の LO 誤差や無線機の識別符号を含むため)。
// 無ければ skip。自分の録音で golden を作るには: 録音 → spear-std-t98-decode で確認 → 抜粋と JSON を ~/spear/golden/std_t98/ に置く。
namespace {
std::string golden_base() {
    if (const char* d = std::getenv("SPEAR_GOLDEN_DIR")) return std::string(d) + "/std_t98/ch3_pich";
    if (const char* h = std::getenv("HOME")) return std::string(h) + "/spear/golden/std_t98/ch3_pich";
    return {};
}
struct Golden { double freq_err_hz = 0; int channel = 0; std::string csm; int min_frames = 1, pich_frames = 0, min_sacch = 0; };   // squelch_db は読まない(床相対なので録音ごとの値は要らない)
// 最小限の JSON 読み("key": 値 を探すだけ。ネストなし)
std::string jvalue(const std::string& s, const std::string& key) {
    const auto k = s.find("\"" + key + "\"");
    if (k == std::string::npos) return {};
    auto p = s.find(':', k);
    if (p == std::string::npos) return {};
    ++p;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
    if (p < s.size() && s[p] == '"') { const auto e = s.find('"', p + 1); return s.substr(p + 1, e - p - 1); }
    const auto e = s.find_first_of(",}\n", p);
    return s.substr(p, e - p);
}
bool read_golden(const std::string& base, Golden& g) {
    std::ifstream f(base + ".golden.json");
    if (!f) return false;
    const std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    g.freq_err_hz = std::stod(jvalue(s, "freq_err_hz"));
    g.channel = std::stoi(jvalue(s, "channel"));
    g.csm = jvalue(s, "csm");
    g.min_frames = std::stoi(jvalue(s, "min_frames"));
    g.pich_frames = std::stoi(jvalue(s, "pich_frames"));
    g.min_sacch = std::stoi(jvalue(s, "min_sacch_frames"));
    return true;
}
}

TEST(StdT98Golden, RealRecordingCh3PichAndSacch) {
    const std::string base = golden_base();
    sigmf::Meta meta;
    std::string err;
    Golden g;
    if (base.empty() || !sigmf::read(base, meta, &err) || !read_golden(base, g)) GTEST_SKIP() << "golden recording not present: " << base << " (" << err << ")";
    ASSERT_EQ(meta.sample_rate, 4e6);

    ReceiverConfig cfg;
    cfg.in_rate = meta.sample_rate;
    cfg.pfb_channels = 64;
    cfg.freq_err_hz = g.freq_err_hz;   // 録音した個体の LO 誤差(golden.json)
    Receiver rx(cfg);

    std::vector<Frame> frames;
    std::size_t eye_traces = 0, eye_on_level = 0;
    Observer obs;
    obs.frame = [&](const Frame& f) { frames.push_back(f); };
    obs.eye = [&](int ch, std::span<const float> t) {
        if (ch != g.channel) return;
        ++eye_traces;
        // 判定点(1/4, 3/4)の値が ±1/±3 のいずれかに 0.5 以内なら「目が開いている」
        for (std::size_t k : {t.size() / 4, t.size() * 3 / 4}) {
            const float v = t[k], q = v > 2 ? 3.f : v > 0 ? 1.f : v > -2 ? -1.f : -3.f;
            if (std::fabs(v - q) < 0.5f) ++eye_on_level;
        }
    };
    rx.set_observer(obs);

    std::ifstream f(sigmf::data_path(base), std::ios::binary);
    ASSERT_TRUE(f.good());
    std::vector<sc16> raw(65536);
    std::vector<cf32> iq(65536);
    uint64_t idx = 0;
    while (f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size() * sizeof(sc16))) || f.gcount() > 0) {
        const std::size_t n = static_cast<std::size_t>(f.gcount()) / sizeof(sc16);
        for (std::size_t i = 0; i < n; ++i) iq[i] = cf32(raw[i].real() / 32768.f, raw[i].imag() / 32768.f);
        rx.process(std::span<const cf32>(iq.data(), n), idx);
        idx += n;
        if (f.eof()) break;
    }

    // 抜粋は 1 チャネルだけ送信中: PICH + SACCH、全 CRC OK、他チャネルにフレームなし
    ASSERT_GE(frames.size(), static_cast<std::size_t>(g.min_frames));
    int pich = 0, sacch = 0;
    for (const auto& fr : frames) {
        EXPECT_EQ(fr.channel, g.channel);
        EXPECT_TRUE(fr.rich.parity_ok);
        if (fr.rich.f == 0) { ++pich; EXPECT_TRUE(fr.pich.crc_ok); EXPECT_EQ(fr.pich.csm, g.csm); }
        else { ++sacch; EXPECT_TRUE(fr.sacch.crc_ok); EXPECT_EQ(fr.tch_payload.size(), 36u); }
    }
    EXPECT_EQ(pich, g.pich_frames);
    EXPECT_GE(sacch, g.min_sacch);
    // フレームは 192 シンボルごと、入力サンプル換算 192 × 4e6/2400 = 320000 ± 1 シンボル分
    for (std::size_t i = 1; i < frames.size(); ++i) {
        EXPECT_EQ(frames[i].symbol_index - frames[i - 1].symbol_index, 192u);
        const double d = static_cast<double>(frames[i].input_sample_index - frames[i - 1].input_sample_index);
        EXPECT_NEAR(d, 320000.0, 2000.0);
    }
    // アイパターン: 1.2 s × 2400 baud ≈ 2900 トレース(活動中 = 同期中の間)、判定点の 90% 以上が ±1/±3 上
    EXPECT_GT(eye_traces, 2000u);
    std::printf("[ eye ] traces %zu, decision points on level %.1f%%\n", eye_traces, 100.0 * static_cast<double>(eye_on_level) / static_cast<double>(2 * eye_traces));
    EXPECT_GT(static_cast<double>(eye_on_level) / static_cast<double>(2 * eye_traces), 0.9);
    EXPECT_EQ(rx.metrics(g.channel).csm, g.csm);
    EXPECT_LT(rx.metrics(g.channel).best_sse, 1.0);
    for (int c = 0; c < cfg.num_channels; ++c) if (c != g.channel) EXPECT_EQ(rx.metrics(c).frames, 0u);
}

// v2.0 Table 3-5: channel numbering is deliberately not frequency order.
#include "channel_plan.hpp"
#include <set>
TEST(StdT98Plan, RegisteredFrequencyBoundariesAndUniqueBins) {
    const auto cfg = registered_receiver_config();
    Receiver rx(cfg);
    EXPECT_EQ(cfg.num_channels, 97);
    EXPECT_EQ(rx.band_rate(), 800000);
    const std::vector<std::pair<int, int>> boundaries = {
        {0,351200000}, {29,351381250}, {30,351387500}, {69,351631250},
        {70,351031250}, {81,351100000}, {82,351168750}, {86,351193750},
        {87,351106250}, {96,351162500}};
    for (auto [index, hz] : boundaries)
        EXPECT_EQ(kBandReferenceHz + rx.channel_offset_hz(index), hz) << index;
    std::set<int> bins;
    for (int i = 0; i < cfg.num_channels; ++i) {
        EXPECT_TRUE(bins.insert(rx.channel_to_bin(i)).second);
        EXPECT_EQ(kRegisteredPlan[i].number, i < 82 ? i + 1 : i - 81);
        EXPECT_EQ(kRegisteredPlan[i].aerial, i >= 82);
    }
}

TEST(StdT98Plan, ExpandedBandRoutesCarriersToLogicalChannels) {
    // Exercise the actual decimator/PFB on both band edges and numbering wraps.
    for (int target : {0, 29, 30, 69, 70, 81, 82, 86, 87, 96}) {
        auto cfg = registered_receiver_config();
        cfg.freq_err_hz = 500000; // production LO offset
        Receiver rx(cfg);
        std::vector<cf32> samples(16000);
        const double freq = rx.channel_offset_hz(target) + cfg.freq_err_hz;
        for (int block = 0; block < 12; ++block) {
            const auto first = static_cast<uint64_t>(block) * samples.size();
            for (std::size_t i = 0; i < samples.size(); ++i) {
                const double phase = 2 * M_PI * freq * static_cast<double>(first + i) / cfg.in_rate;
                samples[i] = cf32(static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase)));
            }
            rx.process(samples, first);
        }
        for (int ch = 0; ch < cfg.num_channels; ++ch)
            if (ch != target) { EXPECT_GT(rx.metrics(target).power_db, rx.metrics(ch).power_db + 15) << target << " vs " << ch; }
    }
}

TEST(StdT98Plan, RejectsInvalidExplicitMaps) {
    auto cfg = registered_receiver_config();
    cfg.channel_bins.pop_back();
    EXPECT_THROW(Receiver{cfg}, std::invalid_argument);
    cfg = registered_receiver_config();
    cfg.channel_bins[0] = cfg.channel_bins[1];
    EXPECT_THROW(Receiver{cfg}, std::invalid_argument);
    cfg = registered_receiver_config();
    cfg.channel_bins[0] = 64;
    EXPECT_THROW(Receiver{cfg}, std::invalid_argument);
}
