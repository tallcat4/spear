// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 原本: crates/receiver-core/src/voice_frame.rs と voice_coordinates.in
#include "voice_frame.hpp"

#include "coding.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace spear::jr_train::protocol {
namespace {

// 原本 voice_coordinates.in: 入力 bit t の (多項式 0x1AF の出力位置, 0x11D の出力位置)。255 はパンクチャ
constexpr std::array<std::size_t, 188> kCoordinates = {
    2, 6, 3, 7, 255, 4, 5, 0, 14, 1, 15, 10, 12, 11, 13, 8, 22, 9, 23, 18, 255, 19, 16, 20,
    17, 21, 26, 30, 27, 31, 24, 28, 25, 29, 34, 38, 255, 39, 36, 35, 37, 32, 46, 33, 47, 42, 44, 43,
    45, 40, 54, 41, 255, 50, 51, 55, 48, 52, 49, 53, 58, 62, 59, 63, 56, 60, 57, 61, 255, 70, 71, 66,
    68, 67, 69, 64, 78, 65, 79, 74, 76, 75, 77, 72, 255, 73, 82, 86, 83, 87, 80, 84, 81, 85, 90, 94,
    91, 95, 88, 92, 255, 93, 102, 89, 103, 98, 100, 99, 101, 96, 110, 97, 111, 106, 108, 107, 255, 104, 105, 109,
    114, 118, 115, 119, 112, 116, 113, 117, 122, 126, 123, 127, 255, 124, 125, 120, 134, 121, 135, 130, 132, 131, 133, 128,
    142, 129, 143, 138, 255, 139, 136, 140, 137, 141, 146, 150, 147, 151, 144, 148, 145, 149, 154, 158, 255, 159, 156, 155,
    157, 152, 166, 153, 167, 162, 164, 163, 165, 160, 174, 161, 255, 170, 171, 175, 168, 172, 169, 173,
};
constexpr std::array<std::size_t, 8> kCrcPos = {0, 1, 2, 3, 82, 83, 84, 85};

using Word = std::array<uint8_t, 42>;
using Received = std::array<uint8_t, 176>;
using Input = std::array<uint8_t, 94>;

uint8_t bit(const uint8_t* data, std::size_t i) { return (data[i / 8] >> (i % 8)) & 1; }
void set(uint8_t* data, std::size_t i, uint8_t v) { data[i / 8] |= static_cast<uint8_t>(v << (i % 8)); }
uint8_t parity(std::size_t v) { return static_cast<uint8_t>(std::popcount(v) & 1); }
int ones(uint8_t v) { return std::popcount(v); }

uint8_t crc8(const uint8_t* data) {
    uint16_t state = 255;
    for (std::size_t i = 0; i < 86; ++i) {
        state = static_cast<uint16_t>((state << 1) | (i < 78 ? data[i] : 0));
        if (state & 256) state ^= 0x19b;
    }
    return static_cast<uint8_t>(state);
}

Received convolution(const Input& input) {
    Received result{};
    std::size_t state = 0;
    for (std::size_t i = 0; i < 94; ++i) {
        state = ((state << 1) | input[i]) & 511;
        if (kCoordinates[2 * i] != 255) result[kCoordinates[2 * i]] = parity(state & 0x1af);
        result[kCoordinates[2 * i + 1]] = parity(state & 0x11d);
    }
    return result;
}

Received protected_bits(const Word& word) {
    Received r{};
    for (std::size_t i = 0; i < 88; ++i) {
        r[2 * i] = bit(word.data() + 5, i);
        r[2 * i + 1] = bit(word.data() + 21, i);
    }
    return r;
}

void write_protected(Word& word, const Received& data) {
    std::fill(word.begin() + 5, word.begin() + 16, uint8_t{0});
    std::fill(word.begin() + 21, word.begin() + 32, uint8_t{0});
    for (std::size_t i = 0; i < 88; ++i) {
        set(word.data() + 5, i, data[2 * i]);
        set(word.data() + 21, i, data[2 * i + 1]);
    }
}

// パンクチャされない側(0x11D)だけからの直接の逆変換
Input inverse(const Received& received) {
    Input r{};
    std::size_t state = 0;
    for (std::size_t t = 0; t < 94; ++t) {
        r[t] = received[kCoordinates[2 * t + 1]] ^ parity(state & (0x11d >> 1));
        state = ((state << 1) | r[t]) & 255;
    }
    return r;
}

// 256 状態の硬判定 Viterbi(コストは 255 で飽和)
Input viterbi(const Received& received) {
    std::array<uint16_t, 256> costs;
    costs.fill(255);
    costs[0] = 0;
    std::array<uint16_t, 256> next{};
    std::array<std::array<uint8_t, 32>, 94> survivors{};
    for (std::size_t t = 0; t < 94; ++t) {
        const bool punctured = kCoordinates[2 * t] == 255;
        const uint16_t observed = punctured ? 1 : 2;
        const uint8_t a = received[kCoordinates[2 * t + 1]];
        const uint8_t b = punctured ? 0 : received[kCoordinates[2 * t]];
        for (std::size_t prior = 0; prior < 128; ++prior) {
            const std::size_t symbol = prior * 2;
            const uint16_t distance = static_cast<uint16_t>((parity(symbol & 0x11d) ^ a) + (punctured ? 0 : (parity(symbol & 0x1af) ^ b)));
            const uint16_t complement = static_cast<uint16_t>(observed - distance);
            const uint16_t left = costs[prior], right = costs[prior + 128];
            next[2 * prior] = std::min<uint16_t>(255, std::min<uint16_t>(static_cast<uint16_t>(left + distance), static_cast<uint16_t>(right + complement)));
            next[2 * prior + 1] = std::min<uint16_t>(255, std::min<uint16_t>(static_cast<uint16_t>(left + complement), static_cast<uint16_t>(right + distance)));
            set(survivors[t].data(), 2 * prior, right + complement < left + distance ? 1 : 0);
            set(survivors[t].data(), 2 * prior + 1, right + distance < left + complement ? 1 : 0);
        }
        std::swap(costs, next);
    }
    std::size_t state = 0;
    Input r{};
    for (std::size_t t = 94; t-- > 0;) {
        r[t] = static_cast<uint8_t>(state & 1);
        state = (state >> 1) | (static_cast<std::size_t>(bit(survivors[t].data(), state)) << 7);
    }
    return r;
}

// 外側: CRC-16(byte 28–33)と短縮 RS のパリティ(byte 36–41)を作り直す
void tail(Word& word) {
    const uint16_t crc = crc16(std::span<const uint8_t>(word.data() + 28, 6));
    word[34] = static_cast<uint8_t>(crc >> 8);
    word[35] = static_cast<uint8_t>(crc & 0xff);
    std::array<uint8_t, 14> work{};
    std::copy(word.begin() + 28, word.begin() + 36, work.begin());
    const auto& gf = Galois::get();
    constexpr uint8_t kGen[7] = {1, 63, 1, 218, 32, 227, 38};
    for (std::size_t i = 0; i < 8; ++i) {
        const uint8_t c = work[i];
        for (std::size_t j = 0; j < 7; ++j) work[i + j] ^= gf.mul(c, kGen[j]);
    }
    std::copy(work.begin() + 8, work.end(), word.begin() + 36);
}

std::optional<Speech> checked(const Word& word, const Received& received, const Input& input, uint16_t max) {
    for (std::size_t i = 86; i < 94; ++i)
        if (input[i] != 0) return std::nullopt;
    uint8_t checksum = 0;
    for (std::size_t i = 0; i < kCrcPos.size(); ++i) checksum = static_cast<uint8_t>(checksum | (input[kCrcPos[i]] << i));
    if (checksum != crc8(input.data() + 4)) return std::nullopt;
    const auto encoded = convolution(input);
    uint16_t distance = 0;
    for (std::size_t i = 0; i < 176; ++i) distance = static_cast<uint16_t>(distance + (encoded[i] ^ received[i]));
    if (distance > max) return std::nullopt;
    Word corrected = word;
    write_protected(corrected, encoded);
    tail(corrected);
    if (!std::equal(corrected.begin() + 34, corrected.end(), word.begin() + 34)) return std::nullopt;
    Speech s;
    for (std::size_t i = 0; i < 78; ++i) set(s.payload.data(), i, input[4 + i]);
    for (std::size_t i = 0; i < 40; ++i) {
        set(s.payload.data(), 78 + i, bit(word.data(), i));
        set(s.payload.data(), 118 + i, bit(word.data() + 16, i));
    }
    s.corrected_bits = distance;
    return s;
}

bool mode(const Word& word) { return word[32] == 0x80 && word[33] <= 1; }

std::optional<Speech> joint(const Word& word, const Received& received, const Input& source) {
    Word c = word;
    write_protected(c, convolution(source));
    c[32] = 0x80;
    for (uint8_t alternate = 0; alternate < 2; ++alternate) {
        c[33] = static_cast<uint8_t>((word[33] & 1) ^ alternate);
        if (ones(static_cast<uint8_t>(word[32] ^ c[32])) + ones(static_cast<uint8_t>(word[33] ^ c[33])) > 2) continue;
        tail(c);
        uint32_t outer_bits = 0, outer_symbols = 0, extra = 0;
        for (std::size_t i = 28; i < 42; ++i) {
            const auto bits = static_cast<uint32_t>(ones(static_cast<uint8_t>(c[i] ^ word[i])));
            outer_bits += bits;
            outer_symbols += bits != 0 ? 1 : 0;
            if (i >= 32) extra += bits;
        }
        if (outer_bits > 6 || outer_symbols > 4) continue;
        if (auto speech = checked(c, received, source, 8)) {
            if (speech->corrected_bits + extra <= 12) {
                speech->corrected_bits = static_cast<uint16_t>(speech->corrected_bits + extra);
                return speech;
            }
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<Speech> decode_voice(const std::array<uint8_t, 48>& envelope) {
    const Word word = descramble(envelope);
    const auto received = protected_bits(word);
    const auto inverted = inverse(received);
    const bool is_mode = mode(word);
    if (is_mode)
        if (auto s = checked(word, received, inverted, 8)) return s;
    Word repaired = word;
    if (Galois::get().correct(std::span<uint8_t>(repaired.data() + 28, 14), 6) && mode(repaired) &&
        crc16(std::span<const uint8_t>(repaired.data() + 28, 6)) == static_cast<uint16_t>((repaired[34] << 8) | repaired[35]) && repaired != word) {
        const auto outer = protected_bits(repaired);
        auto accepted = checked(repaired, received, inverse(outer), 8);
        if (!accepted) accepted = checked(repaired, received, viterbi(outer), 8);
        if (accepted) {
            uint16_t extra = 0;
            for (std::size_t i = 32; i < 42; ++i) extra = static_cast<uint16_t>(extra + ones(static_cast<uint8_t>(word[i] ^ repaired[i])));
            accepted->corrected_bits = static_cast<uint16_t>(accepted->corrected_bits + extra);
            return accepted;
        }
    }
    Input decoded{};
    if (is_mode) {
        decoded = viterbi(received);
        if (auto s = checked(word, received, decoded, 8)) return s;
    }
    if (ones(static_cast<uint8_t>(word[32] ^ 0x80)) + ones(static_cast<uint8_t>(word[33] & 0xfe)) > 2) return std::nullopt;
    if (auto s = joint(word, received, inverted)) return s;
    if (!is_mode) decoded = viterbi(received);
    return joint(word, received, decoded);
}

} // namespace spear::jr_train::protocol
