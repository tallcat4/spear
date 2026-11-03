// mrl-celp-c の復号(Decoder::Enhance)のテスト。参照になる原本は無いので、次を確かめる:
//   1. 取り込んだ mrl_decode(ビット一致版)が、原本とビット一致を確かめてある C++ 移植(celp::Native)と同じ PCM を出す
//   2. Voice(Enhance) の出力が mrl_decode_enhanced を時刻をそろえて並べたもの(mrl2wav --enhance と同じ)と一致する(区間の端の 10 ms は絞る)
//   3. 出す index と受理の規則が jr-voice と同じ(参照ベクトルの操作列で)
#include "voice/celp/celp.hpp"
#include "voice/voice.hpp"

#include "mrlcelp.h"

#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace spear::jr_train::voice;

struct Event {
    uint32_t index;
    std::optional<Payload> payload;
};

Payload lcg_payload(uint32_t index) {
    uint64_t state = static_cast<uint64_t>(index) + 1;
    Payload r{};
    for (auto& x : r) {
        state = state * 6364136223846793005ull + 1;
        x = static_cast<uint8_t>(state >> 32);
    }
    r[19] &= 63;
    return r;
}

std::vector<Event> load_events(const std::string& name) {
    std::ifstream f(std::string(JR_TRAIN_VOICE_VECTORS_DIR) + "/" + name + ".events");
    std::vector<Event> events;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream is(line);
        std::string op, hex;
        is >> op;
        if (op != "push") continue;
        Event e{};
        is >> e.index >> hex;
        if (hex != "-") {
            Payload p{};
            for (std::size_t i = 0; i < 20; ++i) p[i] = static_cast<uint8_t>(std::stoul(hex.substr(2 * i, 2), nullptr, 16));
            e.payload = p;
        }
        events.push_back(e);
    }
    return events;
}

// 1 回の push / flush で出たものを、その操作の番号つきで
struct Frame {
    std::size_t step;
    uint32_t index;
    Pcm pcm;
    bool operator==(const Frame&) const = default;
};
std::vector<Frame> run(Decoder d, const std::vector<Event>& events) {
    Voice voice{d};
    std::vector<Frame> out;
    for (std::size_t s = 0; s < events.size(); ++s) {
        voice.push(events[s].index, events[s].payload);
        while (auto o = voice.read()) out.push_back({s, o->index, o->pcm});
    }
    voice.flush();
    while (auto o = voice.read()) out.push_back({events.size(), o->index, o->pcm});
    return out;
}
std::vector<uint32_t> indices(const std::vector<Frame>& f) {
    std::vector<uint32_t> r;
    for (const auto& x : f) r.push_back(x.index);
    return r;
}

TEST(MrlCelp, BitExactDecoderMatchesTheVerifiedCppPort) {
    auto mem = std::make_unique<double[]>(mrl_decoder_size() / sizeof(double) + 1);
    mrl_decoder* d = mrl_decoder_init(mem.get());
    celp::Native native;
    for (uint32_t i = 0; i < 400; ++i) {
        const Payload p = lcg_payload(i);
        std::array<int16_t, 320> pcm{};
        ASSERT_EQ(mrl_decode(d, p.data(), pcm.data()), 0);
        ASSERT_EQ(pcm, native.decode(p).pcm) << "frame " << i;
    }
}

TEST(MrlCelp, EnhanceStreamIsTimeAlignedMrlDecodeEnhanced) {
    constexpr uint32_t kFrames = 40;
    std::vector<Event> events;
    for (uint32_t i = 0; i < kFrames; ++i) events.push_back({100 + i, lcg_payload(i)});
    const auto got = run(Decoder::Enhance, events);
    ASSERT_EQ(got.size(), kFrames);
    // mrl2wav --enhance と同じ並べ方: 最初の 160 sample を捨てて、最後に flush の 160 sample を足す
    auto mem = std::make_unique<double[]>(mrl_enhanced_size() / sizeof(double) + 1);
    mrl_enhanced* e = mrl_enhanced_init(mem.get(), nullptr);
    std::vector<int16_t> want;
    for (uint32_t i = 0; i < kFrames; ++i) {
        std::array<int16_t, 320> pcm{};
        ASSERT_EQ(mrl_decode_enhanced(e, events[i].payload->data(), pcm.data()), 0);
        want.insert(want.end(), pcm.begin() + (i == 0 ? 160 : 0), pcm.end());
    }
    std::array<int16_t, 160> tail{};
    mrl_enhanced_flush(e, tail.data());
    want.insert(want.end(), tail.begin(), tail.end());
    ASSERT_EQ(want.size(), kFrames * 320u);
    std::vector<int16_t> all;
    for (std::size_t k = 0; k < got.size(); ++k) {
        EXPECT_EQ(got[k].index, 100 + k);
        EXPECT_EQ(got[k].step, k + 1) << "one frame of lookahead";
        all.insert(all.end(), got[k].pcm.begin(), got[k].pcm.end());
    }
    for (std::size_t n = 80; n + 80 < all.size(); ++n) ASSERT_EQ(all[n], want[n]) << n;
    for (std::size_t n = 0; n < 80; ++n) {
        EXPECT_LE(std::abs(all[n]), std::abs(want[n])) << "fade in " << n;
        EXPECT_LE(std::abs(all[all.size() - 1 - n]), std::abs(want[want.size() - 1 - n])) << "fade out " << n;
    }
}

class SameRules : public ::testing::TestWithParam<const char*> {};
TEST_P(SameRules, EmitsTheSameIndicesAsJrVoiceAndIsDeterministic) {
    const auto events = load_events(GetParam());
    ASSERT_FALSE(events.empty());
    const auto enh = run(Decoder::Enhance, events);
    EXPECT_EQ(indices(enh), indices(run(Decoder::JrVoice, events)));
    EXPECT_EQ(enh, run(Decoder::Enhance, events)) << "a new decoder starts from the same state";
    std::size_t last_step = SIZE_MAX;
    for (const auto& f : enh) {
        EXPECT_NE(f.step, last_step) << "at most one frame per operation (Voice requires draining before the next push)";
        last_step = f.step;
    }
}
INSTANTIATE_TEST_SUITE_P(Vectors, SameRules, ::testing::Values("continuous", "drifting", "gaps", "missing", "runs", "max_index", "repeated", "zeros"));

TEST(MrlCelp, StatesFromAnEarlierCallDoNotLeakPastAGap) {
    std::vector<Event> a, b;
    for (uint32_t i = 0; i < 16; ++i) {
        a.push_back({i, lcg_payload(i)});
        b.push_back({i, lcg_payload(i + 1000)});
    }
    a.push_back({16, std::nullopt});
    b.push_back({16, std::nullopt});
    for (uint32_t i = 25; i < 45; ++i) {
        a.push_back({i, lcg_payload(i)});
        b.push_back({i, lcg_payload(i)});
    }
    const auto ra = run(Decoder::Enhance, a), rb = run(Decoder::Enhance, b);
    ASSERT_EQ(ra.size(), rb.size());
    for (std::size_t i = 16; i < ra.size(); ++i) EXPECT_EQ(ra[i].pcm, rb[i].pcm) << i;
}

TEST(MrlCelp, ApiRulesMatchJrVoice) {
    Voice voice{Decoder::Enhance};
    EXPECT_EQ(voice.decoder(), Decoder::Enhance);
    Payload bad{};
    bad[19] = 0x40;
    EXPECT_THROW(voice.push(0, bad), VoiceError);
    voice.push(1, lcg_payload(1));
    EXPECT_FALSE(voice.read()) << "first frame waits for the next one";
    voice.push(2, lcg_payload(2));
    EXPECT_THROW(voice.push(3, lcg_payload(3)), VoiceError) << "undrained output";
    ASSERT_TRUE(voice.read());
    EXPECT_THROW(voice.push(2, lcg_payload(2)), VoiceError) << "indices must increase";
    voice.flush();
    auto last = voice.read();
    ASSERT_TRUE(last);
    EXPECT_EQ(last->index, 2u);
    voice.flush();
    EXPECT_FALSE(voice.read()) << "a second flush must not replay the tail";
    EXPECT_EQ(voice.status().finished, 1u);
    EXPECT_EQ(voice.status().output, 2u);
    EXPECT_THROW(voice.push(9, lcg_payload(9)), VoiceError);
    EXPECT_EQ(Voice{Decoder::JrVoice}.decoder(), Decoder::JrVoice);
}

} // namespace
