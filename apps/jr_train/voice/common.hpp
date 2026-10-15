// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// jr-voice 移植の共通部品: Rust の浮動小数・整数の意味論を C++ で再現する小さな関数。
// 原本と同じ PCM を出すため、演算の丸めと NaN の扱いを Rust に合わせる(-ffp-contract=off でビルドする)。
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace spear::jr_train::voice {

// 原本の Err(()) に相当。Stream / Voice の境界で捕まえて「失敗」として扱う
struct Failure {};

namespace rs {

// f32::clamp / f64::clamp(NaN はそのまま)
template <class T>
inline T clamp(T x, T lo, T hi) {
    if (x < lo) x = lo;
    if (x > hi) x = hi;
    return x;
}
// f32::max / f32::min(llvm.maxnum / minnum: 片方が NaN なら他方)
inline float max(float a, float b) { return std::fmax(a, b); }
inline float min(float a, float b) { return std::fmin(a, b); }
inline double max(double a, double b) { return std::fmax(a, b); }
inline double min(double a, double b) { return std::fmin(a, b); }

// f32::round_ties_even(既定の丸めモード)
inline float round_ties_even(float x) { return std::nearbyint(x); }
inline double round_ties_even(double x) { return std::nearbyint(x); }

// `x as T`(浮動小数 → 整数): 範囲外は飽和、NaN は 0、小数は 0 方向へ切り捨て
template <class T, class F>
inline T as(F x) {
    static_assert(std::is_integral_v<T> && std::is_floating_point_v<F>);
    if (std::isnan(x)) return 0;
    constexpr F lo = static_cast<F>(std::numeric_limits<T>::min());
    constexpr F hi = static_cast<F>(std::numeric_limits<T>::max());
    if (x <= lo) return std::numeric_limits<T>::min();
    if (x >= hi) return std::numeric_limits<T>::max();
    return static_cast<T>(x);
}

// f32::total_cmp(a, b).is_lt()
inline bool total_lt(float a, float b) {
    int32_t x, y;
    std::memcpy(&x, &a, 4);
    std::memcpy(&y, &b, 4);
    x ^= static_cast<int32_t>(static_cast<uint32_t>(x >> 31) >> 1);
    y ^= static_cast<int32_t>(static_cast<uint32_t>(y >> 31) >> 1);
    return x < y;
}
inline bool total_lt(double a, double b) {
    int64_t x, y;
    std::memcpy(&x, &a, 8);
    std::memcpy(&y, &b, 8);
    x ^= static_cast<int64_t>(static_cast<uint64_t>(x >> 63) >> 1);
    y ^= static_cast<int64_t>(static_cast<uint64_t>(y >> 63) >> 1);
    return x < y;
}

// 変数の底・定数の指数の powi は LLVM が二乗の積に展開する(x^3 = x·x², x^4 = x²·x²)
template <class T>
inline T powi(T x, int n) {
    T r = 1;
    bool have = false;
    T square = x;
    while (n) {
        if (n & 1) {
            r = have ? r * square : square;
            have = true;
        }
        n >>= 1;
        if (n) square = square * square;
    }
    return have ? r : T(1);
}
// 定数の底・定数の指数の powi は LLVM の定数畳み込みで std::pow(double, int) になる
inline double powi_const(double x, int n) { return std::pow(x, static_cast<double>(n)); }

// 浮動小数のスライスの左からの和(Iterator::sum の順)
template <class It>
inline auto sum(It first, It last) {
    using T = std::decay_t<decltype(*first)>;
    T s = T(-0.0);
    for (; first != last; ++first) s += *first;
    return s;
}

} // namespace rs
} // namespace spear::jr_train::voice
