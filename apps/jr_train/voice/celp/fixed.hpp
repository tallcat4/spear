// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 固定小数点の原始演算。原本: src/celp/fixed_core/fixed.rs。
// mcelp-codec(https://github.com/soltia48/mcelp-codec、MIT、celp/LICENSE)の整数 DSP を流用・改変したもの。
//
// 16 bit 固定小数点機の算術: 乗算は小数乗算(積を 1 bit 左シフトして Q15×Q15 → Q31)、累算器は 40 bit で
// 飽和せずに折り返す(飽和は明示した所だけ)、メモリ上の値は符号拡張。40 bit 累算器を int64_t で持ち、acc() で範囲へ畳む。
// シフトは Rust のリリースビルドと同じく 2 の補数で折り返す(uint64_t 経由)。
#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <utility>

namespace spear::jr_train::voice::celp::fixed {

inline int64_t shl(int64_t v, int s) { return static_cast<int64_t>(static_cast<uint64_t>(v) << (s & 63)); }
inline int64_t shr(int64_t v, int s) { return v >> (s & 63); }

// 40 bit 累算器の範囲へ折り返す
inline int64_t acc(int64_t v) { return shr(shl(v, 24), 24); }
// 40 bit → 32 bit に飽和
inline int64_t sat(int64_t v) { return std::clamp<int64_t>(v, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()); }
// 累算器の上位 16 bit
inline int16_t hi(int64_t v) { return static_cast<int16_t>(v >> 16); }
// 累算器の下位 16 bit
inline int16_t low(int64_t v) { return static_cast<int16_t>(v); }
// 小数乗算 Q15 × Q15 → Q31
inline int64_t mul(int16_t x, int16_t y) { return static_cast<int64_t>(x) * static_cast<int64_t>(y) * 2; }
// 符号なし 16 bit × 符号付き
inline int64_t mul_us(int16_t x, int16_t y) { return static_cast<int64_t>(static_cast<uint16_t>(x)) * static_cast<int64_t>(y) * 2; }
// 32 × 16 の小数乗算(上下の半分から組み立てる)
inline int64_t mul32x16(int64_t x, int16_t y) {
    const int64_t partial = acc(mul_us(low(x), y)) >> 16;
    return acc(partial + mul(hi(x), y));
}
// メモリへの格納・読み出しで残る 32 bit だけを保つ
inline int64_t trunc32(int64_t v) { return static_cast<int32_t>(v); }

// 40 bit 累算器の冗長符号ビット数(bit 31 に正規化する左シフト量)。0 なら 0
inline int32_t exp(int64_t v) {
    v = acc(v);
    if (v == 0) return 0;
    const int64_t mag = v < 0 ? ~v : v;
    const int top = 63 - std::countl_zero(static_cast<uint64_t>(mag));
    return 30 - top;
}

// 符号付きの量だけ算術シフト
inline int64_t shift(int64_t v, int32_t s) { return s >= 0 ? acc(shl(v, s)) : acc(shr(v, -s)); }

// 引き戻し除算の 1 段
inline int64_t restoring_step(int64_t num, int16_t den) {
    const int64_t alu = num - shl(static_cast<int64_t>(den), 15);
    return acc(alu >= 0 ? shl(alu, 1) + 1 : shl(num, 1));
}
// 1 bit ずつの引き戻し除算。商は下位 bit に溜まる
inline int64_t restoring_divide(int64_t num, int16_t den, uint32_t steps) {
    for (uint32_t i = 0; i < steps; ++i) num = restoring_step(num, den);
    return num;
}

// 逆数風の除算: value を正規化し、16383 を正規化した大きさで割る。(商, 後で掛ける指数) を返す
inline std::pair<int64_t, int32_t> normalised_reciprocal(int64_t value) {
    const int32_t e = exp(value);
    const int32_t shift_out = e + 1;
    const int64_t normalised = shift(value, e);
    const int64_t magnitude = sat(normalised < 0 ? -normalised : normalised) >> 16;
    int64_t quotient = restoring_divide(shl(16383, 16), static_cast<int16_t>(magnitude), 15);
    // 商は下位 16 bit にあるので上位の半分へ移す
    quotient = acc(shl(static_cast<int64_t>(static_cast<uint64_t>(quotient) & 0xffff), 16));
    if (hi(normalised) < 0) return {acc(-quotient), shift_out};
    return {quotient, shift_out};
}

} // namespace spear::jr_train::voice::celp::fixed
