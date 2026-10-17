// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// jr_dsp の内部部品(テストからも使う)。原本: crates/jr-conventional-dsp/src/{sequence,signal,equalizer}.rs。
// 演算は std::complex の演算子を使わず実部・虚部で書く(原本と同じ順序・同じ丸めにするため)。
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

namespace spear::jr_train::dsp::detail {

struct C32 {
    float re = 0.f, im = 0.f;
};

// Rust f32::total_cmp(a, b).is_lt()
inline bool total_lt(float a, float b) {
    int32_t x, y;
    std::memcpy(&x, &a, 4);
    std::memcpy(&y, &b, 4);
    x ^= static_cast<int32_t>(static_cast<uint32_t>(x >> 31) >> 1);
    y ^= static_cast<int32_t>(static_cast<uint32_t>(y >> 31) >> 1);
    return x < y;
}

// ---- 適応系列推定(sequence.rs)----
// π/4 交番は除去済み。64 状態それぞれが直前 3 シンボルと 4 タップの複素伝送路推定(NLMS)を持つ。
// 枝メトリックは振幅と位相の両方(二乗ユークリッド距離、上限 16)。トレースバック 32。
class Sequence {
public:
    static constexpr std::size_t kStates = 64;
    static constexpr std::size_t kDepth = 32;
    static constexpr float kChannelStep = 0.08f;

    struct Result {
        std::optional<uint8_t> dibit;   // トレースバック後の差動復号 dibit(32 シンボル遅れ)
        float phase_error = 0.f;        // 最良生き残りの予測に対する位相誤差(不確かなら 0)
        float error_power = 1.f;        // 予測誤差の電力(不確かなら 1)
    };

    Sequence();
    Result process(C32 input, bool reliable);
    // 現在の観測から最良生き残りの遅延波と複素利得を除いたもの(判定前の診断値。DSP には戻さない)
    std::optional<C32> equalized() const { return equalized_; }

    // テスト用の内部状態。伝送路は内部では SoA(タップ k × 状態)で持つ
    std::array<float, kStates> costs{};
    std::array<C32, 4> channel(std::size_t state) const;
    void set_channel(std::size_t state, const std::array<C32, 4>& h);

private:
    alignas(32) float hr_[4][kStates], hi_[4][kStates];
    std::array<std::array<uint8_t, kStates>, kDepth> history_{};
    std::size_t write_ = 0, filled_ = 0;
    std::optional<std::size_t> previous_;
    std::optional<C32> equalized_;
};

// ---- 表示用の受動的な観測(signal.rs)----
class SignalView {
public:
    static constexpr std::size_t kCapacity = 1024;
    void push(std::optional<C32> symbol);
    // 新しい count 点(古い順)を RMS 正規化して out に書き、EVM [%] を返す。電力 0 なら count = 0。
    std::size_t read(C32* out, std::size_t capacity, double& evm) const;

private:
    std::optional<C32> previous_;
    std::array<C32, kCapacity> ring_{};
    std::size_t head_ = 0, count_ = 0;
};

// ---- 半シンボル間隔の前段(equalizer.rs)----
// 中央タップ固定(原本の ADAPTATION = 0)。窓と微分からタイミング誤差を出す。
inline constexpr std::size_t kTaps = 21;
class Equalizer {
public:
    Equalizer() { weights_[kTaps / 2].re = 1.f; }
    // 戻り値: (出力, タイミング誤差)
    std::pair<C32, float> process(const std::array<C32, kTaps>& input, const std::array<C32, kTaps>& derivatives, bool reliable);

private:
    std::array<C32, kTaps> weights_{};
};

} // namespace spear::jr_train::dsp::detail
