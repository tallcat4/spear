// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 原本: src/celp/{mod,bits,post_fec,gain,spectral,core}.rs、src/celp/fixed_core/{lsp,pitch,codebook,excitation,synth,postfilter}.rs。
// mcelp-codec(https://github.com/soltia48/mcelp-codec、MIT、celp/LICENSE)の整数 DSP と表を流用・改変したもの。
// 整数演算は原本(Rust のリリースビルド)と同じく 2 の補数で折り返す。i16 の加減算は int で計算して int16_t に戻す。
#include "celp/celp.hpp"

#include "celp/fixed.hpp"
#include "celp/tables.hpp"
#include "common.hpp"
#include "models.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <tuple>

namespace spear::jr_train::voice::celp {

using namespace fixed;
namespace T = tables;

namespace {

inline int16_t i16(int v) { return static_cast<int16_t>(v); }

// ================= lsp.rs =================
// LSF(Q13 ラジアン)→ LSP(Q15 の cos)。表引き + 線形補間
std::array<int16_t, kOrder> lsf_to_lsp(const std::array<int16_t, kOrder>& lsf) {
    std::array<int16_t, kOrder> lsp{};
    for (std::size_t i = 0; i < kOrder; ++i) {
        // 20861(Q15)を小数乗算で倍にすると 4/π: Q13 ラジアンを 6.8 固定小数点の表 index へ
        const int64_t scaled = mul(lsf[i], 20861);
        const auto index = static_cast<std::size_t>(static_cast<int64_t>(hi(std::min<int64_t>(shift(scaled, -8), shl(63, 16)))));
        const int64_t frac = static_cast<int16_t>(hi(scaled) & 0xff);
        const int64_t interp = acc(shl(static_cast<int64_t>(T::COS[index]), 16) + shl(frac, 3) * static_cast<int64_t>(T::COS_SLOPE[index]) * 2);
        lsp[i] = hi(interp);
    }
    return lsp;
}

// 5 つの LSP(1 つおき)からの対称 / 反対称多項式 f[0..=5](Q24)
std::array<int64_t, 6> lsp_polynomial(const std::array<int16_t, kOrder>& lsp, std::size_t first) {
    std::array<int64_t, 6> f{};
    f[0] = shl(1, 24);
    f[1] = trunc32(-shl(static_cast<int64_t>(lsp[first]), 10));
    for (std::size_t i = 2; i <= 5; ++i) {
        const int16_t l = i16(-static_cast<int>(lsp[first + 2 * (i - 1)]));
        f[i] = trunc32(acc(2 * f[i - 2] + shl(mul32x16(f[i - 1], l), 1)));
        for (std::size_t j = i - 1; j >= 2; --j) f[j] = trunc32(acc(f[j] + shl(mul32x16(f[j - 1], l), 1) + f[j - 2]));
        f[1] = trunc32(acc(f[1] + shl(static_cast<int64_t>(l), 10)));
    }
    return f;
}

// LSP → 直接形 LPC(Q12、a[0] = 4096)
std::array<int16_t, kOrder + 1> lsp_to_lpc(const std::array<int16_t, kOrder>& lsp) {
    auto f1 = lsp_polynomial(lsp, 0);
    auto f2 = lsp_polynomial(lsp, 1);
    for (std::size_t i = 5; i >= 1; --i) {
        f1[i] = trunc32(acc(f1[i] + f1[i - 1]));
        f2[i] = trunc32(acc(f2[i] - f2[i - 1]));
    }
    std::array<int16_t, kOrder + 1> a{};
    a[0] = 4096;
    for (std::size_t i = 1; i <= 5; ++i) {
        a[i] = hi(acc(shift(shift(acc(f1[i] + f2[i]), -1), 4) + shl(1, 15)));
        a[kOrder + 1 - i] = hi(acc(shift(shift(acc(f1[i] - f2[i]), -1), 4) + shl(1, 15)));
    }
    return a;
}

// 2 つの LSP の中点
std::array<int16_t, kOrder> midpoint(const std::array<int16_t, kOrder>& first, const std::array<int16_t, kOrder>& second) {
    std::array<int16_t, kOrder> out{};
    for (std::size_t i = 0; i < kOrder; ++i) out[i] = hi(acc(shl(static_cast<int64_t>(first[i]) + static_cast<int64_t>(second[i]), 15)));
    return out;
}

// LPC → 反射係数(Levinson を逆向きに)
std::array<int16_t, kOrder> reflection_coefficients(const std::array<int16_t, kOrder + 1>& lpc) {
    std::array<int16_t, kOrder> k{};
    std::copy(lpc.begin() + 1, lpc.end(), k.begin());
    for (std::size_t step = 0; step < kOrder - 1; ++step) {
        const std::size_t order = kOrder - step;
        const std::size_t top = order - 1;
        const int16_t kv = k[top];
        // 1 − k²(Q30)とその逆数
        const int64_t residual = acc(shl(16384, 16) - shift(acc(static_cast<int64_t>(kv) * static_cast<int64_t>(kv) * 2), 5));
        const auto [quotient, exponent] = normalised_reciprocal(residual);
        const int32_t scale = static_cast<int32_t>(static_cast<uint32_t>(exponent) << 27) >> 27;   // 符号付き 5 bit のシフト量
        const int16_t inverse = hi(shift(quotient, -1));
        const std::size_t pairs = ((order - 2) >> 1) + 1;
        for (std::size_t p = 0; p < pairs; ++p) {
            const std::size_t i = p, j = top - 1 - p;
            const int64_t lo_ = acc(shl(static_cast<int64_t>(k[i]), 16) - shift(acc(mul(k[j], kv)), 3));
            const int16_t updated_low = hi(shift(mul32x16(sat(lo_), inverse), scale));
            const int64_t hi_ = acc(shl(static_cast<int64_t>(k[j]), 16) - shift(acc(mul(kv, k[i])), 3));
            const int16_t updated_high = hi(shift(mul32x16(sat(hi_), inverse), scale));
            k[i] = updated_low;
            k[j] = updated_high;
        }
    }
    return k;
}

// ================= pitch.rs =================
constexpr std::size_t kInterpTaps = 10;
constexpr int16_t kResolution = 3;

struct Filters {
    const int16_t* forward;
    const int16_t* backward;
};
Filters interpolation_filters(std::size_t phase) {
    const std::size_t mirrored = static_cast<std::size_t>(kResolution) - phase;
    return {T::PITCH_INTERP.data() + phase, T::PITCH_INTERP.data() + mirrored};
}
int64_t interpolated_sample(const int16_t* signal, std::size_t centre, const Filters& f) {
    int64_t value = 0;
    for (std::size_t k = 0; k < kInterpTaps; ++k) value = acc(value + mul(signal[centre - k], f.forward[3 * k]));
    for (std::size_t k = 0; k < kInterpTaps; ++k) value = acc(value + mul(signal[centre + 1 + k], f.backward[3 * k]));
    return value;
}

void predict(int16_t* exc, std::size_t at, const Lag& lag) {
    // フィルタは「符号を反転した」小数部で引く。負の小数部は整数遅延に繰り入れる
    int phase = -static_cast<int>(lag.frac);
    std::size_t delay = static_cast<std::size_t>(static_cast<int64_t>(lag.integer));
    if (phase < 0) {
        phase += kResolution;
        delay += 1;
    }
    const auto f = interpolation_filters(static_cast<std::size_t>(phase));
    for (std::size_t n = 0; n < kSubframe; ++n) {
        const std::size_t base = at + n - delay;
        const int64_t value = interpolated_sample(exc, base, f);
        exc[at + n] = hi(sat(acc(value + 0x8000)));
    }
}

// 固定符号帳ベクトルの周期強調(ラグがサブフレームより短いときだけ)
void sharpen(std::array<int16_t, kSubframe>& code, const Lag& lag, int16_t gain) {
    int32_t first = static_cast<int32_t>(i16(lag.integer - static_cast<int>(kInterpTaps)));
    int phase = -static_cast<int>(lag.frac);
    if (phase < 0) {
        phase += kResolution;
        first += 1;
    }
    if (first >= static_cast<int32_t>(kSubframe)) return;
    const auto ufirst = static_cast<std::size_t>(first);
    constexpr std::size_t kGuard = 2 * kInterpTaps + 1;
    std::array<int16_t, kGuard + kSubframe> buf{};
    std::copy(code.begin(), code.begin() + static_cast<std::ptrdiff_t>(ufirst), buf.begin() + kGuard);
    const auto f = interpolation_filters(static_cast<std::size_t>(phase));
    for (std::size_t n = ufirst; n < kSubframe; ++n) {
        const std::size_t centre = kGuard + (n - ufirst) - kInterpTaps;
        const int64_t value = interpolated_sample(buf.data(), centre, f);
        const int64_t scaled = mul32x16(acc(value), gain);
        const int64_t mixed = acc(shift(scaled, 1) + shl(static_cast<int64_t>(code[n]), 16) + shl(1, 15));
        code[n] = hi(mixed);
        buf[kGuard + n] = code[n];
    }
}

// ================= codebook.rs =================
constexpr std::size_t kClasses = 6;
constexpr std::size_t kAltClass = 5;
constexpr std::size_t kAltPulses = 5;
constexpr int16_t kAltAmplitude = 8192;
constexpr std::size_t kShapeLen = 20;
constexpr std::size_t kPositions = 40;
constexpr std::size_t kMaxPulses = 3;

struct Innovation {
    std::array<int16_t, kSubframe> code{};
    std::size_t cls = 0;
};

std::pair<std::size_t, uint16_t> classify(uint16_t index) {
    index = std::min<uint16_t>(index, static_cast<uint16_t>(T::FCB_INDEX_LIMIT[0] - 1));
    std::size_t cls = 0;
    for (std::size_t k = kClasses; k-- > 0;)
        if (index >= T::FCB_CLASS_BASE[k]) {
            cls = k;
            break;
        }
    return {cls, index};
}

void decompose(std::size_t cls, uint16_t index, std::size_t pulses, std::array<int16_t, kMaxPulses>& positions, std::array<int16_t, kMaxPulses>& signs) {
    int64_t rest = static_cast<int64_t>(index) - static_cast<int64_t>(T::FCB_CLASS_BASE[cls]);
    signs.fill(0);
    for (std::size_t s = 0; s < pulses; ++s) {
        signs[s] = static_cast<int16_t>(rest & 1);
        rest = shift(rest, -1);
    }
    positions.fill(0);
    int64_t value = shl(rest, 16);
    for (std::size_t k = pulses; k-- > 0;) {
        const int16_t recip = T::FCB_RADIX_INV[kMaxPulses * cls + k];
        const int16_t radix = T::FCB_RADIX[kMaxPulses * cls + k];
        const int16_t quotient = hi(shift(acc(static_cast<int64_t>(hi(value)) * static_cast<int64_t>(recip) * 2), -2));
        value = acc(value - shl(static_cast<int64_t>(quotient) * static_cast<int64_t>(radix) * 2, 15));
        positions[k] = hi(value);
        value = shl(static_cast<int64_t>(quotient), 16);
    }
}

std::array<int16_t, kSubframe> place_shapes(std::size_t cls, const std::array<int16_t, kMaxPulses>& positions, const std::array<int16_t, kMaxPulses>& signs,
                                            std::size_t pulses) {
    const auto first_shape = static_cast<std::size_t>(T::FCB_SHAPE_SEL[kMaxPulses * cls]);
    std::array<int16_t, kSubframe> code{};
    for (std::size_t k = 0; k < pulses; ++k) {
        const auto slot = static_cast<std::size_t>(static_cast<int64_t>(positions[k]));
        const auto pos = static_cast<std::size_t>(T::FCB_POSITIONS.at(kPositions * (kMaxPulses * cls + k) + slot));
        const int16_t* shape = T::FCB_SHAPES.data() + kShapeLen * (first_shape + k);
        for (std::size_t n = pos; n < std::min(kSubframe, pos + kShapeLen); ++n) {
            const int64_t x = shl(static_cast<int64_t>(code[n]), 16);
            const int64_t y = shl(static_cast<int64_t>(shape[n - pos]), 16);
            code[n] = hi(signs[k] != 0 ? acc(x + y) : acc(x - y));
        }
    }
    return code;
}

std::array<int16_t, kSubframe> alternative(uint16_t index) {
    const int64_t rest = static_cast<int64_t>(index) - static_cast<int64_t>(T::FCB_CLASS_BASE[kAltClass]);
    const int64_t upper = shift(rest, -7);
    const int64_t lower = rest - shift(upper, 7);
    std::array<int16_t, kSubframe> code{};
    const std::pair<const int16_t*, std::size_t> parts[2] = {{T::FCB_ALT_A.data(), static_cast<std::size_t>(upper)}, {T::FCB_ALT_B.data(), static_cast<std::size_t>(lower)}};
    for (const auto& [base, offset] : parts) {
        for (std::size_t k = 0; k < kAltPulses; ++k) {
            const int16_t entry = base[kAltPulses * offset + k];
            // 位置は 1 始まり。符号がパルスの極性
            const std::size_t pos = static_cast<std::size_t>(std::abs(static_cast<int>(entry))) - 1;
            const int64_t x = shl(static_cast<int64_t>(code.at(pos)), 16);
            const int64_t amp = shl(static_cast<int64_t>(kAltAmplitude), 16);
            code[pos] = hi(entry <= 0 ? acc(x - amp) : acc(x + amp));
        }
    }
    return code;
}

Innovation codebook_decode(uint16_t index) {
    const auto [cls, idx] = classify(index);
    Innovation r;
    r.cls = cls;
    if (cls == kAltClass) {
        r.code = alternative(idx);
        return r;
    }
    const auto pulses = static_cast<std::size_t>(T::FCB_PULSES[cls]);
    std::array<int16_t, kMaxPulses> positions{}, signs{};
    decompose(cls, idx, pulses, positions, signs);
    r.code = place_shapes(cls, positions, signs, pulses);
    return r;
}

// ================= excitation.rs(固定符号帳の強調利得)=================
constexpr int64_t kHalf = 16384;
constexpr int64_t kWasStrong = 13107;
constexpr int64_t kSamePitch = 24576;

std::pair<int16_t, int16_t> interpolation_gains(int16_t reflection, int16_t previous_gain, int16_t lag, int16_t previous_lag) {
    const bool peaky = acc(shift(shl(static_cast<int64_t>(reflection), 16), 3) - shl(kHalf, 16)) > 0;
    const bool strong = acc(shift(shl(static_cast<int64_t>(previous_gain), 16), 1) - shl(kWasStrong, 16)) > 0;
    const bool above_floor = acc(shl(static_cast<int64_t>(lag), 16) - acc(kSamePitch * static_cast<int64_t>(previous_lag) * 2)) > 0;
    const bool below_ceiling = acc(shift(acc(static_cast<int64_t>(previous_lag) * kSamePitch * 2), 1) - shl(static_cast<int64_t>(lag), 16)) > 0;
    const bool continuous = above_floor && below_ceiling;
    // 尖ったスペクトルだけが 2 つ目の利得を消す。他の条件を満たさなければ前の値を保つ
    const int64_t second = peaky ? 0 : (strong && continuous ? kHalf : static_cast<int64_t>(previous_gain));
    return {low(kHalf), low(second)};
}

// ================= synth.rs =================
void synthesis(const std::array<int16_t, kOrder + 1>& a, const int16_t* input, int16_t* output, std::size_t n_in, std::array<int16_t, kOrder>& mem) {
    std::array<int16_t, kOrder + kSubframe> window{};
    std::copy(mem.begin(), mem.end(), window.begin());
    for (std::size_t n = 0; n < n_in; ++n) {
        int64_t sum = 0;
        for (std::size_t i = 1; i <= kOrder; ++i) sum = acc(sum - mul(a[i], window[n + kOrder - i]));
        const int16_t y = hi(sat(acc(shift(sum, 3) + shl(static_cast<int64_t>(input[n]), 16) + shl(1, 15))));
        window[kOrder + n] = y;
        output[n] = y;
    }
    std::copy(window.begin() + static_cast<std::ptrdiff_t>(n_in), window.begin() + static_cast<std::ptrdiff_t>(n_in + kOrder), mem.begin());
}

// ================= spectral.rs =================
std::array<float, 10> residual(const HalfParams& p) {
    std::array<float, 10> r{};
    for (std::size_t k = 0; k < 10; ++k)
        r[k] = static_cast<float>(T::LSF_CB1[static_cast<std::size_t>(p.stage1) * 10 + k]) +
               (k < 5 ? celp_config::low(p.stage2_low, k) : celp_config::high(p.stage2_high, k - 5));
    return r;
}
std::array<float, 10> predict_lsf(const std::array<std::array<float, 10>, 3>& history, const std::array<float, 10>& res, uint8_t mode) {
    // SSE の乗算・加算の順に合わせる: 現在、履歴 0、1、2
    std::array<float, 10> out{};
    const std::size_t m = mode;
    for (std::size_t k = 0; k < 10; ++k)
        out[k] = ((res[k] * celp_config::ma(m, 0, k) + history[0][k] * celp_config::ma(m, 1, k)) + history[1][k] * celp_config::ma(m, 2, k)) +
                 history[2][k] * celp_config::ma(m, 3, k);
    std::stable_sort(out.begin(), out.end(), [](float x, float y) { return rs::total_lt(x, y); });
    out[0] = rs::max(out[0], 41.f);
    for (std::size_t k = 1; k < 10; ++k) out[k] = rs::max(out[k], out[k - 1] + 321.f);
    out[9] = rs::min(out[9], 25682.f);
    return out;
}
std::array<int16_t, 10> to_lsp(const std::array<float, 10>& lsf) {
    // 偶数丸め → 単調にクランプ(LSF_GAP ではない)
    int16_t prior = 0;
    std::array<int16_t, 10> q{};
    for (std::size_t i = 0; i < 10; ++i) {
        prior = std::clamp<int16_t>(std::max<int16_t>(rs::as<int16_t>(rs::round_ties_even(lsf[i])), prior), 0, 25682);
        q[i] = prior;
    }
    return lsf_to_lsp(q);
}

int16_t quantize_gain(float x) { return rs::as<int16_t>(rs::clamp(rs::round_ties_even(x), 0.f, 32767.f)); }

uint16_t read_bits(std::initializer_list<uint8_t> values) {
    uint16_t v = 0;
    for (uint8_t b : values) v = static_cast<uint16_t>(2 * v + (b & 1));
    return v;
}

} // namespace

// ================= bits.rs =================
std::size_t HalfParams::gain_index(std::size_t sub) const {
    const auto g = gain_low[sub] ? gain_low[sub] : gain_low[0];
    return static_cast<std::size_t>(gain_high[sub] & 7) * 16 + static_cast<std::size_t>(g.value_or(0) & 15);
}

FrameParams unpack_bits(const std::array<uint8_t, 158>& input) {
    FrameParams fp;
    for (std::size_t h = 0; h < 2; ++h) {
        auto q = [&](std::size_t k) { return input[84 + 39 * h + k]; };
        auto a = [&](std::size_t k) { return input[20 * h + k]; };
        auto b = [&](std::size_t k) { return input[40 + 20 * h + k]; };
        auto range = [&](std::size_t from, std::size_t to) {
            uint16_t v = 0;
            for (std::size_t k = from; k < to; ++k) v = static_cast<uint16_t>(2 * v + (q(k) & 1));
            return v;
        };
        auto code = [&](std::size_t k) {
            return read_bits({q(k == 0 ? 18 : 30), q(k == 0 ? 19 : 31), b(7 + k), b(5 + k), a(6 + k), a(4 + k), b(6 + k), b(4 + k), a(11 + k), a(9 + k), b(11 + k), b(9 + k),
                              a(10 + k), a(8 + k), b(10 + k), b(8 + k)});
        };
        HalfParams& p = fp.halves[h];
        p.gain_low[0] = static_cast<uint8_t>(range(23, 27));
        p.gain_low[1] = h == 0 ? std::optional<uint8_t>(static_cast<uint8_t>(range(35, 39))) : std::nullopt;
        p.code = {code(0), code(8)};
        p.gain_high = {static_cast<uint8_t>(range(20, 23)), static_cast<uint8_t>(range(32, 35))};
        p.mode = q(0) & 1;
        p.stage1 = static_cast<uint8_t>(range(1, 8));
        p.stage2_low = static_cast<uint8_t>(read_bits({q(8), q(9), q(10), q(11), a(3), a(1)}));
        p.stage2_high = static_cast<uint8_t>(read_bits({b(3), b(1), a(2), a(0), b(2), b(0)}));
        p.absolute_pitch = static_cast<uint8_t>(read_bits({q(12), q(13), q(14), q(15), q(16), q(17), a(7), a(5)}));
        p.relative_pitch = static_cast<uint8_t>(read_bits({q(27), q(28), q(29), a(15), a(13)}));
    }
    return fp;
}

// ================= post_fec.rs =================
std::optional<std::array<uint8_t, 158>> receiver_bits(const std::array<uint8_t, 20>& payload) {
    if (payload[19] & 0xc0) return std::nullopt;
    auto bit = [&](std::size_t i) { return static_cast<uint8_t>((payload[i / 8] >> (i % 8)) & 1); };
    std::array<uint8_t, 158> out{};
    for (std::size_t i = 0; i < 80; ++i) out[i] = bit(78 + i);
    for (std::size_t i : {1, 5, 7}) out[i] ^= 1;
    for (std::size_t i = 0; i < 74; ++i) out[84 + i] = bit(i);
    return out;
}

// ================= pitch.rs =================
Lag decode_absolute(int16_t code) {
    if (code >= 197) return {i16(code - 112), 0};   // 範囲の上端は整数分解能だけ
    const int16_t quotient = low(restoring_divide(static_cast<int64_t>(code) + 2, kResolution, 16));
    const int16_t integer = i16(quotient + 19);
    const int16_t frac = i16(code - low(mul(integer, kResolution) >> 1) + 58);
    return {integer, frac};
}

Lag decode_relative(int16_t code, int16_t prev) {
    int16_t mn = std::max<int16_t>(i16(prev - 5), 20);
    if (mn + 9 > 143) mn = 134;
    const int16_t quotient = i16(low(restoring_divide(static_cast<int64_t>(code) + 2, kResolution, 16)) - 1);
    return {i16(quotient + mn), i16(code - 2 - low(mul(quotient, kResolution) >> 1))};
}

// ================= gain.rs =================
Gains GainState::decode(std::size_t index, const std::array<int16_t, 80>& code) {
    int64_t e = 0;
    for (int16_t c : code) e += static_cast<int64_t>(c) * static_cast<int64_t>(c);
    const float energy = static_cast<float>(e) / 80.f;
    const float prediction = ((((history[0] * 0.68f + history[1] * 0.5f) + history[2] * 0.33f) + history[3] * 0.16f) - 0.5f * std::log(rs::max(energy, 1.f))) +
                             celp_config::gain_bias();
    const float correction = celp_config::log_gain(index);
    const Gains g{quantize_gain(celp_config::pitch_gain(index)), quantize_gain(std::exp(prediction + correction))};
    std::rotate(history.rbegin(), history.rbegin() + 1, history.rend());
    history[0] = correction;
    return g;
}

// ================= spectral.rs =================
Spectral::Spectral() {
    std::array<float, 10> mean{};
    for (std::size_t i = 0; i < 10; ++i) mean[i] = static_cast<float>(T::LSF_MEAN[i]);
    history = {mean, mean, mean};
}

std::array<int16_t, 10> Spectral::decode(const HalfParams& p) {
    const auto r = residual(p);
    const auto lsf = predict_lsf(history, r, p.mode);
    history[2] = history[1];
    history[1] = history[0];
    history[0] = r;
    return to_lsp(lsf);
}

// ================= postfilter.rs =================
int16_t OutputFilter::filter_section(std::size_t section, int16_t value) {
    const auto& ff = T::OUT_FIR;
    const auto& fb = T::OUT_IIR;
    int16_t* fr = fir.data() + section * 3;
    int64_t* ir = iir.data() + section * 3;
    int64_t b = acc(static_cast<int64_t>(value) * static_cast<int64_t>(ff[0]) * 2);
    for (std::size_t k = 0; k < 3; ++k) b = acc(b + mul(fr[k], ff[k + 1]));
    fr[2] = fr[1];
    fr[1] = fr[0];
    fr[0] = value;
    for (std::size_t k = 0; k < 3; ++k) b = acc(b - shift(mul32x16(ir[k], fb[k]), 1));
    ir[2] = ir[1];
    ir[1] = ir[0];
    ir[0] = trunc32(shift(b, 1));
    return hi(sat(acc(shift(b, 2) + shl(1, 15))));
}

int16_t OutputFilter::filter_sample(int16_t value) {
    constexpr int16_t kOutScale = 19661;   // 最終出力の尺度(Q15)
    for (std::size_t section = 0; section < 2; ++section) value = filter_section(section, value);
    const int64_t scaled = acc(static_cast<int64_t>(value) * static_cast<int64_t>(kOutScale) * 2);
    return hi(sat(acc(shift(scaled, 1) + shl(1, 15))));
}

void OutputFilter::run(int16_t* block, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) block[i] = filter_sample(block[i]);
}

// ================= core.rs =================
Core::Core() { std::copy(T::LSP_MEAN.begin(), T::LSP_MEAN.end(), previous_lsp.begin()); }

std::array<int16_t, 160> Core::decode(const std::array<int16_t, 10>& lsp, const HalfParams& params, GainState& gain, std::array<SubframeTrace, 2>& traces) {
    const auto mid = midpoint(previous_lsp, lsp);
    const auto lpc0 = lsp_to_lpc(mid);
    const auto lpc1 = lsp_to_lpc(lsp);
    previous_lsp = lsp;
    std::array<int16_t, 160> out{};
    for (std::size_t sub = 0; sub < 2; ++sub) {
        const auto& lpc = sub == 0 ? lpc0 : lpc1;
        const Lag lag = sub == 0 ? decode_absolute(static_cast<int16_t>(params.absolute_pitch)) : decode_relative(static_cast<int16_t>(params.relative_pitch), previous_lag);
        previous_lag = lag.integer;
        const std::size_t at = 154 + 80 * sub;
        predict(excitation.data(), at, lag);
        const auto reflection = reflection_coefficients(lpc);
        std::tie(sharpen_fixed, sharpen_voiced) = interpolation_gains(reflection[0], sharpen_fixed, lag.integer, last_half_lag);
        const auto innovation = codebook_decode(params.code[sub]);
        auto code = innovation.code;
        if (lag.integer < 80) {
            const int16_t g = (innovation.cls == 5 || (innovation.cls >= 1 && innovation.cls < 3)) ? sharpen_voiced : sharpen_fixed;
            sharpen(code, lag, g);
        }
        const auto gain_history = gain.history;
        const Gains gains = gain.decode(params.gain_index(sub), code);
        sharpen_fixed = std::clamp<int16_t>(gains.pitch, 3277, 13017);
        for (std::size_t n = 0; n < 80; ++n) {
            const int64_t adaptive = mul(excitation[at + n], gains.pitch);
            const int64_t fx = mul(code[n], gains.code);
            excitation[at + n] = hi(sat(acc(shift(acc(adaptive + shift(fx, 1)), 1) + 0x8000)));
        }
        synthesis(lpc, excitation.data() + at, out.data() + 80 * sub, 80, synthesis_memory);
        SubframeTrace& t = traces[sub];
        t.lag = lag;
        t.code_class = innovation.cls;
        t.gain_index = params.gain_index(sub);
        t.gains = gains;
        t.gain_history = gain_history;
        t.innovation = code;
        std::copy(excitation.begin() + static_cast<std::ptrdiff_t>(at), excitation.begin() + static_cast<std::ptrdiff_t>(at + 80), t.mixed_excitation.begin());
    }
    std::copy(excitation.begin() + 160, excitation.end(), excitation.begin());
    last_half_lag = previous_lag;
    output_filter.run(out.data(), out.size());
    return out;
}

// ================= mod.rs =================
Native::Native() {
    float initial = 0.f;
    for (std::size_t i = 0; i < 128; ++i) initial += celp_config::log_gain(i);
    gain_.history.fill(initial / 128.f);
}

Observation Native::decode(const std::array<uint8_t, 20>& payload) {
    const auto bits = receiver_bits(payload);
    if (!bits) throw Failure{};
    FrameParams params = unpack_bits(*bits);
    uint8_t lo = 0;
    for (std::size_t k = 74; k < 78; ++k) lo = static_cast<uint8_t>(2 * lo + ((payload[k / 8] >> (k % 8)) & 1));
    // 原本の注記: 次の 2 つの解釈は経験的に確かめたモデル上の仮定で、ビット仕様を完全に復元したものではない
    params.halves[1].gain_low[1] = lo;
    params.halves[0].absolute_pitch ^= 3;
    Observation result;
    for (std::size_t h = 0; h < 2; ++h) {
        const auto lsp = spectrum_.decode(params.halves[h]);
        std::array<SubframeTrace, 2> traces;
        const auto pcm = core_.decode(lsp, params.halves[h], gain_, traces);
        std::copy(pcm.begin(), pcm.end(), result.pcm.begin() + static_cast<std::ptrdiff_t>(h * 160));
        for (std::size_t s = 0; s < 2; ++s) {
            const auto& t = traces[s];
            std::copy(t.mixed_excitation.begin(), t.mixed_excitation.end(), result.excitation.begin() + static_cast<std::ptrdiff_t>(h * 160 + s * 80));
            const std::size_t index = t.gain_index;
            const auto& v = t.gain_history;
            const float correction = celp_config::log_gain(index);
            const float amplitude = ((((v[0] * 0.68f + v[1] * 0.5f) + v[2] * 0.33f) + v[3] * 0.16f) + celp_config::gain_bias()) + correction;
            int64_t e = 0;
            for (int16_t x : t.innovation) e += static_cast<int64_t>(x) * static_cast<int64_t>(x);
            const float energy = static_cast<float>(e) / 80.f;
            result.gain[h * 2 + s] = {static_cast<float>(index), correction, v[0], v[1], v[2], v[3], amplitude, energy, static_cast<float>(t.gains.code),
                                      static_cast<float>(t.gains.pitch), static_cast<float>(t.lag.integer), static_cast<float>(t.code_class)};
        }
    }
    return result;
}

} // namespace spear::jr_train::voice::celp
