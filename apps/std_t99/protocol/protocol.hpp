// ARIB STD-T99 Annex 2 (digital low-power community radio) physical-layer decoding.
// Frame: SW(20) RICH(16) SACCH(60) + two 144-bit halves = 384 bit = 192 four-level symbols, 80 ms.
// The halves are PICH1/PICH2 in a synchronous burst, TCH or FACCH in a communication channel.
// Channel coding is the same family as the STD-T98 frames handled by std-t98-receiver:
// K=5 R=1/2 convolutional code, the same puncturing, 6/12-bit CRC and PN(9,5) whitening.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace spear::std_t99 {

using Symbols = std::vector<int8_t>;   // -3, -1, +1, +3
using Bits = std::vector<uint8_t>;     // 0 / 1, transmission order

inline constexpr int kFrameSymbols = 192;
inline constexpr int kSyncSymbols = 10;

// Soft symbols (±1/±3 scale) -> nearest level
Symbols quantize(std::span<const float> raw);
// Annex 2 4.1.10: PN(9,5), S8..S0 = 011100100, applied to the 182 symbols after SW. Self-inverse.
Symbols dewhiten(const Symbols& symbols);
// Table Annex 2-3-2: 01 -> +3, 00 -> +1, 10 -> -1, 11 -> -3
Bits symbols_to_bits(const Symbols& symbols);

struct FrameFields {
    Bits sw, rich, sacch, first, second;   // 20, 16, 60, 144, 144
};
FrameFields split_frame(const Bits& bits384);

// ---- RICH (Figure Annex 2-4-4/2-4-12): 8 information bits, one per di-bit (+3/+1 -> 0, -1/-3 -> 1) ----
struct Rich {
    int f = 0;        // 01 synchronous burst, 10 communication channel
    int m = 0;        // 01 SACCH non-super frame, 10 super frame
    int o = 0;        // halves: 00 FACCH/FACCH (PICH1/PICH2 in SB), 01 FACCH/TCH, 10 TCH/FACCH, 11 TCH/TCH
    int d = 0;        // reserved
    int parity = 0;
    bool parity_ok = false;   // even parity over the 4 MSB bits (F, M) as written in 4.1.8.4
};
Rich decode_rich(const Bits& rich16);

// ---- SACCH (60 bit): 26 information bits = W(8: structure 2 + group 6) + 18-bit unit, CRC-6 ----
struct Sacch {
    Bits message;          // 26 decoded bits
    bool crc_ok = false;
    int bit_errors = 0;    // Viterbi path metric (hard decision)
    int structure = 0;     // 11, 10, 01, 00 = super frame unit 1..4; 00 also = single-frame unit
    int group = 0;
    Bits unit;             // 18 bits
};
Sacch decode_sacch(const Bits& sacch60);

// ---- PICH / FACCH (144 bit): 80 information bits, CRC-12 ----
struct Control {
    Bits message;          // 80 decoded bits
    bool crc_ok = false;
    int bit_errors = 0;
};
Control decode_control(const Bits& half144);

// ---- TCH: 144 bits = two 72-bit voice frames (Chapter 5: 48 voice + 24 FEC each), carried unchanged ----
std::array<uint8_t, 18> traffic_payload(const Bits& half144);   // 2 x 9 bytes, MSB first
// Soft decisions of one half (0 = first, 1 = second) from the 192 unquantized symbols.
// Same convention as std-t98-receiver .llr: positive = 1, magnitude = confidence.
std::array<int8_t, 144> traffic_llr(std::span<const float> raw192, int half);
inline constexpr float kLlrScale = 32.f;

uint64_t field(const Bits& bits, std::size_t first, std::size_t count);   // MSB-first unsigned
std::string to_hex(const Bits& bits);                                      // left-aligned, zero padded
// CRC-32/MPEG-2 (poly 04C11DB7, init FFFFFFFF, no reflection, no final XOR).
// Matches the caller-name check bits of the Annex 2 7.7.2 test signals.
uint32_t crc32_mpeg2(std::span<const uint8_t> bytes);

} // namespace spear::std_t99
