// 秘話(鍵探索)のテスト。
//   1) PN / スクランブル解除: std-t98-tools の Python(pn_sequence.py / secret_voice.py)と同じ値
//   2) ffnn(C++ 再実装)/ hybrid(ONNX Runtime)の logits が torch と一致(golden: secret_golden.txt、無ければ skip)
//   3) 鍵探索 E2E: 実録音の平文 TCH(ch3_payloads.txt)を既知鍵でスクランブルし、Python の SecretCracker と同じ鍵を返す
//   4) 要求ポリシー(Tracker)と Worker
// モデルはバイナリに埋め込み。golden(~/spear/golden/std_t98、SPEAR_GOLDEN_DIR)は音声由来でリポジトリ外、無ければ skip。
#include "ambe.hpp"
#include "receiver.hpp"
#include "spear/core/sigmf.hpp"
#include "ambe/bits.hpp"
#include "secret/cracker.hpp"
#include "secret/ffnn.hpp"
#include "secret/hybrid.hpp"
#include "secret/models.hpp"
#include "secret/silence.hpp"
#include "secret/pn.hpp"
#include "secret/tracker.hpp"
#include "secret/worker.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace spear::std_t98;
using namespace spear::std_t98::secret;

namespace {
std::string golden_dir() {
    if (const char* d = std::getenv("SPEAR_GOLDEN_DIR")) return std::string(d) + "/std_t98";
    if (const char* h = std::getenv("HOME")) return std::string(h) + "/spear/golden/std_t98";
    return {};
}
std::vector<uint8_t> from_hex(const std::string& h) {
    std::vector<uint8_t> v;
    for (std::size_t i = 0; i + 1 < h.size(); i += 2) v.push_back(static_cast<uint8_t>(std::stoul(h.substr(i, 2), nullptr, 16)));
    return v;
}
std::string to_hex(const uint8_t* p, std::size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) { s.push_back(d[p[i] >> 4]); s.push_back(d[p[i] & 15]); }
    return s;
}
std::string pn_hex(uint16_t key) {
    const Pn196 pn = pn_sequence(key);
    uint8_t packed[25] = {};
    for (int i = 0; i < kPnBits; ++i) packed[i / 8] |= static_cast<uint8_t>(pn[static_cast<std::size_t>(i)] << (7 - i % 8));
    return to_hex(packed, 25);
}
std::vector<uint8_t> hex_to_bits(const std::string& h, int nbits) {
    const auto bytes = from_hex(h);
    std::vector<uint8_t> b(static_cast<std::size_t>(nbits));
    for (int i = 0; i < nbits; ++i) b[static_cast<std::size_t>(i)] = static_cast<uint8_t>((bytes[static_cast<std::size_t>(i / 8)] >> (7 - i % 8)) & 1);
    return b;
}

struct Golden {
    struct Logit { std::vector<uint8_t> x; float l0, l1; };
    std::vector<Logit> ffnn, hybrid;
    struct Crack { int key, bursts, current, resolved, source; };
    std::vector<Crack> crack;
};
Golden load_golden() {
    Golden g;
    std::ifstream f(golden_dir() + "/secret_golden.txt");
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream is(line);
        std::string kind;
        is >> kind;
        if (kind == "ffnn" || kind == "hybrid") {
            std::string hx; float l0, l1;
            is >> hx >> l0 >> l1;
            (kind == "ffnn" ? g.ffnn : g.hybrid).push_back({hex_to_bits(hx, kInputDim), l0, l1});
        } else if (kind == "crack") {
            Golden::Crack c{};
            is >> c.key >> c.bursts >> c.current >> c.resolved >> c.source;
            g.crack.push_back(c);
        }
    }
    return g;
}
// 実録音の TCH ペイロード(3600)→ FEC → 2450(ThumbDV 順 49 bit × 4)
std::vector<Burst> load_real_bursts() {
    std::vector<Burst> out;
    std::ifstream f(golden_dir() + "/ch3_payloads.txt");
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream is(line);
        std::string ch, sym, hx;
        is >> ch >> sym >> hx;
        const auto bytes = from_hex(hx);
        if (bytes.size() != 36) continue;
        Burst b;
        for (int k = 0; k < 4; ++k) b[static_cast<std::size_t>(k)] = unpack_frame49(fec_demod_3600_to_2450(std::span<const uint8_t, 9>(bytes.data() + k * 9, 9)));
        out.push_back(b);
    }
    return out;
}
Burst scramble(const Burst& b, uint16_t key, uint8_t series = 0) {
    if (!key) return b;
    const Pn196 pn = pn_sequence(key, series);
    Burst out = b;
    for (int f = 0; f < kFramesPerBurst; ++f) {
        const Frame49 ks = keystream_thumbdv(pn, f);
        for (int i = 0; i < kBitsPerFrame; ++i) out[static_cast<std::size_t>(f)][static_cast<std::size_t>(i)] ^= ks[static_cast<std::size_t>(i)];
    }
    return out;
}
Burst measured_silence_burst() {
    Burst plain{};
    const char* samples[] = {"deba0301a6e600", "9f9ac301a64500", "de9cc909b6e600", "df9ac201a64c00"};
    for (int f = 0; f < 4; ++f) {
        const auto bits = hex_to_bits(samples[f], 49);
        std::copy(bits.begin(), bits.end(), plain[f].begin());
    }
    return plain;
}
// TSan では全鍵探索が約 25 倍遅い(0.6 s → 14 s)ので、E2E は先頭 2 ケースだけにして CI を伸ばさない(スレッドの検証は Worker のテストで足りる)
#if defined(__SANITIZE_THREAD__)
constexpr std::size_t kMaxCrackCases = 2;
#else
constexpr std::size_t kMaxCrackCases = 1000;
#endif
} // namespace

// ---------------------------------------------------------------- PN / スクランブル

TEST(StdT98Secret, PnSequenceMatchesPython) {
    // generate_pn_sequence_196(key) を packbits した値
    EXPECT_EQ(pn_hex(1), "80010006001400780110066015407f81010606141478791110");
    EXPECT_EQ(pn_hex(12345), "9c0d482fb0e1a245cd9cad4befb8619145679f5143e7885130");
    EXPECT_EQ(pn_hex(32767), "fffe00040018005001e0044019805501fe040418185051e1e0");
}

TEST(StdT98Secret, DescrambleMatchesPythonInBothBitOrders) {
    // descramble_burst(payloads, generate_pn_sequence_196(12345)) の出力
    const char* in[4] = {"be68d99b64b200", "0123456789abcd", "fedcba98765400", "5555aaaa5555aa"};
    const char* expect[4] = {"3f2391da5b0180", "38a4c4aa137700", "78a3992ed6c080", "cebb0a436f1d80"};
    const Pn196 pn = pn_sequence(12345);
    for (int f = 0; f < 4; ++f) {
        std::array<uint8_t, 7> payload{};
        const auto bytes = from_hex(in[f]);
        std::copy(bytes.begin(), bytes.end(), payload.begin());
        // ThumbDV 順(モデル入力順): bit i ^= pn[f*49 + kThumbDv[i]]
        Frame49 bits = unpack_frame49(payload);
        const Frame49 ks = keystream_thumbdv(pn, f);
        for (int i = 0; i < kBitsPerFrame; ++i) bits[static_cast<std::size_t>(i)] ^= ks[static_cast<std::size_t>(i)];
        const auto packed = pack_frame49(bits);
        EXPECT_EQ(to_hex(packed.data(), 7), expect[f]) << "frame " << f;
        // mbelib d 順(AmbeDecoder が使う経路): d[j] ^= pn[f*49 + j] — 同じ結果になる(kThumbDv が両者の対応表だから)
        ambe::Bits49 d = ambe::unpack_thumbdv(payload);
        const uint8_t* ksd = keystream_d(pn, f);
        for (std::size_t j = 0; j < d.size(); ++j) d[j] = static_cast<uint8_t>(d[j] ^ ksd[j]);
        const auto packed_d = ambe::pack_thumbdv(d);
        EXPECT_EQ(to_hex(packed_d.data(), 7), expect[f]) << "frame " << f << " (d order)";
    }
}

TEST(StdT98Secret, DecoderReturnsRawPayloadAndAppliesKeystreamAfterFec) {
    const auto bursts_hex = [] {
        std::ifstream f(golden_dir() + "/ch3_payloads.txt");
        std::string line, ch, sym, hx;
        std::getline(f, line);
        std::istringstream(line) >> ch >> sym >> hx;
        return hx;
    }();
    if (bursts_hex.size() != 72) GTEST_SKIP() << "ch3_payloads golden not present";
    const auto bytes = from_hex(bursts_hex);
    const std::span<const uint8_t, 9> block(bytes.data(), 9);
    AmbeDecoder plain, keyed, zero;
    std::array<int16_t, 160> p1{}, p2{}, p3{};
    const auto r1 = plain.decode_3600(block, p1);
    const Pn196 pn = pn_sequence(4242);
    const auto r2 = keyed.decode_3600(block, p2, keystream_d(pn, 0));
    const Pn196 zeros{};
    zero.decode_3600(block, p3, zeros.data());
    EXPECT_EQ(r1.raw2450, fec_demod_3600_to_2450(block));
    EXPECT_EQ(r2.raw2450, r1.raw2450) << "raw2450 は XOR 前の値";
    EXPECT_EQ(r2.total_errors, r1.total_errors);
    EXPECT_EQ(p3, p1) << "全 0 の keystream は平文と同じ";
    EXPECT_NE(p2, p1) << "鍵つきは別の音声になる";
}

// ---------------------------------------------------------------- モデル

TEST(StdT98Secret, FfnnLogitsMatchTorch) {
    const Golden g = load_golden();
    if (g.ffnn.empty()) GTEST_SKIP() << "secret golden not present";
    Ffnn ffnn;
    std::string err;
    ASSERT_TRUE(ffnn.load_embedded(&err)) << err;
    EXPECT_EQ(ffnn.hidden(), 128);
    float max_diff = 0;
    for (const auto& c : g.ffnn) {
        float l[2];
        ffnn.logits(c.x.data(), l);
        max_diff = std::max({max_diff, std::abs(l[0] - c.l0), std::abs(l[1] - c.l1)});
        EXPECT_EQ(l[0] > l[1], c.l0 > c.l1) << "argmax differs";
    }
    EXPECT_LT(max_diff, 1e-3f) << "float の加算順の差だけのはず";
    std::printf("ffnn: %zu cases, max |dlogit| = %.3g\n", g.ffnn.size(), static_cast<double>(max_diff));
}

TEST(StdT98Secret, HybridLogitsMatchTorch) {
    const Golden g = load_golden();
    if (g.hybrid.empty()) GTEST_SKIP() << "secret golden not present";
    Hybrid h;
    std::string err;
    ASSERT_TRUE(h.load_embedded(&err)) << err;
    std::vector<float> in(g.hybrid.size() * kInputDim), out(g.hybrid.size() * 2);
    for (std::size_t j = 0; j < g.hybrid.size(); ++j)
        for (int i = 0; i < kInputDim; ++i) in[j * kInputDim + static_cast<std::size_t>(i)] = static_cast<float>(g.hybrid[j].x[static_cast<std::size_t>(i)]);
    ASSERT_TRUE(h.logits(in.data(), g.hybrid.size(), out.data(), &err)) << err;
    float max_diff = 0;
    for (std::size_t j = 0; j < g.hybrid.size(); ++j) {
        max_diff = std::max({max_diff, std::abs(out[j * 2] - g.hybrid[j].l0), std::abs(out[j * 2 + 1] - g.hybrid[j].l1)});
        EXPECT_EQ(out[j * 2] > out[j * 2 + 1], g.hybrid[j].l0 > g.hybrid[j].l1) << "argmax differs at " << j;
    }
    EXPECT_LT(max_diff, 1e-3f);
    std::printf("hybrid: %zu cases, max |dlogit| = %.3g\n", g.hybrid.size(), static_cast<double>(max_diff));
}

// ---------------------------------------------------------------- 鍵探索 E2E

TEST(StdT98Secret, AnalyticSilenceBypassesModelsAndUpdatesCache) {
    // No models loaded: the analytical path must not depend on NN inference.
    Ffnn ffnn;
    Hybrid hybrid;
    Cracker cracker(ffnn, hybrid);
    const auto plain = measured_silence_burst();
    const auto encrypted = scramble(plain, 12345);
    const auto result = cracker.resolve(777, std::array<Burst, 1>{encrypted});
    EXPECT_EQ(result.key, 12345);
    EXPECT_EQ(result.source, ResultSource::AnalyticSilence);
    EXPECT_STREQ(to_string(result.source), "analytic silence hit");
    EXPECT_EQ(result.cache, (std::vector<uint16_t>{12345}));
    const auto clear = cracker.resolve(12345, std::array<Burst, 1>{plain});
    EXPECT_EQ(clear.key, 0);
    EXPECT_EQ(clear.source, ResultSource::None);
    EXPECT_EQ(clear.cache, result.cache);
    const std::atomic<bool> cancel{true};
    EXPECT_EQ(cracker.resolve(0, std::array<Burst, 1>{encrypted}, &cancel).key, 0);
    EXPECT_EQ(cracker.cache(), result.cache);
}

TEST(StdT98Secret, SolverOnlyRejectsNonSilenceEvenWithCurrentAndCachedKey) {
    Ffnn ffnn;
    Hybrid hybrid;
    Cracker cracker(ffnn, hybrid);
    ASSERT_EQ(cracker.resolve(0, std::array<Burst, 1>{scramble(measured_silence_burst(), 12345)}).key, 12345);
    Burst plain{};
    const auto bits = hex_to_bits("deba0381a6e600", 49);
    for (auto& f : plain) std::copy(bits.begin(), bits.end(), f.begin());
    const std::vector<Burst> bursts(5, scramble(plain, 12345));
    ASSERT_FALSE(recover_silence_key(bursts));
    for (const uint16_t current : {uint16_t{0}, uint16_t{12345}}) {
        const auto result = cracker.resolve(current, bursts);
        EXPECT_EQ(result.key, 0);
        EXPECT_EQ(result.source, ResultSource::None);
    }
}

TEST(StdT98Secret, NonSilenceStillUsesCurrentKeyVerification) {
    Ffnn ffnn;
    Hybrid hybrid;
    std::string err;
    ASSERT_TRUE(ffnn.load_embedded(&err)) << err;
    ASSERT_TRUE(hybrid.load_embedded(&err)) << err;
    Cracker cracker(ffnn, hybrid, {.ai_fallback = true});
    // 非ゼロゲインの通常フレームは解析対象外。AI 明示指定の経路も維持する。
    Burst plain{};
    const auto bits = hex_to_bits("deba0381a6e600", 49);
    for (auto& f : plain) std::copy(bits.begin(), bits.end(), f.begin());
    const std::vector<Burst> bursts(5, scramble(plain, 12345));
    ASSERT_FALSE(recover_silence_key(bursts));
    ASSERT_GT(cracker.ffnn_margin(Cracker::to_blocks(bursts)[0], 12345), 0.f);
    const auto result = cracker.resolve(12345, bursts);
    EXPECT_EQ(result.key, 12345);
    EXPECT_EQ(result.source, ResultSource::CurrentKey);
    const auto cached = cracker.resolve(0, bursts);
    EXPECT_EQ(cached.key, 12345);
    EXPECT_EQ(cached.source, ResultSource::GlobalCache);
}

TEST(StdT98Secret, CrackerRecoversKeyFromScrambledRealSpeech) {
    const Golden g = load_golden();
    const auto real = load_real_bursts();
    if (g.crack.empty() || real.size() < 10) GTEST_SKIP() << "secret golden / payloads not present";
    Ffnn ffnn;
    Hybrid hybrid;
    std::string err;
    ASSERT_TRUE(ffnn.load_embedded(&err)) << err;
    ASSERT_TRUE(hybrid.load_embedded(&err)) << err;
    Cracker cracker(ffnn, hybrid, {.ai_fallback = true});
    std::size_t n = 0;
    for (const auto& c : g.crack) {
        if (n++ >= kMaxCrackCases) break;
        cracker.clear_cache();
        std::vector<Burst> bursts;
        for (int i = 0; i < c.bursts; ++i) bursts.push_back(scramble(real[static_cast<std::size_t>(i)], static_cast<uint16_t>(c.key)));
        const auto t0 = std::chrono::steady_clock::now();
        const Resolution r = cracker.resolve(static_cast<uint16_t>(c.current), bursts);
        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("crack key=%5d bursts=%2d current=%5d -> %5u (%s) %.2f s  [python: %d src %d]\n", c.key, c.bursts, c.current, r.key, to_string(r.source), dt, c.resolved, c.source);
        EXPECT_EQ(r.key, c.resolved) << "key " << c.key;
        // A silence hit may now bypass the legacy path; key identity must
        // still match the independent Python golden in every case.
        if (r.source != ResultSource::AnalyticSilence) {
            EXPECT_EQ(static_cast<int>(r.source), c.source) << "key " << c.key;
        }
    }
}

// ---------------------------------------------------------------- ポリシー / ワーカー

TEST(StdT98Secret, TrackerRequestsAt5ThenEvery10WithFullWindow) {
    Tracker t({.recheck_interval = 10});
    Burst b{};
    Tracker::Request rq;
    EXPECT_EQ(t.status(), TrackerStatus::Idle);
    for (int i = 0; i < 4; ++i) EXPECT_FALSE(t.on_secret_burst(b, &rq)) << i;
    EXPECT_EQ(t.status(), TrackerStatus::Collecting);
    EXPECT_TRUE(t.on_secret_burst(b, &rq));           // 5 個目で最初の要求(窓 5)
    EXPECT_EQ(rq.bursts.size(), 5u);
    EXPECT_EQ(rq.session, 1u);
    EXPECT_EQ(t.status(), TrackerStatus::Pending);
    EXPECT_FALSE(t.on_secret_burst(b, &rq));          // pending 中は出さない
    t.on_result(99, 123);                             // 別セッションの結果は無視
    EXPECT_TRUE(t.pending());
    t.on_result(1, 1234);
    EXPECT_EQ(t.key(), 1234);
    EXPECT_EQ(t.status(), TrackerStatus::Keyed);
    // 再確認: 前回要求(index 4)から 10 バースト後(index 14)、窓が 10 に満ちていること(index 5 は上の pending 中の 1 つ)
    for (int i = 6; i <= 13; ++i) EXPECT_FALSE(t.on_secret_burst(b, &rq)) << i;
    EXPECT_TRUE(t.on_secret_burst(b, &rq));
    EXPECT_EQ(rq.bursts.size(), 10u);
    EXPECT_EQ(rq.current_key, 1234);
    t.on_result(1, 0);                                // 見つからず: 鍵は保持、状態は Keyed のまま(鍵 > 0)
    EXPECT_EQ(t.key(), 1234);
    EXPECT_EQ(t.status(), TrackerStatus::Keyed);
    // 平文に戻る → セッション終了、鍵は保持。次の秘話呼は新セッションで最初から
    t.on_clear();
    EXPECT_EQ(t.status(), TrackerStatus::Idle);
    EXPECT_EQ(t.key(), 1234);
    for (int i = 0; i < 4; ++i) EXPECT_FALSE(t.on_secret_burst(b, &rq));
    EXPECT_TRUE(t.on_secret_burst(b, &rq));
    EXPECT_EQ(rq.session, 2u);
    EXPECT_EQ(rq.current_key, 1234);
    t.on_result(2, 0);
    EXPECT_EQ(t.status(), TrackerStatus::Keyed) << "鍵を持っているので miss でも keyed";
}

TEST(StdT98Secret, TrackerMissWithoutKeyIsReported) {
    Tracker t({.recheck_interval = 10});
    Burst b{};
    Tracker::Request rq;
    for (int i = 0; i < 5; ++i) t.on_secret_burst(b, &rq);
    t.on_result(1, 0);
    EXPECT_EQ(t.status(), TrackerStatus::Miss);
}

TEST(StdT98Secret, EmbeddedModelsLoad) {
    Ffnn ffnn;
    Hybrid hybrid;
    std::string err;
    EXPECT_TRUE(ffnn.load_embedded(&err)) << err;
    EXPECT_TRUE(hybrid.load_embedded(&err)) << err;
    EXPECT_EQ(ffnn.hidden(), 128);
    // 埋め込みの safetensors ヘッダが壊れていれば load は false を返す(落ちない)
    const auto bytes = embedded_ffnn_safetensors();
    std::vector<uint8_t> broken(bytes.begin(), bytes.begin() + 64);
    Ffnn f2;
    EXPECT_FALSE(f2.load(broken, &err));
    EXPECT_FALSE(err.empty());
}

TEST(StdT98Secret, WorkerStopsCleanlyDuringSearch) {
    // 探索中に破棄しても打ち切って戻る(App 停止で GUI thread を待たせない)
    const auto real = load_real_bursts();
    if (real.size() < 5) GTEST_SKIP() << "payloads not present";
    const auto t0 = std::chrono::steady_clock::now();
    {
        Worker w([](const Worker::Result&) {});
        w.wait_ready();
        ASSERT_TRUE(w.status().ready) << w.status().error;
        Worker::Request rq;
        for (int i = 0; i < 5; ++i) rq.bursts.push_back(scramble(real[static_cast<std::size_t>(i)], 31337));
        w.submit(rq);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_LT(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), 5.0);
}

TEST(StdT98Secret, WorkerReturnsGuidedResultWithoutExternalGolden) {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<Worker::Result> results;
    Worker w([&](const Worker::Result& r) { std::lock_guard<std::mutex> lk(mu); results.push_back(r); cv.notify_all(); });
    w.wait_ready();
    ASSERT_TRUE(w.status().ready) << w.status().error;
    Worker::Request rq;
    rq.channel = 96; rq.session = 7; // S15: last channel of the expanded plan
    rq.bursts.assign(10, scramble(measured_silence_burst(), 31337));
    w.submit(rq);
    std::unique_lock<std::mutex> lk(mu);
    ASSERT_TRUE(cv.wait_for(lk, std::chrono::seconds(10), [&] { return !results.empty(); }));
    EXPECT_EQ(results[0].channel, 96);
    EXPECT_EQ(results[0].session, 7u);
    EXPECT_EQ(results[0].key, 31337);
    EXPECT_EQ(results[0].source, ResultSource::GuidedAI);
    EXPECT_TRUE(results[0].accepted);
    std::printf("worker: guided AI %.6f s\n", results[0].seconds);
}

TEST(StdT98Secret, WorkerResolvesSubmittedRequestOnItsOwnThread) {
    const auto real = load_real_bursts();
    if (real.size() < 5) GTEST_SKIP() << "payloads not present";
    std::mutex mu;
    std::condition_variable cv;
    std::vector<Worker::Result> results;
    Worker w([&](const Worker::Result& r) { std::lock_guard<std::mutex> lk(mu); results.push_back(r); cv.notify_all(); });
    w.wait_ready();
    ASSERT_TRUE(w.status().ready) << w.status().error;
    Worker::Request rq;
    rq.channel = 2; rq.session = 7; rq.current_key = 0;
    for (int i = 0; i < 5; ++i) rq.bursts.push_back(scramble(real[static_cast<std::size_t>(i)], 31337));
    w.submit(rq);
    std::unique_lock<std::mutex> lk(mu);
    ASSERT_TRUE(cv.wait_for(lk, std::chrono::seconds(60), [&] { return !results.empty(); }));
    EXPECT_EQ(results[0].channel, 2);
    EXPECT_EQ(results[0].session, 7u);
    // Five bursts cannot provide the two disjoint AI verification blocks.
    EXPECT_FALSE(results[0].accepted);
    EXPECT_EQ(results[0].source, ResultSource::None);
    std::printf("worker: %s %.2f s\n", to_string(results[0].source), results[0].seconds);
}

// 実録音はリポジトリ外。3 回の PTT をそれぞれ鍵なしから復元する。
TEST(StdT98Secret, RecordedThreePttRecovers32767UsingSolverOnly) {
    const char* path = std::getenv("SPEAR_STD_T98_SECRET_IQ");
    const char* home = std::getenv("HOME");
    const std::string base = path ? path : std::string(home ? home : "") + "/spear/recordings/rec_20261021_035250";
    spear::sigmf::Meta meta;
    std::string err;
    if (!spear::sigmf::read(base, meta, &err)) GTEST_SKIP() << "secret IQ recording not present: " << base;
    ASSERT_EQ(meta.sample_rate, 4000000);
    ReceiverConfig cfg;
    cfg.in_rate = meta.sample_rate; cfg.pfb_channels = 64; cfg.freq_err_hz = 250000;
    Receiver rx(cfg);
    Ffnn ffnn;
    Hybrid hybrid;
    Cracker cracker(ffnn, hybrid);  // AI モデルはロードしない
    Tracker tracker;
    int calls = 0, recovered = 0;
    bool secret_call = false, have_call = false, resolved = false;
    uint64_t last_sym = 0;
    Observer obs;
    obs.frame = [&](const Frame& f) {
        if (f.channel != 8) return;
        if (f.rich.f == 0 || (last_sym && f.symbol_index > last_sym + 4*192)) {
            tracker = Tracker(); secret_call = false; have_call = false; resolved = false;
            cracker.clear_cache();
        }
        last_sym = f.symbol_index;
        if (f.rich.f != 1 || f.tch_payload.size() != 36) return;
        if (f.sacch.crc_ok) secret_call = f.sacch.call_stat == 1;
        if (!secret_call) return;
        if (!have_call) { ++calls; have_call = true; }
        Burst burst{};
        for (int phase = 0; phase < 4; ++phase)
            burst[phase] = unpack_frame49(fec_soft_3600_to_2450(std::span<const int8_t,72>(f.tch_llr.data()+phase*72,72)));
        Tracker::Request request;
        if (!tracker.on_secret_burst(burst, &request)) return;
        const auto t0 = std::chrono::steady_clock::now();
        const auto result = cracker.resolve(request.current_key, request.bursts);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
        EXPECT_TRUE(result.key == 0 || result.key == 32767);
        if (result.key) {
            EXPECT_EQ(result.source, ResultSource::AnalyticSilence);
            if (!resolved) { ++recovered; resolved = true; }
            std::printf("PTT %d: solver key=%u at symbol=%llu, %.6f s\n", calls, result.key, static_cast<unsigned long long>(f.symbol_index), seconds);
        }
        tracker.on_result(request.session, result.key);
    };
    rx.set_observer(obs);
    std::ifstream input(spear::sigmf::data_path(base), std::ios::binary);
    ASSERT_TRUE(input.good());
    std::vector<spear::sc16> raw(65536);
    std::vector<spear::cf32> iq(raw.size());
    uint64_t index = 0;
    while (input.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()*sizeof(spear::sc16))) || input.gcount()>0) {
        const auto n = static_cast<std::size_t>(input.gcount()) / sizeof(spear::sc16);
        for (std::size_t i = 0; i < n; ++i) iq[i] = spear::cf32(raw[i].real()/32768.f, raw[i].imag()/32768.f);
        rx.process(std::span<const spear::cf32>(iq.data(), n), index);
        index += n;
    }
    EXPECT_EQ(calls, 3);
    EXPECT_EQ(recovered, 3);
}

TEST(StdT98Secret, GuidedWorkerEnhancedSeriesOverlapPttAndOverrun) {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<Worker::Result> results;
    Worker w([&](const Worker::Result& r) { std::lock_guard<std::mutex> lk(mu); results.push_back(r); cv.notify_all(); });
    w.wait_ready();
    ASSERT_TRUE(w.status().ready) << w.status().error;
    auto submit = [&](Worker::Request request) {
        std::unique_lock<std::mutex> lk(mu);
        const auto size = results.size();
        w.submit(std::move(request));
        if (!cv.wait_for(lk, std::chrono::seconds(10), [&] { return results.size() > size; })) {
            ADD_FAILURE() << "worker timeout";
            return Worker::Result{};
        }
        return results.back();
    };
    for (uint8_t series = 0; series < 16; ++series) {
        Worker::Request r;
        r.channel = series;
        r.session = 1;
        r.bursts.assign(5, scramble(measured_silence_burst(), 12345, series));
        EXPECT_FALSE(submit(r).accepted);
        EXPECT_FALSE(submit(r).accepted) << "overlap must not count twice";
        r.first_burst = 5;
        const auto result = submit(r);
        EXPECT_TRUE(result.accepted) << "series " << int(series);
        EXPECT_EQ(result.series, series);
        EXPECT_EQ(result.key, 12345);
        r.first_burst = 100;
        EXPECT_FALSE(submit(r).accepted) << "gap must reset evidence";
        r.session = 2;
        r.first_burst = 0;
        EXPECT_FALSE(submit(r).accepted) << "PTT must reset evidence";
        r.first_burst = 5;
        r.valid.assign(5, {true, false, true, true});
        EXPECT_FALSE(submit(r).accepted) << "invalid bursts cannot form AI blocks";
    }
}
