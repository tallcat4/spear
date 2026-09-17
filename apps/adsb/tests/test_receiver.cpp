// ADS-B 受信機の検証。
//   1) 合成 PPM(規格のパルス配置で既知フレームを並べ、IF オフセット + 雑音を加えたもの)で 回転 → FIR → 振幅 → 検出 → CRC → provenance を
//      サンプル単位で確認する(合成は部品試験に留める: 変調器を発明していない、PPM は規格の定義そのもの)
//   2) golden: 実機録音(~/spear/golden/adsb/es.sigmf-*)を丸ごと通し、es.golden.txt(index hex の行)のフレームが全部出ること。無ければ skip
#include "aircraft.hpp"
#include "receiver.hpp"
#include "spear/core/sigmf.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <numbers>
#include <random>
#include <set>
#include <sstream>

using namespace spear;
using namespace spear::adsb;

namespace {
constexpr double kRate = 8e6;
constexpr int kSpc = 4;

struct Synth {
    std::vector<cf32> iq;
    std::mt19937 rng{7};
    explicit Synth(std::size_t n, float noise) : iq(n) {
        std::normal_distribution<float> g(0.f, noise);
        for (auto& x : iq) x = cf32(g(rng), g(rng));
    }
    // start から 1 フレーム(プリアンブル + PPM)を IF に載せる
    void add_frame(std::size_t start, const std::string& hex, float amp, double if_hz) {
        FrameBytes b{}; int nbits = 0;
        ASSERT_TRUE(from_hex(hex, b, &nbits));
        std::vector<uint8_t> chips(static_cast<std::size_t>(kPreambleChips + 2 * nbits), 0);
        for (int c : {0, 2, 7, 9}) chips[static_cast<std::size_t>(c)] = 1;
        for (int k = 0; k < nbits; ++k) {
            const bool bit = (b[static_cast<std::size_t>(k / 8)] >> (7 - k % 8)) & 1;
            chips[static_cast<std::size_t>(kPreambleChips + 2 * k + (bit ? 0 : 1))] = 1;
        }
        for (std::size_t c = 0; c < chips.size(); ++c) {
            if (!chips[c]) continue;
            for (int s = 0; s < kSpc; ++s) {
                const std::size_t i = start + c * kSpc + static_cast<std::size_t>(s);
                if (i >= iq.size()) return;
                const double ph = 2 * std::numbers::pi * if_hz * static_cast<double>(i) / kRate;
                iq[i] += amp * cf32(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
            }
        }
    }
};

struct Got { uint64_t index; std::string hex; Frame f; };

std::vector<Got> run(Receiver& rx, const std::vector<cf32>& iq, std::size_t block, uint64_t first_index = 0) {
    std::vector<Got> out;
    Observer o;
    o.frame = [&](const Frame& f, std::span<const float> w) {
        EXPECT_EQ(w.size(), static_cast<std::size_t>(kWindowChips * kSpc));
        out.push_back({f.input_sample_index, to_hex(f.bytes, f.nbits), f});
    };
    rx.set_observer(o);
    for (std::size_t i = 0; i < iq.size(); i += block) {
        const std::size_t n = std::min(block, iq.size() - i);
        rx.process(std::span<const cf32>(iq.data() + i, n), first_index + i);
    }
    return out;
}
}

TEST(AdsbReceiver, DecodesSyntheticFramesWithSampleAccurateProvenance) {
    Synth s(400000, 0.01f);
    ReceiverConfig cfg;
    const double if_hz = cfg.lo_offset_hz + 30e3;   // LO 誤差 30 kHz 相当。振幅復調なので影響しないはず
    const std::vector<std::pair<std::size_t, std::string>> frames = {
        {1000, "8D4840D6202CC371C32CE0576098"},
        {65536 - 300, "8D40621D58C382D690C8AC2863A7"},   // ブロック境界(65536)をまたぐ
        {150000, "8D40621D58C386435CC412692AD6"},
        {250000, "8D485020994409940838175B284F"},
    };
    for (const auto& [start, hex] : frames) s.add_frame(start, hex, 0.3f, if_hz);
    Receiver rx(cfg);
    const auto got = run(rx, s.iq, 65536, 1000000);
    ASSERT_EQ(got.size(), frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
        EXPECT_EQ(got[i].hex, frames[i].second);
        EXPECT_NEAR(static_cast<double>(got[i].index), static_cast<double>(1000000 + frames[i].first), 1.0) << "provenance: 元 sample index(FIR 群遅延を引く)";
        EXPECT_EQ(got[i].f.input_sample_end - got[i].f.input_sample_index, static_cast<uint64_t>((16 + 224) * kSpc));
        EXPECT_TRUE(got[i].f.accepted);
        EXPECT_GT(got[i].f.snr_db, 10);
    }
    EXPECT_EQ(rx.metrics().frames, 4u);
    EXPECT_EQ(rx.metrics().by_df[17], 4u);
    EXPECT_GE(rx.metrics().preambles, 4u);
}

TEST(AdsbReceiver, ApFormatNeedsKnownIcaoAndSingleBitFix) {
    Synth s(300000, 0.01f);
    ReceiverConfig cfg;
    cfg.fix_single_bit = true;
    // DF4(AP = ICAO 4840D6、高度 38000 ft): test_protocol と同じ組み立て
    FrameBytes d{}; d[0] = 4 << 3;
    const uint32_t nq = 1560, ac13 = ((nq & 0x7E0) << 2) | ((nq & 0x10) << 1) | (nq & 0xF) | 0x0010;
    d[2] = static_cast<uint8_t>((ac13 >> 8) & 0x1F); d[3] = static_cast<uint8_t>(ac13 & 0xFF);
    {
        FrameBytes z = d;
        const uint32_t p = crc_remainder(std::span<const uint8_t>(z.data(), 7), 56) ^ 0x4840D6u;
        d[4] = static_cast<uint8_t>(p >> 16); d[5] = static_cast<uint8_t>(p >> 8); d[6] = static_cast<uint8_t>(p);
    }
    const std::string df4 = to_hex(d, 56);
    s.add_frame(5000, df4, 0.3f, cfg.lo_offset_hz);                                   // まだ ICAO 未知 → 捨てる
    s.add_frame(50000, "8D4840D6202CC371C32CE0576098", 0.3f, cfg.lo_offset_hz);       // ICAO を学習
    s.add_frame(100000, df4, 0.3f, cfg.lo_offset_hz);                                 // 受理
    // 1 bit 誤り: bit 40 のパルスを逆側に置いたフレーム(ICAO 4840D6 は既知なので訂正して受理される)
    {
        FrameBytes b{}; int n = 0; from_hex("8D4840D6202CC371C32CE0576098", b, &n);
        b[5] ^= 0x80;
        s.add_frame(200000, to_hex(b, 112), 0.3f, cfg.lo_offset_hz);
    }
    // 1 bit 誤り + 未知 ICAO(485020): 訂正はできるが受理しない(誤訂正で幻の機体を作らない)
    {
        FrameBytes b{}; int n = 0; from_hex("8D485020994409940838175B284F", b, &n);
        b[5] ^= 0x80;
        s.add_frame(250000, to_hex(b, 112), 0.3f, cfg.lo_offset_hz);
    }
    Receiver rx(cfg);
    const auto got = run(rx, s.iq, 65536);
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0].hex, "8D4840D6202CC371C32CE0576098");
    EXPECT_EQ(got[1].hex, df4);
    EXPECT_EQ(got[1].f.msg.icao, 0x4840D6u);
    EXPECT_EQ(got[1].f.msg.altitude_ft, 38000);
    EXPECT_EQ(got[2].hex, "8D4840D6202CC371C32CE0576098");
    EXPECT_TRUE(got[2].f.fixed);
    // 閾値 6 dB では雑音からも候補が出る(CRC で落ちる)ので、捨てた数は下限だけ見る
    EXPECT_GE(rx.metrics().ap_unknown, 1u);
    EXPECT_EQ(rx.metrics().fixed, 1u);
    EXPECT_GE(rx.metrics().crc_bad, 1u) << "未知 ICAO の訂正フレームは crc_bad に数える";
}

TEST(AdsbReceiver, AircraftTableResolvesPositionFromPair) {
    AircraftTable table;
    table.set_reference(Position{52.0, 4.0});
    int created = 0, first_pos = 0;
    AircraftTable::Callbacks cb;
    cb.created = [&](const Aircraft&, const Frame&) { ++created; };
    cb.first_position = [&](const Aircraft&, const Frame&) { ++first_pos; };
    table.set_callbacks(cb);
    auto mk = [](const std::string& hex, double t) { Frame f; from_hex(hex, f.bytes, &f.nbits); f.msg = decode(f.bytes, f.nbits); f.t_s = t; f.accepted = true; f.rssi_db = -30; return f; };
    table.update(mk("8D4840D6202CC371C32CE0576098", 0));
    EXPECT_EQ(table.size(), 1u);
    EXPECT_EQ(table.all()[0].callsign, "KLM1023");
    table.update(mk("8D40621D58C382D690C8AC2863A7", 1.0));   // even のみ → 基準位置からの局所解
    ASSERT_EQ(table.size(), 2u);
    const Aircraft* a = table.find(0x40621D);
    ASSERT_NE(a, nullptr);
    ASSERT_TRUE(a->position.has_value());
    EXPECT_NEAR(a->position->lat, 52.25720, 1e-3);
    EXPECT_NEAR(a->position->lon, 3.91937, 1e-3);
    EXPECT_EQ(a->altitude_ft.value_or(0), 38000);
    ASSERT_TRUE(a->distance_km.has_value());
    EXPECT_NEAR(*a->distance_km, 29.2, 1.0);
    table.update(mk("8D40621D58C386435CC412692AD6", 1.5));   // odd → 前回位置からの局所解
    EXPECT_EQ(a->positions, 2u);
    EXPECT_EQ(created, 2);
    EXPECT_EQ(first_pos, 1);
    // 基準なしでも偶奇ペアで大域解が出る
    AircraftTable t2;
    t2.update(mk("8D40621D58C382D690C8AC2863A7", 10.0));
    EXPECT_FALSE(t2.find(0x40621D)->position.has_value());
    t2.update(mk("8D40621D58C386435CC412692AD6", 11.0));
    ASSERT_TRUE(t2.find(0x40621D)->position.has_value());
    EXPECT_NEAR(t2.find(0x40621D)->position->lat, 52.26578, 1e-3);
    // 期限切れ
    t2.expire(11.0 + 61);
    EXPECT_EQ(t2.size(), 0u);
}

// ---- golden: 実機録音 ----
namespace {
std::string golden_base() {
    if (const char* d = std::getenv("SPEAR_GOLDEN_DIR")) return std::string(d) + "/adsb/es";
    if (const char* h = std::getenv("HOME")) return std::string(h) + "/spear/golden/adsb/es";
    return "";
}
}

TEST(AdsbReceiver, GoldenRecordingDecodesAllExpectedFrames) {
    const std::string base = golden_base();
    sigmf::Meta meta;
    std::ifstream g(base + ".golden.txt");
    if (base.empty() || !sigmf::read(base, meta) || !g) GTEST_SKIP() << "golden 録音なし: " << base;
    std::set<std::pair<uint64_t, std::string>> expected;
    std::string line;
    while (std::getline(g, line)) {
        std::istringstream is(line);
        uint64_t idx; std::string hex;
        if (is >> idx >> hex) expected.insert({idx, hex});
    }
    ASSERT_FALSE(expected.empty());
    ReceiverConfig cfg;
    cfg.in_rate = meta.sample_rate;
    ASSERT_FALSE(meta.captures.empty());
    cfg.lo_offset_hz = 1090e6 - meta.captures[0].frequency;
    Receiver rx(cfg);
    std::set<std::pair<uint64_t, std::string>> got;
    Observer o;
    o.frame = [&](const Frame& f, std::span<const float>) { got.insert({f.input_sample_index, to_hex(f.bytes, f.nbits)}); };
    rx.set_observer(o);
    std::ifstream f(sigmf::data_path(base), std::ios::binary);
    ASSERT_TRUE(f);
    const std::size_t block = 65536;
    std::vector<sc16> raw(block);
    std::vector<cf32> iq(block);
    uint64_t idx = 0;
    while (f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(block * sizeof(sc16))) || f.gcount() > 0) {
        const std::size_t n = static_cast<std::size_t>(f.gcount()) / sizeof(sc16);
        for (std::size_t i = 0; i < n; ++i) iq[i] = cf32(raw[i].real() / 32768.f, raw[i].imag() / 32768.f);
        rx.process(std::span<const cf32>(iq.data(), n), idx);
        idx += n;
    }
    std::size_t missing = 0;
    for (const auto& e : expected)
        if (!got.count(e)) { ++missing; ADD_FAILURE() << "golden フレームが出ない: " << e.first << " " << e.second; }
    EXPECT_EQ(missing, 0u);
    EXPECT_GE(got.size(), expected.size());
}
