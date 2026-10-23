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

// 背景雑音 (b0=124, b1=16) はゲイン・スペクトルが変動する。
// 1 枚の既知 12 bit では鍵を確定できないため、異なる位置の 2 枚で解く。
constexpr std::array<int, 12> kBackgroundPositions = {0,1,2,3,37,38,39,4,5,6,7,35};
constexpr std::array<uint8_t, 12> kBackgroundPlain = {1,1,1,1,1,0,0,1,0,0,0,0};
struct BackgroundSolver {
    std::array<Row, 15> basis{};
    std::array<uint16_t, 24> equations{};
};
constexpr auto make_background_solvers() {
    std::array<uint16_t, kPnBits> pn{};
    for (int i = 0; i < 15; ++i) pn[i] = static_cast<uint16_t>(1u << i);
    for (int i = 15; i < kPnBits; ++i) pn[i] = pn[i-15] ^ pn[i-14];
    std::array<std::array<BackgroundSolver, 4>, 4> out{};
    for (int a = 0; a < 4; ++a) for (int b = a+1; b < 4; ++b) {
        auto& s = out[a][b];
        for (int i = 0; i < 24; ++i) {
            s.equations[i] = pn[(i < 12 ? a : b)*49 + kBackgroundPositions[i%12]];
            Row r{s.equations[i], uint32_t{1} << i};
            for (int bit = 14; bit >= 0; --bit) {
                if (!(r.coefficients & (1u << bit))) continue;
                if (!s.basis[bit].coefficients) { s.basis[bit] = r; break; }
                r.coefficients ^= s.basis[bit].coefficients;
                r.observations ^= s.basis[bit].observations;
            }
        }
    }
    return out;
}
constexpr auto kBackgroundSolvers = make_background_solvers();
static_assert([] {
    for (int a = 0; a < 4; ++a) for (int b = a+1; b < 4; ++b)
        for (const auto& row : kBackgroundSolvers[a][b].basis)
            if (!row.coefficients) return false;
    return true;
}(), "Background constraints must have rank 15 for every phase pair");

struct BackgroundResult { std::optional<uint16_t> key; bool ambiguous = false; };
BackgroundResult recover_background_key(std::span<const Burst> bursts, const std::atomic<bool>* cancel) {
    struct Observation { uint32_t bits; int phase; std::size_t burst; };
    std::vector<Observation> observations;
    for (std::size_t bi = 0; bi < bursts.size(); ++bi) for (int phase = 0; phase < 4; ++phase) {
        const auto& f = bursts[bi][phase];
        if (std::any_of(f.begin(), f.end(), [](uint8_t bit) { return bit > 1; })) continue;
        ambe::Bits49 d{};
        for (int i = 0; i < 49; ++i) d[ambe::kThumbDv[i]] = f[i];
        uint32_t bits = 0;
        for (int i = 0; i < 12; ++i)
            bits |= static_cast<uint32_t>(d[kBackgroundPositions[i]] ^ kBackgroundPlain[i]) << i;
        observations.push_back({bits, phase, bi});
    }
    std::vector<uint16_t> candidates;
    for (const auto& a : observations) for (const auto& b : observations) {
        if (a.phase >= b.phase) continue;
        if (cancel && cancel->load(std::memory_order_relaxed)) return {};
        const auto& s = kBackgroundSolvers[a.phase][b.phase];
        const uint32_t bits = a.bits | (b.bits << 12);
        uint16_t key = 0;
        for (int bit = 0; bit < 15; ++bit) {
            const auto& r = s.basis[bit];
            if ((std::popcount(r.observations & bits) ^ std::popcount(static_cast<unsigned>(r.coefficients & key))) & 1)
                key |= static_cast<uint16_t>(1u << bit);
        }
        bool valid = true;
        for (int i = 0; i < 24; ++i)
            if ((std::popcount(static_cast<unsigned>(s.equations[i] & key)) & 1) != static_cast<int>((bits >> i) & 1u)) { valid = false; break; }
        if (valid) candidates.push_back(key);
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    BackgroundResult result;
    for (const auto key : candidates) {
        const auto pn = pn_sequence(key);
        std::array<uint32_t, 4> expected{};
        for (int phase = 0; phase < 4; ++phase) for (int i = 0; i < 12; ++i)
            expected[phase] |= static_cast<uint32_t>(pn[phase*49 + kBackgroundPositions[i]]) << i;
        std::array<unsigned, 4> phase_votes{};
        std::size_t votes = 0, burst_count = 0, last_burst = bursts.size();
        for (const auto& o : observations) if (o.bits == expected[o.phase]) {
            ++votes; ++phase_votes[o.phase];
            if (o.burst != last_burst) { ++burst_count; last_burst = o.burst; }
        }
        // ペアだけでは偶然一致する。3 位置で各 2 枚・2 バースト以上で裏付ける。
        const auto supported_phases = std::count_if(phase_votes.begin(), phase_votes.end(), [](unsigned count) { return count >= 2; });
        if (votes < 6 || supported_phases < 3 || burst_count < 2) continue;
        if (result.key) return {std::nullopt, true};
        result.key = key;
    }
    return result;
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
    const auto background = recover_background_key(bursts, cancel);
    if (cancel && cancel->load(std::memory_order_relaxed)) return std::nullopt;
    if (background.ambiguous || (result && background.key && result != background.key)) return std::nullopt;
    return result ? result : background.key;
}

} // namespace spear::std_t98::secret
