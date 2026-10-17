#include "silence.hpp"
#include "ambe/bits.hpp"

#include <algorithm>
#include <bit>
#include <initializer_list>
#include <vector>

namespace spear::std_t98::secret {
namespace {

// b0=119, b1=16, b2=0, b6=14 give rank 14 (21 bits).
// b5 in {2,8} also fixes d[26]=0, raising the rank to 15 at every phase.
constexpr std::array<int, 22> kPositions = {
    0, 1, 2, 3, 37, 38, 39, 4, 5, 6, 7, 35, 8, 9, 10, 11, 36, 28, 29, 30, 45, 26};
constexpr std::array<uint8_t, 22> kPlain = {
    1, 1, 1, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0};

struct Row { uint16_t coefficients = 0; uint32_t observations = 0; };
struct Solver {
    std::array<Row, 15> basis{};
    std::array<uint16_t, 22> equations{};
};

constexpr auto make_solvers() {
    // pn[t+15] = pn[t] XOR pn[t+1]; first 15 outputs are the key bits.
    std::array<uint16_t, kPnBits> pn{};
    for (int i = 0; i < 15; ++i) pn[i] = static_cast<uint16_t>(1u << i);
    for (int i = 15; i < kPnBits; ++i) pn[i] = pn[i - 15] ^ pn[i - 14];
    std::array<Solver, kFramesPerBurst> solvers{};
    for (int phase = 0; phase < kFramesPerBurst; ++phase) {
        auto& s = solvers[phase];
        for (std::size_t i = 0; i < kPositions.size(); ++i) {
            s.equations[i] = pn[phase * kBitsPerFrame + kPositions[i]];
            Row r{s.equations[i], uint32_t{1} << i};
            for (int b = 14; b >= 0; --b) {
                if (!(r.coefficients & (1u << b))) continue;
                if (!s.basis[b].coefficients) { s.basis[b] = r; break; }
                r.coefficients ^= s.basis[b].coefficients;
                r.observations ^= s.basis[b].observations;
            }
        }
    }
    return solvers;
}
constexpr auto kSolvers = make_solvers();
static_assert([] {
    for (const auto& s : kSolvers)
        for (const auto& r : s.basis) if (!r.coefficients) return false;
    return true;
}(), "Silence constraints must have rank 15 at all four phases");

unsigned field(const ambe::Bits49& d, std::initializer_list<int> positions) {
    unsigned value = 0;
    for (int i : positions) value = (value << 1) | d[i];
    return value;
}
bool one_of(unsigned value, std::initializer_list<unsigned> values) {
    return std::find(values.begin(), values.end(), value) != values.end();
}
bool matches_silence(const ambe::Bits49& d) {
    // Validate the entire profile, including bits not used to solve the key.
    // Do not use PCM amplitude: an incorrect key can decode to silence too.
    return field(d, {0,1,2,3,37,38,39}) == 119 &&
           field(d, {4,5,6,7,35}) == 16 &&
           field(d, {8,9,10,11,36}) == 0 &&
           one_of(field(d, {12,13,14,15,16,17,18,19,43}), {86,87,90,91,308}) &&
           one_of(field(d, {20,21,22,23,40,41,42}), {27,36,78}) &&
           one_of(field(d, {24,25,26,27,44}), {2,8}) &&
           field(d, {28,29,30,45}) == 14 &&
           one_of(field(d, {31,32,33,46}), {1,3,13}) &&
           one_of(field(d, {34,47,48}), {1,3,5,7});
}

} // namespace

std::optional<uint16_t> silence_key_candidate(const Frame49& frame, int phase) {
    if (phase < 0 || phase >= kFramesPerBurst) return std::nullopt;
    ambe::Bits49 d{};
    for (int i = 0; i < kBitsPerFrame; ++i) {
        if (frame[i] > 1) return std::nullopt;
        d[ambe::kThumbDv[i]] = frame[i];
    }
    uint32_t observations = 0;
    for (std::size_t i = 0; i < kPositions.size(); ++i)
        observations |= static_cast<uint32_t>(d[kPositions[i]] ^ kPlain[i]) << i;
    const auto& s = kSolvers[phase];
    uint16_t key = 0;
    for (int b = 0; b < 15; ++b) {
        const auto& r = s.basis[b];
        const int rhs = std::popcount(r.observations & observations) & 1;
        const int known = std::popcount(static_cast<unsigned>(r.coefficients & key)) & 1;
        if (rhs != known) key |= static_cast<uint16_t>(1u << b);
    }
    // The seven dependent equations must also hold; never silently fit errors.
    for (std::size_t i = 0; i < kPositions.size(); ++i)
        if (static_cast<uint32_t>(std::popcount(static_cast<unsigned>(s.equations[i] & key)) & 1) !=
            ((observations >> i) & 1u)) return std::nullopt;
    const auto pn = pn_sequence(key);
    for (int i = 0; i < kBitsPerFrame; ++i) d[i] ^= pn[phase * kBitsPerFrame + i];
    if (!matches_silence(d)) return std::nullopt;
    return key;
}

std::optional<uint16_t> recover_silence_key(std::span<const Burst> bursts, const std::atomic<bool>* cancel) {
    struct Evidence { uint16_t key; unsigned phases; };
    std::vector<Evidence> evidence;
    for (const auto& burst : bursts) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return std::nullopt;
        for (int phase = 0; phase < kFramesPerBurst; ++phase) {
            const auto key = silence_key_candidate(burst[phase], phase);
            if (!key) continue;
            auto it = std::find_if(evidence.begin(), evidence.end(), [&](const Evidence& e) { return e.key == *key; });
            if (it == evidence.end()) evidence.push_back({*key, 1u << phase});
            else it->phases |= 1u << phase;
        }
    }
    std::optional<uint16_t> result;
    for (const auto& e : evidence) {
        if (std::popcount(e.phases) < 2) continue;
        if (result) return std::nullopt;   // conflicting supported keys, including plaintext
        result = e.key;
    }
    return result;
}

} // namespace spear::std_t98::secret
