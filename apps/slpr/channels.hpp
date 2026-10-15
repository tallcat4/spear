// 特定小電力トランシーバー(422 MHz 帯、単信)のチャネル表 — このファイルが唯一の真値。
// 出典: 各社の対応表(ICOM IC / STANDARD FTH・PK・CL・HX / MOTOROLA MS50・MS80・CL08 / ALINCO DJ / KENWOOD UBZ / UNIDEN / Wedge Talkie)。
// 呼び方は ICOM 準拠(01–20 の通し番号)。単信 11ch(422.0500–422.1750)と単信 9ch(422.2000–422.3000)は 12.5 kHz 間隔で、
// 両群の間(422.1875)だけ 1 スロット空く。中継用(440 MHz 帯)は扱わない。
#pragma once

#include <array>
#include <cstddef>

namespace spear::slpr {

constexpr double kSpacingHz = 12500;
constexpr int    kNumChannels = 20;
// 帯域中心 = ch11(422.1750 MHz)。ch01 が −125 kHz、ch20 が +125 kHz で、20 ch が ±125 kHz に収まる
constexpr double kBandCenterHz = 422.175e6;

struct ChannelDef {
    int         number;     // ICOM の通し番号 1..20
    const char* name;       // 画面表示 "01".."20"
    double      freq_hz;
    int         grid;       // 帯域中心からのオフセット / 12.5 kHz(PFB の bin)
    const char* group;      // "SIMPLEX 11CH" / "SIMPLEX 9CH"
};

constexpr const char* kGroup11 = "SIMPLEX 11CH";
constexpr const char* kGroup9  = "SIMPLEX 9CH";

inline constexpr std::array<ChannelDef, kNumChannels> kChannels = {{
    { 1, "01", 422.0500e6, -10, kGroup11},
    { 2, "02", 422.0625e6,  -9, kGroup11},
    { 3, "03", 422.0750e6,  -8, kGroup11},
    { 4, "04", 422.0875e6,  -7, kGroup11},
    { 5, "05", 422.1000e6,  -6, kGroup11},
    { 6, "06", 422.1125e6,  -5, kGroup11},
    { 7, "07", 422.1250e6,  -4, kGroup11},
    { 8, "08", 422.1375e6,  -3, kGroup11},
    { 9, "09", 422.1500e6,  -2, kGroup11},
    {10, "10", 422.1625e6,  -1, kGroup11},
    {11, "11", 422.1750e6,   0, kGroup11},
    {12, "12", 422.2000e6,   2, kGroup9},
    {13, "13", 422.2125e6,   3, kGroup9},
    {14, "14", 422.2250e6,   4, kGroup9},
    {15, "15", 422.2375e6,   5, kGroup9},
    {16, "16", 422.2500e6,   6, kGroup9},
    {17, "17", 422.2625e6,   7, kGroup9},
    {18, "18", 422.2750e6,   8, kGroup9},
    {19, "19", 422.2875e6,   9, kGroup9},
    {20, "20", 422.3000e6,  10, kGroup9},
}};

constexpr double channel_offset_hz(int i) { return kChannels[static_cast<std::size_t>(i)].grid * kSpacingHz; }

} // namespace spear::slpr
