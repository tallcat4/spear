// ARIB STD-T99 Annex 2(デジタル小電力コミュニティ無線)のチャネル表 — 唯一の真値。
// 表 Annex 2-3-1: CH1–9 は 142.934375 MHz から、CH10–18 は 146.934375 MHz から 6.25 kHz 間隔。CH18 は呼出チャネル(4.7)。
// std-t99-receiver-sharp の Call.cs(Channels)と同じ値。
#pragma once

#include <cmath>
#include <optional>

namespace spear::std_t99 {

inline constexpr int kNumChannels = 18;
inline constexpr int kCallingChannel = 18;
inline constexpr double kSpacingHz = 6250;

// ch: 1..18
inline constexpr double channel_freq_hz(int ch) {
    return ch <= 9 ? 142934375.0 + (ch - 1) * kSpacingHz : 146934375.0 + (ch - 10) * kSpacingHz;
}

// 周波数 → チャネル(±500 Hz 以内)
inline std::optional<int> find_channel(double hz) {
    for (int c = 1; c <= kNumChannels; ++c)
        if (std::abs(channel_freq_hz(c) - hz) <= 500) return c;
    return std::nullopt;
}

} // namespace spear::std_t99
