#include "ambe/bits.hpp"
#include "secret/silence.hpp"

#include <gtest/gtest.h>

#include <array>
#include <random>
#include <string_view>
#include <vector>

using namespace spear::std_t98;
using namespace spear::std_t98::secret;

namespace {
// AMBE-3000R measurements, rate 34 (ThumbDV), frames 2..7 after reset.
// Kept as captured bytes, independent of the solver's field tables.
constexpr std::array<std::string_view, 6> kMeasured = {
    "deba0301a6e600", "9f9ac301a64500", "de9cc909b6e600",
    "df9ac201a64c00", "9f9ac301a6e700", "deba0109b6e600"};
Frame49 frame(std::string_view hex) {
    std::array<uint8_t, 7> bytes{};
    auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<uint8_t>((digit(hex[2*i]) << 4) | digit(hex[2*i+1]));
    return unpack_frame49(bytes);
}
Frame49 encrypt(const Frame49& plain, const Pn196& pn, int phase) {
    const auto ks = keystream_thumbdv(pn, phase);
    Frame49 out{};
    for (int i = 0; i < kBitsPerFrame; ++i) out[i] = plain[i] ^ ks[i];
    return out;
}
Burst two_silences(uint16_t key, int first, int second) {
    Burst b{};
    const auto pn = pn_sequence(key);
    b[first] = encrypt(frame(kMeasured[0]), pn, first);
    b[second] = encrypt(frame(kMeasured[1]), pn, second);
    return b;
}
} // namespace

TEST(StdT98Silence, RecoversEveryKeyAtEveryPhaseFromMeasuredFrames) {
    for (uint32_t key = 1; key <= kMaxKey; ++key) {
        const auto pn = pn_sequence(static_cast<uint16_t>(key));
        for (int phase = 0; phase < kFramesPerBurst; ++phase) {
            const auto raw = encrypt(frame(kMeasured[(key + phase) % kMeasured.size()]), pn, phase);
            ASSERT_EQ(silence_key_candidate(raw, phase), key) << "phase " << phase;
        }
    }
}

TEST(StdT98Silence, RequiresDifferentPhasesAndAcceptsAllSixPairs) {
    for (uint16_t key : {uint16_t{1}, uint16_t{12345}, kMaxKey}) {
        for (int first = 0; first < 4; ++first) {
            Burst b{};
            b[first] = encrypt(frame(kMeasured[2]), pn_sequence(key), first);
            EXPECT_FALSE(recover_silence_key(std::array<Burst, 1>{b}));
            EXPECT_FALSE(recover_silence_key(std::vector<Burst>(10, b)));
            for (int second = first + 1; second < 4; ++second) {
                const auto pair = two_silences(key, first, second);
                EXPECT_EQ(recover_silence_key(std::array<Burst, 1>{pair}), key);
                // The PN resets each TCH: evidence can also span two bursts.
                Burst a{}, c{};
                a[first] = pair[first]; c[second] = pair[second];
                EXPECT_EQ(recover_silence_key(std::array{a, c}), key);
            }
        }
    }
}

TEST(StdT98Silence, RejectsConflictingKeysIncludingPlaintext) {
    const auto a = two_silences(12345, 0, 1);
    const auto b = two_silences(777, 2, 3);
    const auto clear = two_silences(0, 0, 1);
    EXPECT_EQ(recover_silence_key(std::array<Burst, 1>{clear}), 0);
    EXPECT_FALSE(recover_silence_key(std::array{a, b}));
    EXPECT_FALSE(recover_silence_key(std::array{clear, b}));
    EXPECT_FALSE(recover_silence_key(std::array{b, clear}));
}

TEST(StdT98Silence, ChecksUnusedBitsAndDoesNotGuessThroughCorruption) {
    const auto pn = pn_sequence(12345);
    auto d = ambe::unpack_thumbdv(pack_frame49(frame(kMeasured[0])));
    d[13] ^= 1; // outside the 22 solved bits; b3 is now outside the measured set
    const auto invalid = unpack_frame49(ambe::pack_thumbdv(d));
    for (int phase = 0; phase < 4; ++phase)
        EXPECT_FALSE(silence_key_candidate(encrypt(invalid, pn, phase), phase));

    const auto pair = two_silences(12345, 0, 1);
    for (int bit = 0; bit < 49; ++bit) {
        auto damaged = pair;
        damaged[0][bit] ^= 1;
        const auto result = recover_silence_key(std::array<Burst, 1>{damaged});
        // A flipped variable bit may still be a valid silence: abstain or
        // return the correct key, never a different key from one intact vote.
        EXPECT_TRUE(!result || *result == 12345) << "bit " << bit;
        const auto more = two_silences(12345, 2, 3);
        damaged[2] = more[2]; damaged[3] = more[3];
        EXPECT_EQ(recover_silence_key(std::array<Burst, 1>{damaged}), 12345);
    }
}

TEST(StdT98Silence, UnsupportedModesAndInvalidInputsAbstain) {
    EXPECT_FALSE(recover_silence_key({}));
    EXPECT_FALSE(silence_key_candidate(frame(kMeasured[0]), -1));
    EXPECT_FALSE(silence_key_candidate(frame(kMeasured[0]), 4));
    auto invalid = frame(kMeasured[0]); invalid[0] = 2;
    EXPECT_FALSE(silence_key_candidate(invalid, 0));
    // 背景雑音は 1 バーストだけでは証拠不足。起動直後の非対応フレームも棄却。
    for (auto hex : {"f74ad80926b400", "f74ac1432ea100", "cd4ac301b6e600", "8d4ac009a64d00"}) {
        Burst b{};
        for (int phase = 0; phase < 4; ++phase)
            b[phase] = encrypt(frame(hex), pn_sequence(12345), phase);
        EXPECT_FALSE(recover_silence_key(std::array<Burst, 1>{b}));
    }
    const std::atomic<bool> cancel{true};
    EXPECT_FALSE(recover_silence_key(std::array<Burst, 1>{two_silences(12345, 0, 1)}, &cancel));
}

TEST(StdT98Silence, RandomNonSilenceDoesNotProduceConfirmedKeys) {
    std::mt19937 rng(0x98);
    for (int trial = 0; trial < 10000; ++trial) {
        std::array<Burst, 10> window{};
        for (auto& b : window) for (auto& f : b) for (auto& bit : f)
            bit = static_cast<uint8_t>(rng() & 1u);
        EXPECT_FALSE(recover_silence_key(window)) << "trial " << trial;
    }
}

TEST(StdT98Silence, BackgroundRecoversAllKeysWithoutGainOrSpectrumAssumptions) {
    for (uint32_t key = 0; key <= kMaxKey; ++key) {
        std::array<Burst, 2> window{};
        for (int bi = 0; bi < 2; ++bi) for (int phase = 0; phase < 4; ++phase)
            window[bi][phase] = encrypt(frame((bi+phase)%2 ? "f74ad80926b400" : "f74ac1432ea100"), pn_sequence(static_cast<uint16_t>(key)), phase);
        ASSERT_EQ(recover_silence_key(window), key);
    }
}

TEST(StdT98Silence, BackgroundRequiresCorroborationAndRejectsAmbiguity) {
    auto background = [](uint16_t key) {
        Burst b{};
        for (int phase = 0; phase < 4; ++phase)
            b[phase] = encrypt(frame("f74ac1432ea100"), pn_sequence(key), phase);
        return b;
    };
    const auto a = background(12345), b = background(777), clear = background(0);
    EXPECT_FALSE(recover_silence_key(std::array<Burst,1>{a}));
    EXPECT_EQ(recover_silence_key(std::array{a,a}), 12345);
    EXPECT_FALSE(recover_silence_key(std::array{a,a,b,b}));
    EXPECT_FALSE(recover_silence_key(std::array{a,a,clear,clear}));
    EXPECT_FALSE(recover_silence_key(std::array{a,a,two_silences(777,0,1)}));
    auto partial = a;
    partial[2] = {}; partial[3] = {};
    EXPECT_FALSE(recover_silence_key(std::array{partial,partial,partial}));
    auto damaged = a;
    damaged[0][0] ^= 1;
    EXPECT_EQ(recover_silence_key(std::array{damaged,a}), 12345);
}
