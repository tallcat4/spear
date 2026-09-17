// STD-T98 受信機のテスト。
//   1) 同期語検出: ../std-t98-tools/tests/test_sync_word_correlator.py と同じシンボル単位の検査
//   2) symbol sync: 公称 26.04 sps の階段波からシンボルが回復できること
//   3) golden: 実機録音の抜粋(~/spear/golden/std_t98/ch3_pich.sigmf-*)を丸ごと通し、CSM/CRC が一致すること
// 合成 RF 変調器は持たない — std-t98-tools 自体が RX 専用で実信号で検証されており、規格外の送信モデルを
// 発明するより実録音を golden にするほうが正しい(§12.3)。
#include "receiver.hpp"
#include "dsp/sync_correlator.hpp"
#include "dsp/symbol_sync.hpp"
#include "spear/core/sigmf.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <random>

using namespace spear;
using namespace spear::std_t98;

namespace {
double sync_energy() { return std::inner_product(kSyncWord.begin(), kSyncWord.end(), kSyncWord.begin(), 0.0); }

int feed(SyncCorrelator& c, std::span<const float> s, uint64_t idx = 0) {
    const auto before = c.stats().detections;
    c.process(s, idx, [](std::span<const float>, uint64_t) {});
    return static_cast<int>(c.stats().detections - before);
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
    SyncCorrelator c(0.05);
    EXPECT_EQ(feed(c, noisy), 0);
    c.set_threshold_ratio(0.2);
    EXPECT_EQ(feed(c, noisy), 1);
}

TEST(StdT98Sync, CollectsExactly192SymbolsAfterDetection) {
    SyncCorrelator c(0.2);
    std::vector<float> stream(50, 0.f);
    stream.insert(stream.end(), kSyncWord.begin(), kSyncWord.end());
    for (int i = 0; i < 182; ++i) stream.push_back(static_cast<float>((i % 4) * 2 - 3));   // -3,-1,1,3,...
    stream.insert(stream.end(), 20, 0.f);
    int frames = 0;
    c.process(stream, 1000, [&](std::span<const float> pkt, uint64_t first) {
        ++frames;
        ASSERT_EQ(pkt.size(), 192u);
        EXPECT_EQ(first, 1050u);                      // provenance: 同期語先頭のシンボル番号
        for (std::size_t i = 0; i < 10; ++i) EXPECT_FLOAT_EQ(pkt[i], kSyncWord[i]);
        EXPECT_FLOAT_EQ(pkt[10], -3.f);
        EXPECT_FLOAT_EQ(pkt[191], static_cast<float>((181 % 4) * 2 - 3));
    });
    EXPECT_EQ(frames, 1);
    EXPECT_EQ(c.stats().detections, 1u);
    EXPECT_FALSE(c.collecting());
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

// ---- golden: 実機録音 ----
// 録音(<base>.sigmf-*)と期待値(<base>.golden.json)はリポジトリ外(個体の LO 誤差や無線機の識別符号を含むため)。
// 無ければ skip。自分の録音で golden を作るには: 録音 → spear-std-t98-decode で確認 → 抜粋と JSON を ~/spear/golden/std_t98/ に置く。
namespace {
std::string golden_base() {
    if (const char* d = std::getenv("SPEAR_GOLDEN_DIR")) return std::string(d) + "/std_t98/ch3_pich";
    if (const char* h = std::getenv("HOME")) return std::string(h) + "/spear/golden/std_t98/ch3_pich";
    return {};
}
struct Golden { double freq_err_hz = 0, squelch_db = -50; int channel = 0; std::string csm; int min_frames = 1, pich_frames = 0, min_sacch = 0; };
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
    g.squelch_db = std::stod(jvalue(s, "squelch_db"));
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
    cfg.squelch_db = g.squelch_db;
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
    // アイパターン: 1.2 s × 2400 baud ≈ 2900 トレース(squelch 開の間)、判定点の 90% 以上が ±1/±3 上
    EXPECT_GT(eye_traces, 2000u);
    std::printf("[ eye ] traces %zu, decision points on level %.1f%%\n", eye_traces, 100.0 * static_cast<double>(eye_on_level) / static_cast<double>(2 * eye_traces));
    EXPECT_GT(static_cast<double>(eye_on_level) / static_cast<double>(2 * eye_traces), 0.9);
    EXPECT_EQ(rx.metrics(g.channel).csm, g.csm);
    EXPECT_LT(rx.metrics(g.channel).best_sse, 1.0);
    for (int c = 0; c < cfg.num_channels; ++c) if (c != g.channel) EXPECT_EQ(rx.metrics(c).frames, 0u);
}
