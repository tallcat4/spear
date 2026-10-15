// JR TRAIN RADIO の DSP / フレーム同期のテスト。
//   1. 参照ベクトル: tests/refgen(原本の Rust 実装)が出した結果と、同じ IQ を C++ に通した結果を比べる。
//   2. 原本の単体テスト(sequence.rs / signal.rs / tests/protocol.rs / lib.rs)の移植。
#include "dsp/detail.hpp"
#include "dsp/jr_dsp.hpp"
#include "protocol/frame_sync.hpp"
#include "platform.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <numbers>
#include <sstream>
#include <string>
#include <vector>

#ifndef JR_TRAIN_VECTORS_DIR
#error "JR_TRAIN_VECTORS_DIR must be defined"
#endif

namespace {

using namespace spear::jr_train;
using dsp::detail::C32;
using cf = std::complex<float>;

std::string vec_path(const std::string& name) { return std::string(JR_TRAIN_VECTORS_DIR) + "/" + name; }

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

struct Reference {
    std::map<std::string, std::string> values;
    struct F {
        uint64_t bit_offset;
        int sync;
        uint32_t errors;
        std::string hex;
    };
    std::vector<F> frames;
    double num(const std::string& k) const { return std::stod(values.at(k)); }
};

Reference read_result(const std::string& path) {
    Reference r;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream is(line);
        std::string key;
        is >> key;
        if (key == "frame") {
            Reference::F fr{};
            is >> fr.bit_offset >> fr.sync >> fr.errors >> fr.hex;
            r.frames.push_back(fr);
        } else {
            std::string v;
            is >> v;
            r.values[key] = v;
        }
    }
    return r;
}

std::string hex(const std::array<uint8_t, 48>& b) {
    std::string s;
    char buf[3];
    for (uint8_t x : b) {
        std::snprintf(buf, sizeof buf, "%02x", x);
        s += buf;
    }
    return s;
}

// ---- 1. 参照ベクトル ----

TEST(JrTrainDsp, MatchesRustReference) {
    std::ifstream meta(vec_path("meta.txt"));
    ASSERT_TRUE(meta) << "missing " << vec_path("meta.txt");
    double rate = 0, center = 0;
    std::size_t samples = 0, block = 0;
    std::vector<std::pair<int, double>> channels;
    std::string line;
    while (std::getline(meta, line)) {
        std::istringstream is(line);
        std::string key;
        is >> key;
        if (key == "rate") is >> rate;
        else if (key == "center") is >> center;
        else if (key == "samples") is >> samples;
        else if (key == "block") is >> block;
        else if (key == "channel") {
            int c;
            double f;
            is >> c >> f;
            channels.emplace_back(c, f);
        }
    }
    const auto raw = read_file(vec_path("band_240k.sc16"));
    ASSERT_EQ(raw.size(), samples * 4);
    std::vector<cf> iq(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        const auto re = static_cast<int16_t>(raw[4 * i] | (raw[4 * i + 1] << 8));
        const auto im = static_cast<int16_t>(raw[4 * i + 2] | (raw[4 * i + 3] << 8));
        iq[i] = {static_cast<float>(re) / 32768.f, static_cast<float>(im) / 32768.f};
    }
    for (const auto& [c, freq] : channels) {
        SCOPED_TRACE("channel " + std::to_string(c));
        const auto ref = read_result(vec_path("ch" + std::to_string(c) + "_result.txt"));
        const auto ref_dibits = read_file(vec_path("ch" + std::to_string(c) + "_dibits.bin"));
        dsp::Demodulator demod(rate, freq - center);
        protocol::Synchronizer sync;
        std::vector<uint8_t> dibits, buf;
        std::vector<protocol::Frame> frames;
        for (std::size_t i = 0; i < samples; i += block) {
            buf.clear();
            demod.process(std::span<const cf>(iq).subspan(i, std::min(block, samples - i)), buf);
            for (uint8_t d : buf) sync.push(d, frames);
            dibits.insert(dibits.end(), buf.begin(), buf.end());
        }
        // 参照ベクトルを作った環境ではすべてビット一致。別の環境(libm が違う)では許容差で比べる(tests/platform.hpp)
        const bool exact = test::same_platform(JR_TRAIN_VECTORS_DIR);
        ASSERT_EQ(dibits.size(), ref_dibits.size());
        std::size_t mismatched = 0;
        for (std::size_t i = 0; i < dibits.size(); ++i) mismatched += dibits[i] != ref_dibits[i];
        const auto snap = demod.snapshot();
        std::vector<cf> points;
        const double evm = demod.read_signal(points, 256);   // refgen は原本 Dsp::signal と同じ 256 点
        const auto ref_points = read_file(vec_path("ch" + std::to_string(c) + "_constellation.cf32"));
        float max_diff = 0.f;
        if (points.size() * 8 == ref_points.size()) {
            for (std::size_t i = 0; i < points.size(); ++i) {
                float re, im;
                std::memcpy(&re, &ref_points[8 * i], 4);
                std::memcpy(&im, &ref_points[8 * i + 4], 4);
                max_diff = std::max({max_diff, std::abs(points[i].real() - re), std::abs(points[i].imag() - im)});
            }
        }
        if (exact) {
            EXPECT_EQ(mismatched, 0u);
            ASSERT_EQ(frames.size(), ref.frames.size());
            for (std::size_t i = 0; i < frames.size(); ++i) {
                EXPECT_EQ(frames[i].bit_offset, ref.frames[i].bit_offset);
                EXPECT_EQ(frames[i].sync_matched, ref.frames[i].sync != 0);
                EXPECT_EQ(frames[i].marker_errors, ref.frames[i].errors);
                EXPECT_EQ(hex(frames[i].received), ref.frames[i].hex);
            }
            EXPECT_EQ(sync.synchronized_frames, static_cast<uint64_t>(ref.num("synchronized_frames")));
            EXPECT_EQ(sync.locked, ref.num("frame_locked") != 0);
            EXPECT_EQ(snap.symbols, static_cast<int64_t>(ref.num("symbols")));
            EXPECT_EQ(snap.locked_symbols, static_cast<int64_t>(ref.num("locked_symbols")));
            EXPECT_EQ(snap.carrier_locked, static_cast<int32_t>(ref.num("carrier_locked")));
            EXPECT_NEAR(snap.carrier_error, ref.num("carrier_error"), 1e-6);
            EXPECT_NEAR(snap.frequency_offset_hz, ref.num("frequency_offset_hz"), 1e-3);
            EXPECT_NEAR(evm, ref.num("evm"), 1e-4);
            ASSERT_EQ(points.size() * 8, ref_points.size());
            EXPECT_LT(max_diff, 1e-4f);
        } else {
            // 雑音の多い区間で判定が数個変わりうる。同期の結果と全体の値は保たれること
            EXPECT_LE(mismatched, dibits.size() / 100);
            const auto ref_synced = static_cast<int64_t>(ref.num("synchronized_frames"));
            EXPECT_NEAR(static_cast<double>(sync.synchronized_frames), static_cast<double>(ref_synced), 2.0);
            EXPECT_EQ(snap.symbols, static_cast<int64_t>(ref.num("symbols")));
            EXPECT_NEAR(snap.frequency_offset_hz, ref.num("frequency_offset_hz"), 5.0);
            EXPECT_NEAR(evm, ref.num("evm"), 2.0);
            std::printf("  ch%d: vectors from another platform: %zu / %zu dibits differ\n", c, mismatched, dibits.size());
        }
        // 同じ環境なら完全一致するはず(参考表示)
        std::printf("  ch%d: dibits %zu frames %zu synced %llu evm %.4f%% (ref %.4f%%) max point diff %g\n", c, dibits.size(), frames.size(),
                    static_cast<unsigned long long>(sync.synchronized_frames), evm, ref.num("evm"), static_cast<double>(max_diff));
    }
}

// 原本 lib.rs chunk_boundaries_and_fresh_sessions_preserve_the_complete_output 相当
TEST(JrTrainDsp, ChunkBoundariesDoNotChangeOutput) {
    std::vector<cf> input(30000);
    uint32_t rng = 0x1234567u;
    double phase = 0;
    for (std::size_t i = 0; i < input.size(); ++i) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        if (i % 10 == 0) phase += std::numbers::pi / 4 + (rng >> 30) * std::numbers::pi / 2;
        input[i] = {static_cast<float>(0.3 * std::cos(phase)), static_cast<float>(0.3 * std::sin(phase))};
    }
    for (double rate : {48000.0, 240000.0, 1'024'000.0}) {   // 1.024 MS/s は CIC 経路(÷4)
        std::vector<uint8_t> whole, pieces;
        dsp::Demodulator a(rate, 1000.0), b(rate, 1000.0);
        a.process(input, whole);
        for (std::size_t i = 0; i < input.size(); i += 7) b.process(std::span<const cf>(input).subspan(i, std::min<std::size_t>(7, input.size() - i)), pieces);
        EXPECT_EQ(whole, pieces) << rate;
        const auto sa = a.snapshot(), sb = b.snapshot();
        EXPECT_EQ(sa.symbols, sb.symbols);
        EXPECT_EQ(sa.carrier_error, sb.carrier_error);
    }
}

TEST(JrTrainDsp, RejectsInvalidArguments) {
    EXPECT_THROW(dsp::Demodulator(9000.0, 0.0), std::invalid_argument);
    EXPECT_THROW(dsp::Demodulator(48000.0, 20000.0), std::invalid_argument);
    EXPECT_THROW(dsp::Demodulator(48000.0, 0.0, 2), std::invalid_argument);
    dsp::Demodulator d(48000.0, 0.0);
    std::vector<cf> bad = {{0.f, 0.f}, {std::nanf(""), 0.f}};
    std::vector<uint8_t> out;
    EXPECT_THROW(d.process(bad, out), std::invalid_argument);
    EXPECT_EQ(d.snapshot().symbols, 0);
}

// ---- 2. sequence.rs の単体テスト ----

constexpr float kL = 0.70710677f;
const C32 kSym[4] = {{kL, kL}, {-kL, kL}, {-kL, -kL}, {kL, -kL}};
constexpr uint8_t kGray[4] = {0, 1, 3, 2};

std::vector<std::size_t> qpsk_symbols(std::size_t count) {
    uint32_t state = 0x81234567u;
    std::vector<std::size_t> out(count);
    for (auto& s : out) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        s = state >> 30;
    }
    return out;
}

TEST(JrTrainSequence, DiagnosticCancelsDelayedPathsAndComplexGainWithoutSlicing) {
    dsp::detail::Sequence det;
    const std::size_t prior = 2 + 4 + 16 * 3;
    det.costs.fill(INFINITY);
    det.costs[prior] = 0.f;
    const std::array<C32, 4> channel = {C32{0.6f, 0.3f}, C32{0.4f, -0.2f}, C32{-0.2f, 0.15f}, C32{0.1f, 0.05f}};
    det.channels[prior] = channel;
    const C32 expected{kSym[1].re + 0.12f, kSym[1].im - 0.07f};
    const C32 sent[4] = {expected, kSym[2], kSym[1], kSym[3]};
    C32 received{};
    for (int k = 0; k < 4; ++k) {
        received.re += channel[k].re * sent[k].re - channel[k].im * sent[k].im;
        received.im += channel[k].re * sent[k].im + channel[k].im * sent[k].re;
    }
    EXPECT_FALSE(det.equalized());
    EXPECT_FALSE(det.process(received, true).dibit);
    const auto actual = det.equalized();
    ASSERT_TRUE(actual);
    EXPECT_NEAR(actual->re, expected.re, 1e-6);
    EXPECT_NEAR(actual->im, expected.im, 1e-6);
    det.process(received, false);
    EXPECT_FALSE(det.equalized());
}

TEST(JrTrainSequence, DiagnosticDoesNotInvertUnusableMainTap) {
    for (float gain : {0.f, 1e-4f}) {
        dsp::detail::Sequence det;
        for (auto& ch : det.channels) ch[0].re = gain;
        det.process(kSym[0], true);
        EXPECT_FALSE(det.equalized());
    }
}

TEST(JrTrainSequence, RandomSymbolsSurviveIsiAndMissingInterval) {
    const auto tx = qpsk_symbols(6000);
    dsp::detail::Sequence det;
    std::vector<uint8_t> rec;
    for (std::size_t i = 0; i < tx.size(); ++i) {
        const C32 prev = kSym[tx[i == 0 ? 0 : i - 1]];
        const C32 s{(kSym[tx[i]].re + 0.6f * prev.re) / 1.1661904f, (kSym[tx[i]].im + 0.6f * prev.im) / 1.1661904f};
        const bool reliable = !(i >= 2000 && i < 2240);
        const auto r = det.process(reliable ? s : C32{}, reliable);
        if (r.dibit) rec.push_back(*r.dibit);
        for (float c : det.costs) ASSERT_TRUE(std::isfinite(c) && c >= 0.f);
    }
    ASSERT_EQ(rec.size(), tx.size() - dsp::detail::Sequence::kDepth + 1);
    for (std::size_t i = 500; i < rec.size(); ++i) {
        if (i >= 1900 && i < 2500) continue;
        ASSERT_EQ(rec[i], kGray[(tx[i] + 4 - tx[i - 1]) % 4]) << i;
    }
}

TEST(JrTrainSequence, LongSilenceKeepsChannelAndRecovers) {
    dsp::detail::Sequence det;
    for (int i = 0; i < 10000; ++i) det.process(C32{}, false);
    for (const auto& h : det.channels) EXPECT_EQ(h[0].re, 1.f);
    const auto tx = qpsk_symbols(1000);
    std::vector<uint8_t> out;
    for (auto q : tx)
        if (auto d = det.process(kSym[q], true).dibit) out.push_back(*d);
    for (std::size_t i = 100; i < out.size(); ++i) {
        const std::size_t at = i + 1 - dsp::detail::Sequence::kDepth;
        EXPECT_EQ(out[i], kGray[(tx[at] + 4 - tx[at - 1]) % 4]);
    }
}

// ---- signal.rs の単体テスト ----

dsp::detail::SignalView view_of(double jitter, double scale) {
    dsp::detail::SignalView v;
    double phase = 0;
    const double inc[4] = {0.0, 1.0, -1.0, -2.0};
    for (std::size_t i = 0; i < dsp::detail::SignalView::kCapacity * 3; ++i) {
        phase += inc[i % 4] * std::numbers::pi / 2;
        const double a = phase + (i % 2 == 0 ? jitter : -jitter);
        v.push(C32{static_cast<float>(scale * std::cos(a)), static_cast<float>(scale * std::sin(a))});
    }
    return v;
}

TEST(JrTrainSignal, IdealSymbolsHaveFourCornersAndGainInvariantEvm) {
    for (double scale : {0.2, 1.0, 3.0}) {
        std::vector<C32> pts(dsp::detail::SignalView::kCapacity);
        double evm;
        ASSERT_EQ(view_of(0, scale).read(pts.data(), pts.size(), evm), pts.size());
        EXPECT_LT(evm, 0.001);
        for (const auto& p : pts) {
            EXPECT_NEAR(std::abs(p.re), kL, 1e-5);
            EXPECT_NEAR(std::abs(p.im), kL, 1e-5);
        }
    }
}

TEST(JrTrainSignal, PhaseScatterRaisesEvmAndSilenceClears) {
    std::vector<C32> pts(dsp::detail::SignalView::kCapacity);
    double evm;
    auto v = view_of(0.15, 1.0);
    v.read(pts.data(), pts.size(), evm);
    EXPECT_NEAR(evm, 200.0 * std::sin(0.15), 0.001);
    for (std::size_t i = 0; i < dsp::detail::SignalView::kCapacity + 1; ++i) v.push(std::nullopt);
    EXPECT_EQ(v.read(pts.data(), pts.size(), evm), 0u);
    EXPECT_EQ(evm, 0.0);
}

TEST(JrTrainSignal, MissingObservationsBreakDifferentialPairs) {
    dsp::detail::SignalView v;
    C32 p[1];
    double evm;
    v.push(C32{1.f, 0.f});
    v.push(std::nullopt);
    v.push(C32{0.f, 1.f});
    EXPECT_EQ(v.read(p, 1, evm), 0u);
    v.push(C32{-1.f, 0.f});
    ASSERT_EQ(v.read(p, 1, evm), 1u);
    EXPECT_LT(evm, 0.001);
    EXPECT_TRUE(p[0].re < 0.f && p[0].im > 0.f);
}

// ---- tests/protocol.rs ----

TEST(JrTrainFrameSync, NeedsThreeEnvelopesAndDropsAfterThreeFailures) {
    uint8_t envelope[48] = {};
    for (std::size_t i = 0; i < 3; ++i) {
        envelope[16 * i] = static_cast<uint8_t>(protocol::kMarkers[i] >> 8);
        envelope[16 * i + 1] = static_cast<uint8_t>(protocol::kMarkers[i] & 0xff);
    }
    const uint8_t zero[48] = {};
    protocol::Synchronizer s;
    EXPECT_TRUE(s.packed(envelope, 48).empty());
    EXPECT_TRUE(s.packed(envelope, 48).empty());
    EXPECT_EQ(s.packed(envelope, 48).size(), 3u);
    EXPECT_TRUE(s.locked);
    for (int i = 0; i < 3; ++i) EXPECT_EQ(s.packed(zero, 48).size(), 1u);
    EXPECT_FALSE(s.locked);
}

} // namespace
