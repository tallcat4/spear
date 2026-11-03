#include "protocol.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace spear::std_t99 {

Symbols quantize(std::span<const float> raw) {
    Symbols s(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const float v = raw[i];
        s[i] = v > 2.f ? 3 : v > 0.f ? 1 : v > -2.f ? -1 : -3;
    }
    return s;
}

namespace {
// PN(9,5) (Figure Annex 2-4-20): output S0, feedback S0 xor S4 into S8, shift toward S0. 1 = invert the symbol.
const std::array<uint8_t, 182>& whitening() {
    static const std::array<uint8_t, 182> w = [] {
        std::array<uint8_t, 9> s = {0, 0, 1, 0, 0, 1, 1, 1, 0};   // index = S0..S8; Table Annex 2-4-9 S8..S0 = 011100100
        std::array<uint8_t, 182> out{};
        for (auto& o : out) {
            o = s[0];
            const uint8_t feedback = s[0] ^ s[4];
            for (int i = 0; i < 8; ++i) s[i] = s[i + 1];
            s[8] = feedback;
        }
        return out;
    }();
    return w;
}

// K=5 trellis, G1 = 1 + D^3 + D^4, G2 = 1 + D + D^2 + D^4 (state bit 3 = previous input)
struct Trellis {
    std::array<std::array<int, 2>, 16> next{}, g1{}, g2{};
    Trellis() {
        for (int state = 0; state < 16; ++state)
            for (int bit = 0; bit < 2; ++bit) {
                const int d1 = (state >> 3) & 1, d2 = (state >> 2) & 1, d3 = (state >> 1) & 1, d4 = state & 1;
                next[state][bit] = (bit << 3) | (state >> 1);
                g1[state][bit] = bit ^ d3 ^ d4;
                g2[state][bit] = bit ^ d1 ^ d2 ^ d4;
            }
    }
};
const Trellis& trellis() { static const Trellis t; return t; }

// Hard-decision Viterbi. depunctured holds 0/1, or -1 for an erased (punctured) bit.
// The last 4 steps are the fixed "0" tail bits, so the path ends in state 0.
struct Decoded { Bits bits; int cost; };
Decoded viterbi(const std::vector<int>& depunctured, int steps) {
    const auto& tr = trellis();
    constexpr int inf = std::numeric_limits<int>::max() / 2;
    std::array<int, 16> cost;
    cost.fill(inf);
    cost[0] = 0;
    std::array<Bits, 16> history;
    for (int step = 0; step < steps; ++step) {
        const int r1 = depunctured[static_cast<std::size_t>(step) * 2];
        const int r2 = depunctured[static_cast<std::size_t>(step) * 2 + 1];
        std::array<int, 16> next_cost;
        next_cost.fill(inf);
        std::array<Bits, 16> next_history;
        const int max_bit = step < steps - 4 ? 2 : 1;
        for (int state = 0; state < 16; ++state) {
            if (cost[state] >= inf) continue;
            for (int bit = 0; bit < max_bit; ++bit) {
                const int to = tr.next[state][bit];
                const int c = cost[state] + (r1 != -1 && r1 != tr.g1[state][bit]) + (r2 != -1 && r2 != tr.g2[state][bit]);
                if (c < next_cost[to]) {
                    next_cost[to] = c;
                    next_history[to] = history[state];
                    next_history[to].push_back(static_cast<uint8_t>(bit));
                }
            }
        }
        cost = next_cost;
        history = std::move(next_history);
    }
    return {history[0], cost[0]};
}

// MSB-first CRC with all-ones initial register (Figures Annex 2-4-6 / 2-4-9).
// taps: register positions that receive the feedback in addition to S0. Output S(n-1) .. S0.
template <std::size_t N>
std::array<uint8_t, N> crc(std::span<const uint8_t> bits, std::initializer_list<std::size_t> taps) {
    std::array<uint8_t, N> reg;
    reg.fill(1);
    for (uint8_t bit : bits) {
        const uint8_t feedback = bit ^ reg[N - 1];
        for (std::size_t i = N - 1; i > 0; --i) reg[i] = reg[i - 1];
        reg[0] = feedback;
        for (std::size_t t : taps) reg[t] ^= feedback;
    }
    std::array<uint8_t, N> out;
    for (std::size_t i = 0; i < N; ++i) out[i] = reg[N - 1 - i];
    return out;
}
} // namespace

Symbols dewhiten(const Symbols& symbols) {
    Symbols out = symbols;
    if (out.size() < kFrameSymbols) return out;
    const auto& w = whitening();
    for (std::size_t i = 0; i < w.size(); ++i)
        if (w[i]) out[kSyncSymbols + i] = static_cast<int8_t>(-out[kSyncSymbols + i]);
    return out;
}

Bits symbols_to_bits(const Symbols& symbols) {
    Bits bits;
    bits.reserve(symbols.size() * 2);
    for (int8_t s : symbols) {
        const uint8_t v = s == 3 ? 1 : s == 1 ? 0 : s == -1 ? 2 : 3;
        bits.push_back(v >> 1);
        bits.push_back(v & 1);
    }
    return bits;
}

FrameFields split_frame(const Bits& bits) {
    FrameFields f;
    if (bits.size() < 384) return f;
    auto take = [&](std::size_t from, std::size_t n) { return Bits(bits.begin() + static_cast<long>(from), bits.begin() + static_cast<long>(from + n)); };
    f.sw = take(0, 20);
    f.rich = take(20, 16);
    f.sacch = take(36, 60);
    f.first = take(96, 144);
    f.second = take(240, 144);
    return f;
}

Rich decode_rich(const Bits& rich) {
    Rich r;
    if (rich.size() != 16) return r;
    std::array<int, 8> b{};
    for (int i = 0; i < 8; ++i) b[static_cast<std::size_t>(i)] = rich[static_cast<std::size_t>(i) * 2];   // first bit of each di-bit
    r.f = b[0] << 1 | b[1];
    r.m = b[2] << 1 | b[3];
    r.o = b[4] << 1 | b[5];
    r.d = b[6];
    r.parity = b[7];
    r.parity_ok = ((b[0] + b[1] + b[2] + b[3]) & 1) == r.parity;
    return r;
}

Sacch decode_sacch(const Bits& in) {
    Sacch out;
    if (in.size() != 60) return out;
    // Interleave 5 x 12 (Figure Annex 2-4-16): written row-wise, read column-wise
    std::array<int, 60> deinterleaved{};
    for (int column = 0; column < 12; ++column)
        for (int row = 0; row < 5; ++row) deinterleaved[static_cast<std::size_t>(row * 12 + column)] = in[static_cast<std::size_t>(column * 5 + row)];
    // Puncturing 111111/110110: G2 of the 3rd and 6th step of each 6-step period is erased
    std::vector<int> depunctured;
    std::size_t index = 0;
    for (int period = 0; period < 6; ++period)
        for (int step = 0; step < 6; ++step) {
            depunctured.push_back(deinterleaved[index++]);
            depunctured.push_back(step == 2 || step == 5 ? -1 : deinterleaved[index++]);
        }
    auto decoded = viterbi(depunctured, 36);
    out.bit_errors = decoded.cost;
    out.message.assign(decoded.bits.begin(), decoded.bits.begin() + 26);
    const auto check = crc<6>(std::span(out.message), {1, 2, 5});   // X^6 + X^5 + X^2 + X + 1
    out.crc_ok = std::equal(check.begin(), check.end(), decoded.bits.begin() + 26);
    out.structure = static_cast<int>(field(out.message, 0, 2));
    out.group = static_cast<int>(field(out.message, 2, 6));
    out.unit.assign(out.message.begin() + 8, out.message.end());
    return out;
}

Control decode_control(const Bits& in) {
    Control out;
    if (in.size() != 144) return out;
    // Interleave 9 x 16 (Figure Annex 2-4-10)
    std::array<int, 144> deinterleaved{};
    for (int column = 0; column < 16; ++column)
        for (int row = 0; row < 9; ++row) deinterleaved[static_cast<std::size_t>(row * 16 + column)] = in[static_cast<std::size_t>(column * 9 + row)];
    // Puncturing 11/01: G2 of every other step, starting with the first, is erased (4.1.9)
    std::vector<int> depunctured;
    std::size_t index = 0;
    for (int pair = 0; pair < 48; ++pair) {
        depunctured.push_back(deinterleaved[index++]);
        depunctured.push_back(-1);
        depunctured.push_back(deinterleaved[index++]);
        depunctured.push_back(deinterleaved[index++]);
    }
    auto decoded = viterbi(depunctured, 96);
    out.bit_errors = decoded.cost;
    out.message.assign(decoded.bits.begin(), decoded.bits.begin() + 80);
    const auto check = crc<12>(std::span(out.message), {1, 2, 3, 11});   // X^12 + X^11 + X^3 + X^2 + X + 1
    out.crc_ok = std::equal(check.begin(), check.end(), decoded.bits.begin() + 80);
    return out;
}

std::array<uint8_t, 18> traffic_payload(const Bits& half) {
    std::array<uint8_t, 18> out{};
    for (std::size_t i = 0; i < 144 && i < half.size(); ++i)
        if (half[i]) out[i / 8] |= static_cast<uint8_t>(0x80 >> (i % 8));
    return out;
}

std::array<int8_t, 144> traffic_llr(std::span<const float> raw, int half) {
    std::array<int8_t, 144> llr{};
    if (raw.size() < kFrameSymbols || half < 0 || half > 1) return llr;
    const auto& w = whitening();
    auto saturate = [](float v) { return static_cast<int8_t>(std::lround(std::clamp(v * kLlrScale, -127.f, 127.f))); };
    const int first = 48 + half * 72;
    for (int k = 0; k < 72; ++k) {
        const int s = first + k;
        const float y = w[static_cast<std::size_t>(s - kSyncSymbols)] ? -raw[static_cast<std::size_t>(s)] : raw[static_cast<std::size_t>(s)];
        // +1 -> 00, +3 -> 01, -1 -> 10, -3 -> 11: first bit = sign, second bit = outer level (max-log)
        llr[static_cast<std::size_t>(2 * k)] = saturate(-y);
        llr[static_cast<std::size_t>(2 * k + 1)] = saturate(std::fabs(y) - 2.f);
    }
    return llr;
}

uint64_t field(const Bits& bits, std::size_t first, std::size_t count) {
    uint64_t v = 0;
    for (std::size_t i = first; i < first + count && i < bits.size(); ++i) v = v << 1 | bits[i];
    return v;
}

std::string to_hex(const Bits& bits) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    for (std::size_t i = 0; i < bits.size(); i += 4) {
        unsigned v = 0;
        for (std::size_t j = 0; j < 4; ++j) v = v << 1 | (i + j < bits.size() ? bits[i + j] : 0);
        out.push_back(digits[v]);
    }
    return out;
}

uint32_t crc32_mpeg2(std::span<const uint8_t> bytes) {
    uint32_t reg = 0xffffffffu;
    for (uint8_t b : bytes) {
        reg ^= static_cast<uint32_t>(b) << 24;
        for (int i = 0; i < 8; ++i) reg = reg & 0x80000000u ? (reg << 1) ^ 0x04c11db7u : reg << 1;
    }
    return reg;
}

} // namespace spear::std_t99
