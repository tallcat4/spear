// 音声合成(jr-voice の C++ 移植)のテスト。
//   1. 参照ベクトル: tests/refgen/src/bin/voice.rs(原本の Rust)が出した PCM と、同じ操作列での C++ の出力がビット一致すること
//   2. 原本のテストの移植: src/tests.rs、tests/planner.rs、tests/synthesis_stream.rs(ストリームの契約)
#include "voice/numeric.hpp"
#include "voice/vocoder.hpp"
#include "voice/voice.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef JR_TRAIN_VOICE_VECTORS_DIR
#error "JR_TRAIN_VOICE_VECTORS_DIR must be defined"
#endif

namespace {

using namespace spear::jr_train::voice;

struct Event {
    uint32_t index;
    std::optional<Payload> payload;
};
struct Frame {
    uint32_t step;
    uint32_t index;
    Pcm pcm;
    bool operator==(const Frame&) const = default;
};

std::string path(const std::string& n) { return std::string(JR_TRAIN_VOICE_VECTORS_DIR) + "/" + n; }

// refgen の platform と同じ書式(Rust の std::env::consts::{OS, ARCH})
std::string this_platform() {
#if defined(__APPLE__)
    std::string os = "macos";
#elif defined(__linux__)
    std::string os = "linux";
#else
    std::string os = "other";
#endif
#if defined(__aarch64__)
    return os + "-aarch64";
#elif defined(__x86_64__)
    return os + "-x86_64";
#else
    return os + "-other";
#endif
}
// 浮動小数の結果は libm に依存するので、ビット一致は参照ベクトルを作った OS・アーキテクチャでだけ要求する。
// 別の環境では出力の位置(操作番号・index)だけを確かめ、PCM の差は表示する(refgen を走らせ直せばその環境でも厳密に比べられる)
bool same_platform_as_vectors() {
    std::ifstream f(path("platform"));
    std::string p;
    f >> p;
    return p == this_platform();
}

std::vector<Event> load_events(const std::string& name) {
    std::ifstream f(path(name + ".events"));
    std::vector<Event> events;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream is(line);
        std::string op;
        is >> op;
        if (op != "push") continue;
        Event e{};
        std::string hex;
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

std::vector<Frame> load_frames(const std::string& name) {
    std::ifstream f(path(name + ".out"), std::ios::binary);
    const std::vector<char> raw{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    constexpr std::size_t kRecord = 8 + 640;
    std::vector<Frame> out(raw.size() / kRecord);
    for (std::size_t r = 0; r < out.size(); ++r) {
        const char* p = raw.data() + r * kRecord;
        std::memcpy(&out[r].step, p, 4);
        std::memcpy(&out[r].index, p + 4, 4);
        std::memcpy(out[r].pcm.data(), p + 8, 640);
    }
    return out;
}

std::vector<Frame> run(const std::vector<Event>& events) {
    Voice voice;
    std::vector<Frame> out;
    auto drain = [&](uint32_t step) {
        while (auto o = voice.read()) out.push_back({step, o->index, o->pcm});
    };
    for (std::size_t s = 0; s < events.size(); ++s) {
        voice.push(events[s].index, events[s].payload);
        drain(static_cast<uint32_t>(s));
    }
    voice.flush();
    drain(static_cast<uint32_t>(events.size()));
    return out;
}

class Reference : public ::testing::TestWithParam<const char*> {};

TEST_P(Reference, MatchesRustBitExactly) {
    const std::string name = GetParam();
    const auto events = load_events(name);
    const auto expected = load_frames(name);
    ASSERT_FALSE(events.empty()) << "missing vectors for " << name;
    const auto t0 = std::chrono::steady_clock::now();
    const auto actual = run(events);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    ASSERT_EQ(actual.size(), expected.size());
    std::size_t mismatched_frames = 0;
    int max_diff = 0;
    std::size_t first = actual.size();
    for (std::size_t i = 0; i < actual.size(); ++i) {
        EXPECT_EQ(actual[i].step, expected[i].step) << "frame " << i;
        EXPECT_EQ(actual[i].index, expected[i].index) << "frame " << i;
        if (actual[i].pcm != expected[i].pcm) {
            ++mismatched_frames;
            if (first == actual.size()) first = i;
            for (std::size_t n = 0; n < 320; ++n) max_diff = std::max(max_diff, std::abs(actual[i].pcm[n] - expected[i].pcm[n]));
        }
    }
    std::printf("  %-10s %4zu frames  %.2f ms/frame  mismatched %zu (first %zu, max |diff| %d)\n", name.c_str(), actual.size(),
                actual.empty() ? 0.0 : ms / static_cast<double>(actual.size()), mismatched_frames, first, max_diff);
    if (same_platform_as_vectors()) {
        EXPECT_EQ(mismatched_frames, 0u) << "first mismatched frame " << first << ", max |diff| " << max_diff;
    } else {
        std::printf("  (vectors were generated on another platform: PCM equality is informational only; regenerate with tests/refgen to check bit-exactly)\n");
    }
}

INSTANTIATE_TEST_SUITE_P(Vectors, Reference, ::testing::Values("continuous", "drifting", "gaps", "missing", "runs", "max_index", "repeated", "zeros"));

// ---- 原本 tests/planner.rs / tests/synthesis_stream.rs ----

Payload seed_payload(uint8_t seed) {
    Payload b{};
    for (std::size_t i = 0; i < 20; ++i) b[i] = static_cast<uint8_t>(static_cast<uint8_t>(seed * 29 + static_cast<uint8_t>(i * 17)) ^ 0x5a);
    b[19] &= 0x3f;
    return b;
}
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

std::vector<uint32_t> decode_indices(const std::vector<Event>& events) {
    Voice voice;
    std::vector<uint32_t> indices;
    auto drain = [&] {
        while (auto o = voice.read()) indices.push_back(o->index);
    };
    for (const auto& e : events) {
        voice.push(e.index, e.payload);
        drain();
    }
    voice.flush();
    drain();
    voice.flush();
    EXPECT_FALSE(voice.read()) << "a second flush must not replay the tail";
    return indices;
}

TEST(JrVoiceStream, FirstFrameWaitsForSevenFutureFramesAndFlushKeepsTail) {
    Voice voice;
    for (uint32_t i = 0; i < 8; ++i) {
        voice.push(i, seed_payload(static_cast<uint8_t>(i)));
        if (i < 7) {
            EXPECT_FALSE(voice.read()) << "280 ms synthesis lookahead";
        }
    }
    auto first = voice.read();
    ASSERT_TRUE(first);
    EXPECT_EQ(first->index, 0u);
    EXPECT_FALSE(voice.read());
    voice.flush();
    std::vector<uint32_t> indices;
    while (auto o = voice.read()) indices.push_back(o->index);
    EXPECT_EQ(indices, (std::vector<uint32_t>{1, 2, 3, 4, 5, 6, 7}));
}

TEST(JrVoiceStream, ShortCallsEmitEveryAcceptedFrameAtEof) {
    for (uint32_t length = 1; length <= 7; ++length) {
        std::vector<Event> events;
        std::vector<uint32_t> expected;
        for (uint32_t i = 0; i < length; ++i) {
            events.push_back({42 + i, seed_payload(static_cast<uint8_t>(i))});
            expected.push_back(42 + i);
        }
        EXPECT_EQ(decode_indices(events), expected);
    }
}

TEST(JrVoiceStream, MissingSlotsAndGapsPreserveOnlyAcceptedIndices) {
    const std::vector<Event> events = {{0, seed_payload(0)}, {1, seed_payload(1)}, {2, std::nullopt}, {5, std::nullopt},  {6, seed_payload(2)},
                                       {7, seed_payload(3)}, {8, std::nullopt},    {9, std::nullopt}, {10, seed_payload(4)}, {11, std::nullopt}};
    EXPECT_EQ(decode_indices(events), (std::vector<uint32_t>{0, 1, 6, 7, 10}));
}

TEST(JrVoiceStream, FrequentTwoFrameRunsDrainWithoutLosingFrames) {
    std::vector<Event> events;
    std::vector<uint32_t> indices;
    for (uint32_t i = 0; i < 8; ++i)
        for (uint32_t k : {5 * i, 5 * i + 1}) {
            indices.push_back(k);
            events.push_back({k, seed_payload(static_cast<uint8_t>(k))});
        }
    EXPECT_EQ(decode_indices(events), indices);
}

TEST(JrVoiceStream, MaximumIndexSurvivesGapAndFlush) {
    constexpr uint32_t M = std::numeric_limits<uint32_t>::max();
    const std::vector<Event> events = {{M - 4, seed_payload(0)}, {M - 2, std::nullopt}, {M - 1, seed_payload(1)}, {M, seed_payload(2)}};
    EXPECT_EQ(decode_indices(events), (std::vector<uint32_t>{M - 4, M - 1, M}));
}

TEST(JrVoiceStream, MissingOnlyStreamHasNoOutput) {
    std::vector<Event> events;
    for (uint32_t i = 0; i < 8; ++i) events.push_back({i, std::nullopt});
    EXPECT_TRUE(decode_indices(events).empty());
}

std::vector<std::pair<uint32_t, Pcm>> decode_pcm(const std::vector<Event>& events) {
    Voice voice;
    std::vector<std::pair<uint32_t, Pcm>> out;
    for (const auto& e : events) {
        voice.push(e.index, e.payload);
        const std::size_t before = out.size();
        while (auto o = voice.read()) out.emplace_back(o->index, o->pcm);
        EXPECT_LE(out.size() - before, 1u) << "one output per receive operation";
    }
    voice.flush();
    while (auto o = voice.read()) out.emplace_back(o->index, o->pcm);
    voice.flush();
    EXPECT_FALSE(voice.read());
    return out;
}

TEST(JrVoiceStream, OwnedVoiceFitsAnOrdinaryWorkerStack) {
    std::size_t count = 0;
    std::thread t([&] {
        std::vector<Event> events;
        for (uint32_t i = 0; i < 9; ++i) events.push_back({i, lcg_payload(i)});
        count = decode_pcm(events).size();
    });
    t.join();
    EXPECT_EQ(count, 9u);
}

TEST(JrVoiceStream, RepeatedGapsDoNotExhaustTheInputQueue) {
    for (bool missing_events : {false, true}) {
        std::vector<Event> events;
        std::vector<uint32_t> expected;
        uint32_t index = 0;
        for (uint32_t run = 0; run < 24; ++run) {
            for (uint32_t k = 0; k < 1 + (run * 7) % 19; ++k) {
                events.push_back({index, lcg_payload(index)});
                expected.push_back(index);
                ++index;
            }
            if (missing_events) events.push_back({index, std::nullopt});
            ++index;
        }
        const auto actual = decode_pcm(events);
        std::vector<uint32_t> got;
        for (const auto& r : actual) got.push_back(r.first);
        EXPECT_EQ(got, expected);
        EXPECT_EQ(actual, decode_pcm(events)) << "replay resets all synthesis state";
    }
}

TEST(JrVoiceStream, SeventhFutureFrameIsTheLatestPossibleDependency) {
    std::vector<Event> events;
    for (uint32_t i = 0; i < 32; ++i) events.push_back({i, lcg_payload(i)});
    const auto baseline = decode_pcm(events);
    auto changed = events;
    changed[24].payload = lcg_payload(1337);
    const auto actual = decode_pcm(changed);
    ASSERT_EQ(baseline.size(), actual.size());
    for (std::size_t i = 0; i < 17; ++i) EXPECT_EQ(baseline[i], actual[i]) << i;
    bool any = false;
    for (std::size_t i = 17; i < baseline.size(); ++i) any |= baseline[i] != actual[i];
    EXPECT_TRUE(any);
}

TEST(JrVoiceStream, StatesFromAnEarlierCallDoNotLeakPastAGap) {
    std::vector<Event> first, second;
    for (uint32_t i = 0; i < 16; ++i) first.push_back({i, lcg_payload(i)});
    for (uint32_t i = 25; i < 45; ++i) second.push_back({i, lcg_payload(i)});
    auto a = first;
    a.insert(a.end(), second.begin(), second.end());
    auto b = first;
    for (auto& e : b) e.payload = lcg_payload(e.index + 1000);
    b.insert(b.end(), second.begin(), second.end());
    const auto ra = decode_pcm(a), rb = decode_pcm(b);
    ASSERT_EQ(ra.size(), rb.size());
    for (std::size_t i = 16; i < ra.size(); ++i) EXPECT_EQ(ra[i], rb[i]) << i;
}

// ---- 原本 src/tests.rs(C ABI の受理規則を Voice の API で)----

TEST(JrVoiceApi, ValidatesPayloadAndRequiresDrain) {
    Voice voice;
    Payload bad{};
    bad[19] = 0x80;
    EXPECT_THROW(voice.push(0, bad), VoiceError);
    Payload p{};
    for (uint32_t i = 0; i < 8; ++i) voice.push(i, p);
    EXPECT_THROW(voice.push(8, std::nullopt), VoiceError) << "undrained output";
    auto o = voice.read();
    ASSERT_TRUE(o);
    EXPECT_EQ(o->index, 0u);
}

TEST(JrVoiceApi, IndicesMustIncreaseAndFinishedStreamRejectsInput) {
    Voice voice;
    Payload p{};
    voice.push(5, p);
    EXPECT_THROW(voice.push(5, p), VoiceError);
    voice.flush();
    while (voice.read()) {
    }
    EXPECT_EQ(voice.status().finished, 1u);
    EXPECT_THROW(voice.push(9, p), VoiceError);
    voice.flush();   // 2 回目は何もしない
}

TEST(JrVoiceNumeric, RoundEvenAndEnvelopeImpulse) {
    const std::pair<float, float> cases[] = {{-2.5f, -2.f}, {-1.5f, -2.f}, {-0.5f, 0.f}, {0.5f, 0.f}, {1.5f, 2.f}, {2.5f, 2.f}};
    for (auto [in, expected] : cases) EXPECT_EQ(round_even(in), expected) << in;
    Numeric numeric;
    for (float v : {0.f, 0.01f, 30.f, -30.f}) {
        std::array<float, 32> shape;
        shape.fill(v);
        std::array<float, 256> impulse{};
        ASSERT_TRUE(numeric.envelope_impulse(shape.data(), impulse));
        float power = 0.f;
        for (float x : impulse) {
            ASSERT_TRUE(std::isfinite(x));
            power += x * x;
        }
        EXPECT_NEAR(power, 1.f, 1e-5f);
    }
    Vocoder vocoder;
    Controls parameters{};
    parameters[0] = std::nanf("");
    EXPECT_ANY_THROW(vocoder.process_explicit(parameters, std::nullopt, nullptr, nullptr, true));
}

TEST(JrVoiceNumeric, NoiseIsDeterministicAndBounded) {
    VoiceNoise reference;
    for (uint64_t expected : {0xfc9abc4f5279a3ebull, 0x627ab0f5a910d99aull, 0x2d399fa263b9f449ull, 0x49db60aaa1abea5full, 0xc3a06605dac203dbull, 0xdbd8c6d7d7cfd4a6ull,
                              0xcda8f7efa53eca94ull, 0xbbae304332706acdull})
        EXPECT_EQ(reference.next_u64(), expected);
    VoiceNoise a, b;
    for (int i = 0; i < 10000; ++i) {
        const float x = a.uniform();
        const float y = b.uniform();
        EXPECT_EQ(std::memcmp(&x, &y, 4), 0);
        EXPECT_LE(std::abs(x), std::sqrt(3.f));
    }
}

} // namespace
