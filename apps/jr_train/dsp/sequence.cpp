// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 原本: crates/jr-conventional-dsp/src/{sequence,signal,equalizer}.rs
#include "detail.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <numbers>

namespace spear::jr_train::dsp::detail {
namespace {

constexpr float kLevel = std::numbers::sqrt2_v<float> / 2.f;   // Rust FRAC_1_SQRT_2(f32 で同じ値)
constexpr std::array<C32, 4> kSymbols = {C32{kLevel, kLevel}, C32{-kLevel, kLevel}, C32{-kLevel, -kLevel}, C32{kLevel, -kLevel}};
constexpr std::array<uint8_t, 4> kGray = {0, 1, 3, 2};

static_assert(kLevel == 0.70710677f);

} // namespace

Sequence::Sequence() {
    for (std::size_t k = 0; k < 4; ++k)
        for (std::size_t state = 0; state < kStates; ++state) {
            hr_[k][state] = k == 0 ? 1.f : 0.f;
            hi_[k][state] = 0.f;
        }
}

std::array<C32, 4> Sequence::channel(std::size_t state) const {
    std::array<C32, 4> h;
    for (std::size_t k = 0; k < 4; ++k) h[k] = C32{hr_[k][state], hi_[k][state]};
    return h;
}

void Sequence::set_channel(std::size_t state, const std::array<C32, 4>& h) {
    for (std::size_t k = 0; k < 4; ++k) {
        hr_[k][state] = h[k].re;
        hi_[k][state] = h[k].im;
    }
}

// ---- 枝の計算と生き残りの選択・伝送路の更新(64 状態をまたいで 8 レーンずつ)----
// 各レーンの演算(項の順序・丸め)は原本の 1 状態分のループと同じ。レーンをまたぐ算術はしない(並べ替えだけ)ので、
// SIMD 幅や命令セット(AVX2 / SSE2 / NEON)に依らず結果はビット単位で原本と同じ。FMA には縮約しない(CMake の -ffp-contract=off)。
namespace {

using V = float __attribute__((vector_size(32)));
using M = int32_t __attribute__((vector_size(32)));
constexpr std::size_t kLanes = 8;
constexpr std::size_t kStates = Sequence::kStates;

inline V load(const float* p) { V v; std::memcpy(&v, p, sizeof v); return v; }
inline M load(const int32_t* p) { M v; std::memcpy(&v, p, sizeof v); return v; }
inline void store(float* p, V v) { std::memcpy(p, &v, sizeof v); }
inline V splat(float x) {   // 0 + x にしない(−0 を +0 に変えない)
    V v;
    for (std::size_t l = 0; l < kLanes; ++l) v[l] = x;
    return v;
}
inline V select(M m, V a, V b) { return (V)(((M)a & m) | ((M)b & ~m)); }
inline M select(M m, M a, M b) { return (a & m) | (b & ~m); }

// 4 本(current = 0..3、レーン = s)を状態の順(current + 4 s)に並べて 32 個書く
inline void interleave(const V (&v)[4], float* out) {
    const V t0 = __builtin_shufflevector(v[0], v[1], 0, 8, 1, 9, 4, 12, 5, 13);
    const V t1 = __builtin_shufflevector(v[0], v[1], 2, 10, 3, 11, 6, 14, 7, 15);
    const V t2 = __builtin_shufflevector(v[2], v[3], 0, 8, 1, 9, 4, 12, 5, 13);
    const V t3 = __builtin_shufflevector(v[2], v[3], 2, 10, 3, 11, 6, 14, 7, 15);
    const V u0 = __builtin_shufflevector(t0, t2, 0, 1, 8, 9, 4, 5, 12, 13);
    const V u1 = __builtin_shufflevector(t0, t2, 2, 3, 10, 11, 6, 7, 14, 15);
    const V u2 = __builtin_shufflevector(t1, t3, 0, 1, 8, 9, 4, 5, 12, 13);
    const V u3 = __builtin_shufflevector(t1, t3, 2, 3, 10, 11, 6, 7, 14, 15);
    store(out, __builtin_shufflevector(u0, u1, 0, 1, 2, 3, 8, 9, 10, 11));
    store(out + 8, __builtin_shufflevector(u2, u3, 0, 1, 2, 3, 8, 9, 10, 11));
    store(out + 16, __builtin_shufflevector(u0, u1, 4, 5, 6, 7, 12, 13, 14, 15));
    store(out + 24, __builtin_shufflevector(u2, u3, 4, 5, 6, 7, 12, 13, 14, 15));
}

// 状態 / 直前の状態 x について、遅延 k のシンボル。k=1: x % 4、k=2: (x / 4) % 4、k=3: x / 16
struct SymbolTable {
    alignas(32) float re[4][kStates], im[4][kStates];
    alignas(32) int32_t index[kStates];
    SymbolTable() {
        for (std::size_t x = 0; x < kStates; ++x) {
            const std::size_t sym[4] = {0, x % 4, (x / 4) % 4, x / 16};
            for (std::size_t k = 1; k < 4; ++k) {
                re[k][x] = kSymbols[sym[k]].re;
                im[k][x] = kSymbols[sym[k]].im;
            }
            index[x] = static_cast<int32_t>(x);
        }
    }
};
const SymbolTable kTable;

struct Trellis {
    alignas(32) float hr[4][kStates], hi[4][kStates];                     // 更新前の伝送路(直前の状態の順)
    alignas(32) float cost[4][kStates], pr[4][kStates], pi[4][kStates];   // [current][prior]
    alignas(32) float next_costs[kStates], pred_re[kStates], pred_im[kStates];   // 状態の順
    alignas(32) int32_t parents[kStates];
};

#if defined(__x86_64__) && defined(__GNUC__) && !defined(__clang__) && !defined(__SANITIZE_THREAD__) && !defined(__SANITIZE_ADDRESS__)
#define JR_SEQUENCE_CLONES __attribute__((target_clones("avx2", "default")))
#else
#define JR_SEQUENCE_CLONES
#endif

// 原本の 2 つのループを 1 つにしたもの。
//   (1) (prior, current) の二重ループ: 予測、枝メトリック、各状態で最初に見つかった最小(prior の昇順、厳密な <)
//   (2) 更新ループ: 各状態が勝った直前の状態の伝送路を受け継ぎ、NLMS で更新して、ノルムが 4 を超えたら縮める
// 状態 current + 4 s の候補は prior = s, s+16, s+32, s+48 なので、レーンを s にとると候補は連続した 8 個になり、
// 勝者の伝送路・シンボルも同じマスクで選べる(gather が要らない)。どの候補も勝たなければ原本どおり prior 0 のまま。
// 新しい伝送路は hr / hi(状態の順)に書く。読むのは t.hr / t.hi(更新前の写し)だけ。
JR_SEQUENCE_CLONES
void trellis(Trellis& t, const float* costs, float (*hr_out)[kStates], float (*hi_out)[kStates], float in_re, float in_im, bool reliable) {
    const V zero{}, sixteen = splat(16.f), four = splat(4.f), step = splat(Sequence::kChannelStep), ir = splat(in_re), ii = splat(in_im);
    for (std::size_t b = 0; b < kStates; b += kLanes) {
        const V hr0 = load(&t.hr[0][b]), hi0 = load(&t.hi[0][b]);
        V tr[4], ti[4];
        for (std::size_t k = 1; k < 4; ++k) {   // 遅延 1〜3 の項は current に依らない(同じ式なので同じ値)
            const V hr = load(&t.hr[k][b]), hi = load(&t.hi[k][b]);
            const V sr = load(&kTable.re[k][b]), si = load(&kTable.im[k][b]);
            tr[k] = hr * sr - hi * si;
            ti[k] = hr * si + hi * sr;
        }
        const V c0 = load(&costs[b]);
        for (std::size_t current = 0; current < 4; ++current) {
            const V sr = splat(kSymbols[current].re), si = splat(kSymbols[current].im);
            tr[0] = hr0 * sr - hi0 * si;
            ti[0] = hr0 * si + hi0 * sr;
            V pr = zero, pi = zero;
            for (std::size_t k = 0; k < 4; ++k) {
                pr += tr[k];
                pi += ti[k];
            }
            V error = zero;
            if (reliable) {
                const V dr = ir - pr, di = ii - pi;
                const V e = dr * dr + di * di;
                error = select(e < sixteen, e, sixteen);   // fmin(e, 16)(e が NaN なら 16)
            }
            store(&t.cost[current][b], c0 + error);
            store(&t.pr[current][b], pr);
            store(&t.pi[current][b], pi);
        }
    }
    const V inf = splat(std::numeric_limits<float>::infinity());
    for (std::size_t h = 0; h < 16; h += kLanes) {   // 状態 4h .. 4h+31
        V best[4], bpr[4], bpi[4], parent[4], hr[4][4], hi[4][4];
        for (std::size_t current = 0; current < 4; ++current) {
            V cost = inf, pr = zero, pi = zero;
            M par{};
            V sr[4], si[4];
            for (std::size_t k = 0; k < 4; ++k) {
                hr[k][current] = splat(t.hr[k][0]);
                hi[k][current] = splat(t.hi[k][0]);
                sr[k] = splat(kTable.re[k][0]);
                si[k] = splat(kTable.im[k][0]);
            }
            for (std::size_t j = 0; j < 4; ++j) {
                const std::size_t o = 16 * j + h;
                const V c = load(&t.cost[current][o]);
                const M m = c < cost;
                cost = select(m, c, cost);
                pr = select(m, load(&t.pr[current][o]), pr);
                pi = select(m, load(&t.pi[current][o]), pi);
                par = select(m, load(&kTable.index[o]), par);
                for (std::size_t k = 0; k < 4; ++k) {
                    hr[k][current] = select(m, load(&t.hr[k][o]), hr[k][current]);
                    hi[k][current] = select(m, load(&t.hi[k][o]), hi[k][current]);
                    if (k > 0) {
                        sr[k] = select(m, load(&kTable.re[k][o]), sr[k]);
                        si[k] = select(m, load(&kTable.im[k][o]), si[k]);
                    }
                }
            }
            sr[0] = splat(kSymbols[current].re);
            si[0] = splat(kSymbols[current].im);
            if (reliable) {
                const V er = ir - pr, ei = ii - pi;
                for (std::size_t k = 0; k < 4; ++k) {
                    hr[k][current] += step * (er * sr[k] + ei * si[k]);
                    hi[k][current] += step * (ei * sr[k] - er * si[k]);
                }
                V norm{};
                for (std::size_t k = 0; k < 4; ++k) norm += hr[k][current] * hr[k][current] + hi[k][current] * hi[k][current];
                const M m = norm > sixteen;
                V root;
                for (std::size_t l = 0; l < kLanes; ++l) root[l] = std::sqrt(norm[l]);
                const V gain = four / root;
                for (std::size_t k = 0; k < 4; ++k) {
                    hr[k][current] = select(m, hr[k][current] * gain, hr[k][current]);
                    hi[k][current] = select(m, hi[k][current] * gain, hi[k][current]);
                }
            }
            best[current] = cost;
            bpr[current] = pr;
            bpi[current] = pi;
            parent[current] = (V)par;
        }
        const std::size_t first = 4 * h;
        interleave(best, t.next_costs + first);
        interleave(bpr, t.pred_re + first);
        interleave(bpi, t.pred_im + first);
        interleave(parent, reinterpret_cast<float*>(t.parents + first));   // ビット列の並べ替えだけ
        for (std::size_t k = 0; k < 4; ++k) {
            interleave(hr[k], hr_out[k] + first);
            interleave(hi[k], hi_out[k] + first);
        }
    }
}

} // namespace

Sequence::Result Sequence::process(C32 input, bool reliable) {
    Trellis t;
    std::memcpy(t.hr, hr_, sizeof t.hr);
    std::memcpy(t.hi, hi_, sizeof t.hi);
    trellis(t, costs.data(), hr_, hi_, input.re, input.im, reliable);   // hr_ / hi_ はここで更新後の値になる
    std::array<uint8_t, kStates> parents;
    for (std::size_t state = 0; state < kStates; ++state) parents[state] = static_cast<uint8_t>(t.parents[state]);
    const float* next_costs = t.next_costs;
    // 同値なら最初の最小(原本 total_cmp)
    std::size_t best = 0;
    for (std::size_t state = 1; state < kStates; ++state)
        if (total_lt(next_costs[state], next_costs[best])) best = state;
    const C32 bp{t.pred_re[best], t.pred_im[best]};
    const C32 error{input.re - bp.re, input.im - bp.im};
    const float phase = std::atan2(input.im * bp.re - input.re * bp.im, input.re * bp.re + input.im * bp.im);

    // 診断: 現在の最良生き残りの遅延波を観測から引き、主タップで割る。適応の前に行う(今の誤差を学習で消さない)
    {
        const std::size_t prior = parents[best];
        std::array<C32, 4> channel;   // 更新前の伝送路
        for (std::size_t k = 0; k < 4; ++k) channel[k] = C32{t.hr[k][prior], t.hi[k][prior]};
        const C32 main = channel[0];
        const float power = main.re * main.re + main.im * main.im;
        equalized_.reset();
        if (reliable && std::isfinite(power) && power > 1e-6f) {
            C32 value = input;
            const std::array<C32, 3> past = {kSymbols[prior % 4], kSymbols[(prior / 4) % 4], kSymbols[prior / 16]};
            for (std::size_t k = 0; k < 3; ++k) {
                const C32 h = channel[k + 1], s = past[k];
                value.re -= h.re * s.re - h.im * s.im;
                value.im -= h.re * s.im + h.im * s.re;
            }
            const C32 out{(value.re * main.re + value.im * main.im) / power, (value.im * main.re - value.re * main.im) / power};
            if (std::isfinite(out.re) && std::isfinite(out.im)) equalized_ = out;
        }
    }

    // 共通オフセットを引いて、無限長の入力でも累積コストを有界に保つ
    const float base = next_costs[best];
    for (std::size_t state = 0; state < kStates; ++state) costs[state] = next_costs[state] - base;
    history_[write_] = parents;
    write_ = (write_ + 1) % kDepth;
    filled_ = std::min(filled_ + 1, kDepth);

    Result r;
    if (filled_ == kDepth) {
        std::size_t state = best;
        for (std::size_t back = 0; back < kDepth - 1; ++back) state = history_[(write_ + kDepth - 1 - back) % kDepth][state];
        const std::size_t current = state % 4;
        r.dibit = previous_ ? kGray[(current + 4 - *previous_) % 4] : uint8_t{0};
        previous_ = current;
    }
    r.phase_error = reliable ? phase : 0.f;
    r.error_power = reliable ? error.re * error.re + error.im * error.im : 1.f;
    return r;
}

// ---- SignalView ----

void SignalView::push(std::optional<C32> symbol) {
    // π/4 交番を除いた観測に、差動座標でその増分を戻す(4 つの対角の基準点が π/4-DQPSK の意味を保つ)。
    // 振幅・位相のばらつきは残す(判定も点ごとの正規化もしない)。
    C32 value{};
    if (previous_ && symbol) {
        const C32 p = *previous_, s = *symbol;
        const float real = s.re * p.re + s.im * p.im;
        const float imaginary = s.im * p.re - s.re * p.im;
        const float rotation = kLevel;
        value = C32{(real - imaginary) * rotation, (real + imaginary) * rotation};
    }
    // 観測が無ければ表示を古くし、差動の対を切る(フェードをまたいで無関係なシンボルを結ばない)
    previous_ = symbol;
    ring_[head_] = value;
    head_ = (head_ + 1) % kCapacity;
    count_ = std::min(count_ + 1, kCapacity);
}

std::size_t SignalView::read(C32* out, std::size_t capacity, double& evm) const {
    evm = 0.0;
    const std::size_t count = std::min(count_, capacity);
    if (count == 0) return 0;
    const std::size_t start = (head_ + kCapacity - count) % kCapacity;
    double power = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const C32 z = ring_[(start + i) % kCapacity];
        const double re = z.re, im = z.im;
        power += re * re + im * im;
    }
    power /= static_cast<double>(count);
    if (!std::isfinite(power) || power < 1e-12) return 0;
    const double amplitude = std::sqrt(power);
    double squared_error = 0.0;
    constexpr double ideal = std::numbers::sqrt2 / 2.0;
    static_assert(ideal == 0.7071067811865476);
    for (std::size_t i = 0; i < count; ++i) {
        const C32 z = ring_[(start + i) % kCapacity];
        const double re = static_cast<double>(z.re) / amplitude;
        const double im = static_cast<double>(z.im) / amplitude;
        const double dr = std::abs(re) - ideal, di = std::abs(im) - ideal;
        squared_error += dr * dr + di * di;
        out[i] = C32{static_cast<float>(re), static_cast<float>(im)};
    }
    evm = 100.0 * std::sqrt(squared_error / static_cast<double>(count));
    return count;
}

// ---- Equalizer ----

std::pair<C32, float> Equalizer::process(const std::array<C32, kTaps>& input, const std::array<C32, kTaps>& derivatives, bool reliable) {
    constexpr float kAdaptation = 0.f;   // 原本: CMA 適応はフェードの谷で回復を悪くしたので止めてある
    C32 value{}, derivative{};
    float energy = 0.f;
    for (std::size_t k = 0; k < kTaps; ++k) {
        const C32 w = weights_[k], x = input[k], dx = derivatives[k];
        value.re += w.re * x.re - w.im * x.im;
        value.im += w.re * x.im + w.im * x.re;
        derivative.re += w.re * dx.re - w.im * dx.im;
        derivative.im += w.re * dx.im + w.im * dx.re;
        energy += x.re * x.re + x.im * x.im;
    }
    const float power = value.re * value.re + value.im * value.im;
    if (reliable && energy > 0.01f && std::isfinite(energy) && std::isfinite(power)) {
        const float step = kAdaptation * std::clamp(1.f - power, -2.f, 1.f) / energy;
        for (std::size_t k = 0; k < kTaps; ++k) {
            C32& w = weights_[k];
            const C32 x = input[k];
            w.re += step * (value.re * x.re + value.im * x.im);
            w.im += step * (value.im * x.re - value.re * x.im);
        }
        float norm = 0.f;
        for (const C32& w : weights_) norm += w.re * w.re + w.im * w.im;
        if (norm > 16.f) {
            const float gain = 4.f / std::sqrt(norm);
            for (C32& w : weights_) {
                w.re *= gain;
                w.im *= gain;
            }
        }
    }
    // 選んだ波形とその微分からタイミング誤差を出す(ISI は後段の系列推定が扱う)。フェード中は惰性で進む
    float timing = 0.f;
    if (reliable) {
        const float t = 0.5f * (value.re * derivative.re + value.im * derivative.im);
        timing = t < -1.f ? -1.f : (t > 1.f ? 1.f : t);
    }
    return {value, timing};
}

} // namespace spear::jr_train::dsp::detail
