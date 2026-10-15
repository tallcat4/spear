// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 整数 CELP 復号(励振・スペクトル・利得の状態)。原本: src/celp/{mod,bits,post_fec,gain,spectral,core}.rs と fixed_core/*。
// mcelp-codec(https://github.com/soltia48/mcelp-codec、MIT。著作権者は原本と同じ。celp/LICENSE)の整数 DSP と表を流用・改変し、
// Modified RL-CELP のパラメータ復号と利得・スペクトルの状態管理に組み込んだもの。
#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace spear::jr_train::voice::celp {

inline constexpr std::size_t kSubframe = 80;
inline constexpr std::size_t kOrder = 10;

// ---- bits.rs ----
struct HalfParams {
    std::array<std::optional<uint8_t>, 2> gain_low{};
    std::array<uint16_t, 2> code{};
    std::array<uint8_t, 2> gain_high{};
    uint8_t mode = 0, stage1 = 0, stage2_low = 0, stage2_high = 0, absolute_pitch = 0, relative_pitch = 0;
    std::size_t gain_index(std::size_t sub) const;
};
struct FrameParams {
    std::array<HalfParams, 2> halves;
};
FrameParams unpack_bits(const std::array<uint8_t, 158>& input);
// post_fec.rs: 受理したペイロードを CELP コアのパラメータ配置に並べ替える(上位 2 bit が 0 でなければ nullopt)
std::optional<std::array<uint8_t, 158>> receiver_bits(const std::array<uint8_t, 20>& payload);

// ---- pitch.rs ----
struct Lag {
    int16_t integer = 0;   // 20..=143
    int16_t frac = 0;      // 1/3 sample 単位
};
Lag decode_absolute(int16_t code);
Lag decode_relative(int16_t code, int16_t prev);

// ---- gain.rs ----
struct Gains {
    int16_t pitch = 0, code = 0;
};
struct GainState {
    std::array<float, 4> history{};
    Gains decode(std::size_t index, const std::array<int16_t, 80>& code);
};

// ---- spectral.rs ----
struct Spectral {
    std::array<std::array<float, 10>, 3> history;
    Spectral();
    std::array<int16_t, 10> decode(const HalfParams& p);
};

// ---- postfilter.rs ----
struct OutputFilter {
    std::array<int16_t, 6> fir{};
    std::array<int64_t, 6> iir{};
    void run(int16_t* block, std::size_t n);

private:
    int16_t filter_section(std::size_t section, int16_t value);
    int16_t filter_sample(int16_t value);
};

// ---- core.rs ----
struct SubframeTrace {
    Lag lag;
    std::size_t code_class = 0;
    std::size_t gain_index = 0;
    Gains gains;
    std::array<float, 4> gain_history{};
    std::array<int16_t, 80> innovation{};
    std::array<int16_t, 80> mixed_excitation{};
};
struct Core {
    std::array<int16_t, 10> previous_lsp{};
    std::array<int16_t, 314> excitation{};
    std::array<int16_t, 10> synthesis_memory{};
    int16_t sharpen_fixed = 3277, sharpen_voiced = 3277, last_half_lag = 60, previous_lag = 60;
    OutputFilter output_filter;
    Core();
    // 半フレーム(160 sample)の PCM と 2 サブフレームの観測
    std::array<int16_t, 160> decode(const std::array<int16_t, 10>& lsp, const HalfParams& params, GainState& gain, std::array<SubframeTrace, 2>& traces);
};

// ---- mod.rs ----
struct Observation {
    std::array<int16_t, 320> excitation{};
    std::array<int16_t, 320> pcm{};
    std::array<std::array<float, 12>, 4> gain{};
};
class Native {
public:
    Native();
    // 受理できない(上位 2 bit が 0 でない)ペイロードは Failure を投げる
    Observation decode(const std::array<uint8_t, 20>& payload);

private:
    Core core_;
    Spectral spectrum_;
    GainState gain_;
};

} // namespace spear::jr_train::voice::celp
