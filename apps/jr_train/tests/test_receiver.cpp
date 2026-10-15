// Receiver(前段 + 全チャネル + worker)のテスト。参照ベクトル(tests/vectors、240 kS/s)を使う。
//   1. 240 kS/s をそのまま入れる(前段なし): チャネル並列でも原本と同じフレーム
//   2. ×8 に補間し +250 kHz へずらした 1.92 MS/s(B210 の LO を帯域中心 −250 kHz に置いた状態)を入れる:
//      回転 + ÷8 の前段を通しても、各チャネルが原本と同等に同期する
#include "presets.hpp"
#include "receiver.hpp"
#include "spear/dsp/fir.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <numbers>
#include <sstream>
#include <string>

namespace {

using namespace spear;
using namespace spear::jr_train;

struct Vectors {
    double rate = 0, center = 0;
    std::size_t samples = 0, block = 0;
    std::vector<double> freqs;
    std::vector<bool> present;
    std::vector<cf32> iq;
    // ch → (bit_offset → hex)
    std::vector<std::map<uint64_t, std::string>> frames;
    std::vector<uint64_t> synced;
};

std::string path(const std::string& n) { return std::string(JR_TRAIN_VECTORS_DIR) + "/" + n; }

Vectors load() {
    Vectors v;
    std::ifstream meta(path("meta.txt"));
    std::string line;
    while (std::getline(meta, line)) {
        std::istringstream is(line);
        std::string k;
        is >> k;
        if (k == "rate") is >> v.rate;
        else if (k == "center") is >> v.center;
        else if (k == "samples") is >> v.samples;
        else if (k == "block") is >> v.block;
        else if (k == "channel") {
            int c, p;
            double f;
            is >> c >> f >> p;
            v.freqs.push_back(f);
            v.present.push_back(p != 0);
        }
    }
    std::ifstream f(path("band_240k.sc16"), std::ios::binary);
    std::vector<char> raw{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    v.iq.resize(raw.size() / 4);
    for (std::size_t i = 0; i < v.iq.size(); ++i) {
        int16_t re, im;
        std::memcpy(&re, &raw[4 * i], 2);
        std::memcpy(&im, &raw[4 * i + 2], 2);
        v.iq[i] = cf32(static_cast<float>(re) / 32768.f, static_cast<float>(im) / 32768.f);
    }
    for (std::size_t c = 0; c < v.freqs.size(); ++c) {
        std::ifstream r(path("ch" + std::to_string(c) + "_result.txt"));
        std::map<uint64_t, std::string> fr;
        uint64_t synced = 0;
        while (std::getline(r, line)) {
            std::istringstream is(line);
            std::string k;
            is >> k;
            if (k == "frame") {
                uint64_t off;
                int s;
                uint32_t e;
                std::string hex;
                is >> off >> s >> e >> hex;
                fr[off] = hex;
            } else if (k == "synchronized_frames") {
                is >> synced;
            }
        }
        v.frames.push_back(fr);
        v.synced.push_back(synced);
    }
    return v;
}

std::string hex(const std::array<uint8_t, 48>& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t x : b) {
        s += d[x >> 4];
        s += d[x & 15];
    }
    return s;
}

TEST(JrTrainReceiver, ParallelChannelsMatchReferenceAt240k) {
    const auto v = load();
    ASSERT_FALSE(v.iq.empty());
    ReceiverConfig cfg;
    cfg.in_rate = v.rate;
    for (double f : v.freqs) cfg.channel_offsets_hz.push_back(f - v.center);
    cfg.threads = 3;
    Receiver rx(cfg);
    EXPECT_EQ(rx.decim(), 1u);
    EXPECT_EQ(rx.threads(), 3);
    std::vector<std::map<uint64_t, std::string>> got(v.freqs.size());
    Observer o;
    o.frame = [&](const Frame& f) { got[static_cast<std::size_t>(f.channel)][f.bit_offset] = hex(f.received); };
    rx.set_observer(o);
    for (std::size_t i = 0; i < v.iq.size(); i += 16384 / 8) rx.process(std::span<const cf32>(v.iq).subspan(i, std::min<std::size_t>(2048, v.iq.size() - i)), i);
    for (std::size_t c = 0; c < v.freqs.size(); ++c) {
        EXPECT_EQ(got[c], v.frames[c]) << "channel " << c;
        EXPECT_EQ(rx.status(static_cast<int>(c)).synchronized_frames, v.synced[c]);
    }
}

TEST(JrTrainReceiver, FrontEndRotationAndDecimationFrom1920k) {
    const auto v = load();
    ASSERT_FALSE(v.iq.empty());
    constexpr double kLoOffset = 250e3;   // LO = 帯域中心 − 250 kHz → 帯域は IQ の +250 kHz に見える
    constexpr std::size_t kUp = 8;
    const double rate = v.rate * kUp;
    // ×8 補間(通過域 ±80 kHz)→ +250 kHz へ移す
    spear::dsp::FirInterpolator<cf32> up("up", v.rate, spear::dsp::design_lowpass(rate, 90e3, 16 * kUp + 1), kUp);
    std::vector<cf32> wide(v.iq.size() * kUp);
    up.process(v.iq, wide);
    for (std::size_t i = 0; i < wide.size(); ++i) {
        const double ph = 2.0 * std::numbers::pi * kLoOffset * static_cast<double>(i) / rate;
        wide[i] *= cf32(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
    }
    ReceiverConfig cfg;
    cfg.in_rate = rate;
    cfg.rotate_hz = kLoOffset;
    for (double f : v.freqs) cfg.channel_offsets_hz.push_back(f - v.center);
    Receiver rx(cfg);
    EXPECT_EQ(rx.decim(), 8u);
    EXPECT_DOUBLE_EQ(rx.work_rate(), 240e3);
    std::vector<std::map<uint64_t, std::string>> got(v.freqs.size());
    Observer o;
    std::vector<uint64_t> index_of_first;
    o.frame = [&](const Frame& f) { got[static_cast<std::size_t>(f.channel)][f.bit_offset] = hex(f.received); };
    rx.set_observer(o);
    for (std::size_t i = 0; i < wide.size(); i += 16384) rx.process(std::span<const cf32>(wide).subspan(i, std::min<std::size_t>(16384, wide.size() - i)), i);
    for (std::size_t c = 0; c < v.freqs.size(); ++c) {
        const auto& st = rx.status(static_cast<int>(c));
        std::printf("  ch%zu: synced %llu (ref %llu) frames %zu (ref %zu) evm-ref-free carrier_locked %d offset %.1f Hz\n", c,
                    static_cast<unsigned long long>(st.synchronized_frames), static_cast<unsigned long long>(v.synced[c]), got[c].size(), v.frames[c].size(),
                    st.snap.carrier_locked, st.snap.frequency_offset_hz);
        if (!v.present[c]) {
            EXPECT_EQ(st.synchronized_frames, 0u);
            continue;
        }
        // 前段のフィルタと群遅延で sample 位置がずれるので、ビット位置ではなく「同期したフレーム数」と「中身の一致」を見る
        EXPECT_GE(st.synchronized_frames + 2, v.synced[c]) << "channel " << c;
        std::size_t matched = 0;
        std::map<std::string, int> ref_payloads;
        for (const auto& [off, h] : v.frames[c]) ++ref_payloads[h];
        for (const auto& [off, h] : got[c]) matched += ref_payloads.count(h);
        EXPECT_GE(matched + 3, v.frames[c].size()) << "channel " << c;
    }
}

TEST(JrTrainReceiver, PresetsCoverRustChannelPlan) {
    const auto* east = find_preset("east");
    ASSERT_NE(east, nullptr);
    ASSERT_EQ(east->channels.size(), 22u);
    EXPECT_EQ(east->channels.front().frequency_hz, 352537500.0);
    EXPECT_EQ(east->channels.back().frequency_hz, 352668750.0);
    EXPECT_EQ(find_preset("central")->channels.size(), 3u);
    EXPECT_EQ(find_preset("west")->channels.size(), 19u);
    for (const auto& p : presets()) {
        ReceiverConfig cfg;
        cfg.in_rate = 1.92e6;
        for (const auto& c : p.channels) {
            cfg.channel_offsets_hz.push_back(c.frequency_hz - p.center_hz);
            EXPECT_EQ(std::fmod(c.frequency_hz, 6250.0), 0.0) << c.frequency_hz;
        }
        EXPECT_NO_THROW(Receiver{cfg}) << p.id;
    }
}

TEST(JrTrainReceiver, DiscontinuityStartsNewEpoch) {
    const auto v = load();
    ReceiverConfig cfg;
    cfg.in_rate = v.rate;
    cfg.channel_offsets_hz = {v.freqs[0] - v.center};
    cfg.threads = 1;
    Receiver rx(cfg);
    const std::size_t half = v.iq.size() / 2;
    rx.process(std::span<const cf32>(v.iq).first(half), 0);
    EXPECT_EQ(rx.status(0).epoch, 0u);
    rx.discontinuity();
    EXPECT_EQ(rx.status(0).epoch, 1u);
    EXPECT_FALSE(rx.status(0).frame_locked);
    std::vector<Frame> frames;
    Observer o;
    o.frame = [&](const Frame& f) { frames.push_back(f); };
    rx.set_observer(o);
    rx.process(std::span<const cf32>(v.iq).subspan(half), half);
    ASSERT_FALSE(frames.empty());
    EXPECT_EQ(frames.front().epoch, 1u);
    EXPECT_GE(frames.front().input_sample_index, half);
}

} // namespace
