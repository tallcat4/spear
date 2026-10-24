#include "secret/guided.hpp"
#include "secret/hybrid.hpp"
#include "secret/tracker.hpp"
#include "guided_golden.hpp"
#include <gtest/gtest.h>
#include <limits>
using namespace spear::std_t98::secret;
namespace {
std::vector<Burst> plain() {
    std::vector<Burst> out(12);
    std::size_t i = 0;
    for (auto& b : out) for (auto& f : b) for (auto& bit : f)
        bit = static_cast<uint8_t>(guided_golden::plain[i++] - '0');
    return out;
}
std::vector<Burst> encrypt(uint8_t series, uint16_t key) {
    auto out = plain();
    const auto pn = pn_sequence(key, series);
    for (auto& b : out) for (int phase = 0; phase < 4; ++phase) {
        const auto ks = keystream_thumbdv(pn, phase);
        for (std::size_t bit = 0; bit < 49; ++bit) b[static_cast<std::size_t>(phase)][bit] ^= ks[bit];
    }
    return out;
}
}
TEST(StdT98Guided, EnhancedPnMatchesUpstreamAllSeriesAndBoundaryKeys) {
    for (const auto& v : guided_golden::pn) {
        const auto pn = pn_sequence(static_cast<uint16_t>(v.key), static_cast<uint8_t>(v.series));
        for (std::size_t i = 0; i < pn.size(); ++i) ASSERT_EQ(pn[i], v.bits[i] - '0') << v.series << '/' << v.key << ':' << i;
        uint16_t state = 0;
        for (int i = 0; i < 15; ++i) state |= static_cast<uint16_t>(pn[static_cast<std::size_t>(i)] << i);
        EXPECT_EQ(key_for_state(state, static_cast<uint8_t>(v.series)), v.key);
    }
    EXPECT_THROW(pn_sequence(1, 16), std::invalid_argument);
    EXPECT_THROW(pn_sequence(32768), std::invalid_argument);
}
TEST(StdT98Guided, RecoversAllSeriesAndPlaintextWithoutKnownSilence) {
    for (uint8_t series = 0; series < 16; ++series) for (uint16_t key : {1, 12345, 32767}) {
        SeriesSession s;
        for (auto b : encrypt(series, key)) s.add(b);
        const auto c = s.candidates();
        ASSERT_EQ(c.size(), 5u);
        EXPECT_EQ(c[0].id, (SeriesKey{series, key}));
        EXPECT_GE(c[0].score, 40);
        EXPECT_GE(c[0].score - c[1].score, 20);
    }
    SeriesSession s;
    for (auto b : plain()) s.add(b);
    EXPECT_EQ(s.candidates()[0].id, (SeriesKey{0, 0}));
}
TEST(StdT98Guided, FecGapScoresMatchPythonAndIsolatedFramesDoNotBridge) {
    SeriesSession s, isolated;
    auto data = encrypt(10, 12345);
    for (std::size_t i = 0; i < data.size(); ++i) {
        s.add(data[i], {true, true, i != 3, true});
        isolated.add(data[i], {true, false, true, false});
    }
    EXPECT_EQ(s.pairs(), 45u);
    EXPECT_EQ(isolated.pairs(), 0u);
    EXPECT_TRUE(isolated.candidates().empty());
    const auto got = s.candidates();
    for (std::size_t i = 0; i < got.size(); ++i) {
        const auto& expected = guided_golden::candidates[i];
        EXPECT_EQ(got[i].id.series, expected.series);
        EXPECT_EQ(got[i].id.key, expected.key);
        EXPECT_NEAR(got[i].score, expected.score, 1e-9);
    }
}
TEST(StdT98Guided, HybridOwnsVerdictCachesScoresAndRejectsBadEvidence) {
    const std::vector<LinearCandidate> candidates{{{1, 12345}, 100}, {{2, 32767}, 50}};
    unsigned evaluated = 0;
    GuidedSession s([&](const auto&, auto ids) {
        std::vector<float> scores;
        for (auto id : ids) { ++evaluated; scores.push_back(id.series == 1 ? -10.f : 12.f); }
        return scores;
    });
    for (int i = 0; i < 9; ++i) { s.add({}); EXPECT_FALSE(s.verify(candidates).accepted); }
    s.add({});
    EXPECT_EQ(s.verify(candidates).accepted, (SeriesKey{2, 32767}));
    EXPECT_EQ(evaluated, 4u);
    s.add({}); s.verify(candidates);
    EXPECT_EQ(evaluated, 4u);
    for (const auto values : {std::array<float, 2>{-10, -5}, std::array<float, 2>{12, 11}}) {
        GuidedSession reject([&](const auto&, auto ids) {
            std::vector<float> scores;
            for (auto id : ids) scores.push_back(values[id.series - 1]);
            return scores;
        });
        for (int i = 0; i < 10; ++i) reject.add({});
        EXPECT_FALSE(reject.verify(candidates).accepted);
    }
    GuidedSession nonfinite([](const auto&, auto ids) { return std::vector<float>(ids.size(), std::numeric_limits<float>::quiet_NaN()); });
    for (int i = 0; i < 10; ++i) nonfinite.add({});
    EXPECT_THROW(nonfinite.verify(candidates), std::runtime_error);
    GuidedSession gated([](const auto&, auto) -> std::vector<float> { ADD_FAILURE(); return {}; }, 101);
    for (int i = 0; i < 10; ++i) gated.add({});
    EXPECT_FALSE(gated.verify(candidates).accepted);
}
TEST(StdT98Guided, BlocksAreConsecutiveRecentAndDisjoint) {
    GuidedSession s([](const auto&, auto ids) { return std::vector<float>(ids.size(), 12); });
    for (int i = 0; i < 31; ++i) s.add({}, {true, true, i != 3, true});
    EXPECT_EQ(s.block_starts(), (std::vector<uint64_t>{14, 19, 24}));
}
TEST(StdT98Guided, TrackerResetsPttIgnoresStaleResultsAndCanWithdraw) {
    Tracker t({.streaming = true, .min_window = 1, .max_window = 3, .recheck_interval = 1});
    Tracker::Request r;
    ASSERT_TRUE(t.on_secret_burst({}, &r));
    const auto old = r.session;
    t.on_guided_result(old, SeriesKey{15, 12345});
    EXPECT_EQ(t.series(), 15);
    EXPECT_EQ(t.key(), 12345);
    t.on_guided_result(old, std::nullopt);
    EXPECT_EQ(t.key(), 0);
    t.on_guided_result(old, SeriesKey{0, 0});
    EXPECT_TRUE(t.confirmed());
    EXPECT_EQ(t.status(), TrackerStatus::Keyed);
    t.on_clear();
    t.on_guided_result(old, SeriesKey{15, 12345});
    EXPECT_EQ(t.key(), 0);
    ASSERT_TRUE(t.on_secret_burst({}, &r));
    t.on_guided_result(old, SeriesKey{15, 12345});
    EXPECT_TRUE(t.pending());
    EXPECT_EQ(t.key(), 0);
    for (int i = 0; i < 5; ++i) t.on_secret_burst({}, nullptr, {true, false, true, true});
    t.on_guided_result(r.session, std::nullopt);
    ASSERT_TRUE(t.on_secret_burst({}, &r));
    EXPECT_EQ(r.first_burst, 4u);
    EXPECT_EQ(r.bursts.size(), 3u);
    EXPECT_FALSE(r.valid[0][1]);
}
